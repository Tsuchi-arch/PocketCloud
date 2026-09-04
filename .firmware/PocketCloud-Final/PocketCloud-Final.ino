#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#define ESP_ASYNC_WEBSERVER_DONT_INCLUDE_ASYNC_TCP
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <SPI.h>
#include <map>
#include <set>

// AP name used by WiFiManager the first time the ESP boots (or if home Wi-Fi fails).
// Connect to this network, open http://192.168.4.1, pick your home Wi-Fi, enter the password.
const char* setup_ap_name = "PocketCloud-Start";

// Pins matching your wiring
#define PIN_CS   4
#define PIN_SCK  5
#define PIN_MOSI 6
#define PIN_MISO 7

// ─── Upload buffer ────────────────────────────────────────────────────────────
// Increasing UPLOAD_BUF_SIZE writes larger blocks to the SD card per flush,
// which improves throughput. Safe range for ESP32-S3: 8192 – 32768 bytes.
// Keep below 32768 to leave headroom for the Wi-Fi stack (~150 KB of SRAM).
#define UPLOAD_BUF_SIZE 16384   // 16 KB  ← change this value to tune chunk size

static uint8_t  uploadBuf[UPLOAD_BUF_SIZE];
static size_t   uploadBufLen = 0;
// ─────────────────────────────────────────────────────────────────────────────

// Accounts
const char* ADMIN_USER = "Admin";
const char* ADMIN_PASS = "pcloud";
const char* TEST_USER  = "test";
const char* TEST_PASS  = "pcloud";
const char* WHITELIST_PATH = "/.web/whitelist.txt";
const char* USERS_PATH    = "/.web/users.txt";      // approved: username:password per line
const char* PENDING_PATH  = "/.web/pending.txt";    // awaiting admin: username:password per line

// Forward declarations for user-database helpers (defined further down).
String findUserPass(const char* path, const String& user);

const char* SESSION_COOKIE = "POCKETCLOUD_SESSION";

AsyncWebServer server(80);
File uploadFile;

// --- Session store (RAM; lost on reboot by design) ---
struct SessionInfo {
  String user;       // username/email
  bool isAdmin;
  unsigned long createdAt;
};

std::map<String, SessionInfo> sessions;
std::set<String> revokedTokens;
unsigned long nextSessionId = 1;

String makeToken() {
  // 32-char hex from a simple PRNG seeded by micros(). Good enough for an offline device.
  static const char* hex = "0123456789abcdef";
  String t;
  t.reserve(32);
  unsigned long seed = micros() ^ (nextSessionId * 2654435761UL);
  for (int i = 0; i < 32; i++) {
    seed = seed * 1103515245UL + 12345UL;
    t += hex[(seed >> 16) & 0xF];
  }
  return t;
}

String getCookieValue(AsyncWebServerRequest *req, const char* name) {
  if (!req->hasHeader("Cookie")) return String("");
  String cookie = req->header("Cookie");
  String needle = String(name) + "=";
  int start = cookie.indexOf(needle);
  if (start < 0) return String("");
  start += needle.length();
  int end = cookie.indexOf(';', start);
  if (end < 0) end = cookie.length();
  return cookie.substring(start, end);
}

SessionInfo* lookupSession(AsyncWebServerRequest *req) {
  String token = getCookieValue(req, SESSION_COOKIE);
  if (token.length() == 0) return nullptr;
  if (revokedTokens.count(token)) return nullptr;
  auto it = sessions.find(token);
  if (it == sessions.end()) return nullptr;
  return &it->second;
}

bool isAdminRequest(AsyncWebServerRequest *req) {
  SessionInfo* s = lookupSession(req);
  return s && s->isAdmin;
}

// --- Admin HTML pages (served from PROGMEM, no SD card file needed) ---
const char ADMIN_LOGIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PocketCloud - Admin Session</title>
<style>
body { font-family: -apple-system, sans-serif; background:#1e1e1e; color:#e0e0e0; margin:0; padding:40px; }
.box { max-width:380px; margin:auto; background:#2a2a2a; padding:28px; border-radius:10px; box-shadow:0 4px 15px rgba(0,0,0,.4); }
 h1{margin:0 0 18px;font-size:1.4em;color:#fff}
 label{display:block;font-size:.85em;margin-top:10px;color:#bbb}
 input{width:100%;box-sizing:border-box;background:#1e1e1e;color:#e0e0e0;border:1px solid #444;padding:10px 12px;border-radius:6px;margin-top:4px;font-size:1em}
 button{width:100%;margin-top:18px;background:#4a90e2;color:#fff;border:none;padding:10px 16px;border-radius:6px;font-size:1em;cursor:pointer}
 .err{color:#ffb4ab;margin-top:10px;font-size:.85em;min-height:1em}
 .hint{color:#888;font-size:.8em;margin-top:14px;text-align:center}
</style></head><body>
<div class="box">
  <h1>Admin sign-in</h1>
  <form method="POST" action="/admin/login">
    <label>Username</label>
    <input type="text" name="username" required autocomplete="username">
    <label>Password</label>
    <input type="password" name="password" required autocomplete="current-password">
    <div class="err" id="err"></div>
    <button type="submit">Sign in</button>
  </form>
  <div class="hint" id="hint"></div>
</div>
<script>
if (location.search.includes('error=1')) {
  document.getElementById('err').textContent = 'Invalid admin credentials.';
  document.getElementById('hint').textContent = 'Try again.';
}
</script>
</body></html>
)HTML";

const char ADMIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PocketCloud - Admin Panel</title>
<style>
body{font-family:-apple-system,sans-serif;background:#121212;color:#e0e0e0;margin:0;padding:20px}
.wrap{max-width:760px;margin:auto}
h1{color:#fff;margin:0 0 6px}
.sub{color:#888;margin:0 0 24px;font-size:.9em}
.card{background:#1e1e1e;border:1px solid #333;border-radius:8px;padding:18px;margin-bottom:18px}
 h2{margin:0 0 12px;font-size:1.1em;color:#fff}
 input[type=text],input[type=email]{background:#2a2a2a;color:#e0e0e0;border:1px solid #444;padding:8px 10px;border-radius:6px;font-size:.95em;margin-right:6px}
 button{background:#4a90e2;color:#fff;border:none;padding:8px 14px;border-radius:6px;cursor:pointer;font-size:.9em}
 button.danger{background:#b3261e}
 ul{list-style:none;padding:0;margin:12px 0 0}
 li{display:flex;justify-content:space-between;align-items:center;padding:8px 10px;margin:4px 0;background:#2a2a2a;border:1px solid #333;border-radius:6px;font-size:.9em}
 li button{background:transparent;color:#ffb4ab;border:1px solid #ffb4ab;padding:4px 10px;font-size:.8em}
 .row{display:flex;gap:6px;align-items:center}
 .empty{color:#666;font-size:.85em;font-style:italic}
 .flash{color:#4caf50;font-size:.85em;margin-top:8px;min-height:1em}
 .top{display:flex;justify-content:space-between;align-items:center;margin-bottom:18px}
 .top form{display:inline}
 .top button{background:#333}
</style></head><body>
<div class="wrap">
  <div class="top">
    <div>
      <h1>PocketCloud - Admin</h1>
      <div class="sub">Manage active sessions, signups, and the email whitelist.</div>
    </div>
    <form method="POST" action="/logout"><button type="submit">Sign out</button></form>
  </div>

  <div class="card">
    <h2>Pending signups</h2>
    <ul id="pending"><li class="empty">Loading…</li></ul>
  </div>

  <div class="card">
    <h2>Active sessions</h2>
    <ul id="sessions"><li class="empty">Loading…</li></ul>
  </div>

  <div class="card">
    <h2>Email whitelist</h2>
    <div class="row">
      <input type="email" id="newEmail" placeholder="user@example.com" required>
      <button onclick="addEmail()">Add</button>
    </div>
    <div class="flash" id="flash"></div>
    <ul id="emails"><li class="empty">Loading…</li></ul>
  </div>
</div>

<script>
function flash(msg){var f=document.getElementById('flash');f.textContent=msg;setTimeout(function(){f.textContent='';},2000);}

async function loadSessions(){
  var r=await fetch('/admin/api/users',{credentials:'same-origin'});
  var raw=await r.text();
  if(!r.ok){document.getElementById('sessions').innerHTML='<li class="empty">Failed ('+r.status+').</li>';return;}
  var j;
  try{j=JSON.parse(raw);}catch(e){document.getElementById('sessions').innerHTML='<li class="empty">Bad JSON: '+raw.slice(0,80)+'</li>';return;}
  var ul=document.getElementById('sessions');
  if(!j.sessions.length){ul.innerHTML='<li class="empty">No active sessions.</li>';return;}
  ul.innerHTML='';
  j.sessions.forEach(function(s){
    var li=document.createElement('li');
    var left=document.createElement('span');
    left.textContent=s.user+(s.admin?' (admin)':'');
    li.appendChild(left);
    if(!s.admin){
      var btn=document.createElement('button');
      btn.textContent='Revoke';
      btn.onclick=function(){revoke(s.token);};
      li.appendChild(btn);
    }else{
      var tag=document.createElement('span');
      tag.style.color='#888';
      tag.style.fontSize='.75em';
      tag.textContent='(protected)';
      li.appendChild(tag);
    }
    ul.appendChild(li);
  });
}

async function loadPending(){
  var r=await fetch('/admin/api/pending',{credentials:'same-origin'});
  if(!r.ok){document.getElementById('pending').innerHTML='<li class="empty">Failed ('+r.status+').</li>';return;}
  var j;try{j=await r.json();}catch(e){return;}
  var ul=document.getElementById('pending');
  if(!j.pending.length){ul.innerHTML='<li class="empty">No pending signups.</li>';return;}
  ul.innerHTML='';
  j.pending.forEach(function(p){
    var li=document.createElement('li');
    var info=document.createElement('span');
    info.innerHTML='<b>'+escapeHtml(p.user)+'</b> <span style="color:#888;font-size:.85em">(pw: '+escapeHtml(p.password)+')</span>';
    li.appendChild(info);
    var approve=document.createElement('button');
    approve.textContent='Approve';
    approve.style.background='#2e7d32';
    approve.style.color='#fff';
    approve.style.borderColor='#2e7d32';
    approve.style.marginRight='6px';
    approve.onclick=function(){approveUser(p.user);};
    var deny=document.createElement('button');
    deny.textContent='Deny';
    deny.onclick=function(){denyUser(p.user);};
    li.appendChild(approve);
    li.appendChild(deny);
    ul.appendChild(li);
  });
}

async function approveUser(user){
  var fd=new FormData();fd.set('user',user);
  var r=await fetch('/admin/api/approve',{method:'POST',body:fd,credentials:'same-origin'});
  if(r.ok){flash('Approved '+user);loadPending();}else{flash('Approve failed.');}
}

async function denyUser(user){
  var fd=new FormData();fd.set('user',user);
  var r=await fetch('/admin/api/deny',{method:'POST',body:fd,credentials:'same-origin'});
  if(r.ok){flash('Denied '+user);loadPending();}else{flash('Deny failed.');}
}

function escapeHtml(s){
  return String(s).replace(/[&<>"']/g,function(c){return ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[c];});
}

async function revoke(token){
  var fd=new FormData();fd.set('token',token);
  var r=await fetch('/admin/api/revoke',{method:'POST',body:fd,credentials:'same-origin'});
  if(r.ok){flash('Session revoked.');loadSessions();}else{flash('Revoke failed.');}
}

async function loadEmails(){
  var r=await fetch('/admin/api/whitelist',{credentials:'same-origin'});
  if(!r.ok){document.getElementById('emails').innerHTML='<li class="empty">Failed to load.</li>';return;}
  var j=await r.json();
  var ul=document.getElementById('emails');
  if(!j.emails.length){ul.innerHTML='<li class="empty">Whitelist is empty.</li>';return;}
  ul.innerHTML='';
  j.emails.forEach(function(e){
    var li=document.createElement('li');
    var left=document.createElement('span');left.textContent=e;li.appendChild(left);
    var btn=document.createElement('button');btn.textContent='Remove';
    btn.onclick=function(){removeEmail(e);};
    li.appendChild(btn);
    ul.appendChild(li);
  });
}

async function addEmail(){
  var i=document.getElementById('newEmail');
  var e=i.value.trim();if(!e)return;
  var fd=new FormData();fd.set('email',e);
  var r=await fetch('/admin/api/whitelist/add',{method:'POST',body:fd,credentials:'same-origin'});
  if(r.ok){i.value='';flash('Added '+e);loadEmails();}else{flash('Add failed.');}
}

async function removeEmail(e){
  var fd=new FormData();fd.set('email',e);
  var r=await fetch('/admin/api/whitelist/remove',{method:'POST',body:fd,credentials:'same-origin'});
  if(r.ok){flash('Removed '+e);loadEmails();}else{flash('Remove failed.');}
}

loadPending();
loadSessions();
loadEmails();
</script>
</body></html>
)HTML";

// --- Helpers ---

// List files in the SD root, hiding anything under /.web/ or starting with ".".
String getSDFileList() {
  String list = "<ul>";
  File root = SD.open("/");
  if (!root) return "<li>No SD Card</li>";

  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    String path = name.startsWith("/") ? name : "/" + name;

    if (!path.startsWith("/.") && !path.startsWith("/.web")) {
      list += "<li><a href='/download?file=" + path + "'>" + name + "</a></li>";
    }
    file = root.openNextFile();
  }
  return list + "</ul>";
}

// Read the whitelist file (one email per line, # for comments). Returns empty if missing.
String getWhitelistRaw() {
  File f = SD.open(WHITELIST_PATH, FILE_READ);
  if (!f) return String("");
  String s = f.readString();
  f.close();
  return s;
}

bool isWhitelisted(const String& email) {
  String target = email;
  target.toLowerCase();
  target.trim();
  if (target.length() == 0) return false;

  String raw = getWhitelistRaw();
  int start = 0;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    line.trim();
    if (line.length() > 0 && !line.startsWith("#")) {
      line.toLowerCase();
      if (line == target) return true;
    }
    start = nl + 1;
  }
  return false;
}

bool isAdminCreds(const String& user, const String& pass) {
  return user == ADMIN_USER && pass == ADMIN_PASS;
}

bool isTestCreds(const String& user, const String& pass) {
  return user == TEST_USER && pass == TEST_PASS;
}

bool credentialsValid(const String& user, const String& pass) {
  if (isAdminCreds(user, pass)) return true;
  if (isTestCreds(user, pass)) return true;
  if (isWhitelisted(user)) return true;
  // Approved users (from admin approval of a signup).
  String stored = findUserPass(USERS_PATH, user);
  if (stored.length() > 0 && stored == pass) return true;
  return false;
}

// Append a single email to the whitelist file. No-op if already present.
bool addToWhitelist(const String& email) {
  String e = email;
  e.trim();
  e.toLowerCase();
  if (e.length() == 0 || e.startsWith("#")) return false;
  if (isWhitelisted(e)) return true;
  File f = SD.open(WHITELIST_PATH, FILE_APPEND);
  if (!f) return false;
  f.println(e);
  f.close();
  return true;
}

// Remove a single email from the whitelist file (case-insensitive).
bool removeFromWhitelist(const String& email) {
  String raw = getWhitelistRaw();
  String target = email;
  target.trim();
  target.toLowerCase();
  if (target.length() == 0) return false;

  String out;
  out.reserve(raw.length());
  int start = 0;
  bool removed = false;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    String trimmed = line; trimmed.trim(); trimmed.toLowerCase();
    if (!removed && trimmed == target) {
      removed = true;
    } else {
      out += line;
      out += '\n';
    }
    start = nl + 1;
  }
  if (!removed) return false;
  File f = SD.open(WHITELIST_PATH, FILE_WRITE);
  if (!f) return false;
  f.print(out);
  f.close();
  return true;
}

// --- User database (pending + approved) ---
String readFileLines(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return String("");
  String s = f.readString();
  f.close();
  return s;
}

bool parseUserLine(const String& line, String& user, String& pass) {
  int colon = line.indexOf(':');
  if (colon <= 0) return false;
  user = line.substring(0, colon);
  pass = line.substring(colon + 1);
  user.trim();
  pass.trim();
  return user.length() > 0;
}

String findUserPass(const char* path, const String& user) {
  String raw = readFileLines(path);
  int start = 0;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    line.trim();
    if (line.length() > 0 && !line.startsWith("#")) {
      String u, p;
      if (parseUserLine(line, u, p) && u == user) return p;
    }
    start = nl + 1;
  }
  return String("");
}

bool appendUserPass(const char* path, const String& user, const String& pass) {
  if (user.length() == 0 || pass.length() == 0) return false;
  if (user.indexOf(':') >= 0 || user.indexOf('\n') >= 0) return false;
  if (pass.indexOf('\n') >= 0) return false;
  if (findUserPass(path, user).length() > 0) return true;
  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  f.print(user);
  f.print(':');
  f.println(pass);
  f.close();
  return true;
}

bool removeUser(const char* path, const String& user) {
  String raw = readFileLines(path);
  String out;
  out.reserve(raw.length());
  int start = 0;
  bool removed = false;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    String u, p;
    bool valid = parseUserLine(line, u, p);
    if (valid && !removed && u == user) {
      removed = true;
    } else {
      out += line;
      out += '\n';
    }
    start = nl + 1;
  }
  if (!removed) return false;
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.print(out);
  f.close();
  return true;
}

String userListJson(const char* path) {
  String raw = readFileLines(path);
  String json = "[";
  bool first = true;
  int start = 0;
  while (start < (int)raw.length()) {
    int nl = raw.indexOf('\n', start);
    if (nl < 0) nl = raw.length();
    String line = raw.substring(start, nl);
    line.trim();
    if (line.length() > 0 && !line.startsWith("#")) {
      String u, p;
      if (parseUserLine(line, u, p)) {
        if (!first) json += ",";
        first = false;
        json += "{\"user\":\"" + u + "\",\"password\":\"" + p + "\"}";
      }
    }
    start = nl + 1;
  }
  json += "]";
  return json;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

if (!SD.begin(PIN_CS, SPI, 20000000)) {
  Serial.println("SD Init Failed");
} else {
  const char* requiredFiles[] = {
    WHITELIST_PATH,
    USERS_PATH,
    PENDING_PATH
  };
  for (const char* path : requiredFiles) {
    if (!SD.exists(path)) {
      File f = SD.open(path, FILE_WRITE);
      if (f) f.close();
      Serial.println(String("Created: ") + path);
    }
  }
}
  WiFiManager wm;
  wm.setAPCallback([](WiFiManager *wm){
    Serial.println("WiFiManager: setup AP '" + String(setup_ap_name) + "' is up.");
    Serial.println("  Connect to it and open http://192.168.4.1 to configure Wi-Fi.");
  });

  bool connected = wm.autoConnect(setup_ap_name);
  if (!connected) {
    Serial.println("WiFiManager: failed to connect - restarting in 5s.");
    delay(5000);
    ESP.restart();
  }

  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("pocketcloud")) {
    Serial.println("mDNS: http://pocketcloud.local");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("mDNS: failed to start.");
  }

  // --- Static assets ---
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SD, "/.web/style.css", "text/css");
  });

  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SD, "/.web/script.js", "application/javascript");
  });

  // --- Login page ---
  server.on("/login.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SD, "/.web/login.html", "text/html");
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SD, "/.web/login.html", "text/html");
  });

  // --- /login POST ---
  server.on("/login", HTTP_POST, [](AsyncWebServerRequest *request){
    String user = "";
    String pass = "";
    if (request->hasParam("username", true)) user = request->getParam("username", true)->value();
    if (request->hasParam("email", true) && user.length() == 0) user = request->getParam("email", true)->value();
    if (request->hasParam("password", true)) pass = request->getParam("password", true)->value();

    String next = "/index.html";
    if (request->hasParam("next", true)) {
      next = request->getParam("next", true)->value();
      if (!next.startsWith("/")) next = "/" + next;
      if (next.startsWith("//") || next.startsWith("/\\")) next = "/index.html";
    }

    if (!credentialsValid(user, pass)) {
      String body = "<!DOCTYPE html><html><head><meta http-equiv='refresh' "
                    "content='2;url=/login.html?error=1'></head><body>"
                    "<p>Invalid credentials. Redirecting...</p></body></html>";
      request->send(401, "text/html", body);
      return;
    }

    String token = makeToken();
    SessionInfo si;
    si.user = user;
    si.isAdmin = isAdminCreds(user, pass);
    si.createdAt = millis();
    sessions[token] = si;

    AsyncWebServerResponse *res = request->beginResponse(302, "text/plain", "");
    res->addHeader("Location", next);
    res->addHeader("Set-Cookie", String(SESSION_COOKIE) + "=" + token + "; Path=/; HttpOnly");
    request->send(res);
  });

  // --- /logout ---
  server.on("/logout", HTTP_POST, [](AsyncWebServerRequest *request){
    String token = getCookieValue(request, SESSION_COOKIE);
    if (token.length() > 0) {
      sessions.erase(token);
      revokedTokens.insert(token);
    }
    AsyncWebServerResponse *res = request->beginResponse(302, "text/plain", "");
    res->addHeader("Location", "/login.html");
    res->addHeader("Set-Cookie", String(SESSION_COOKIE) + "=; Path=/; Max-Age=0");
    request->send(res);
  });

  // --- /signup ---
  server.on("/signup", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("username", true) || !request->hasParam("password", true)) {
      request->send(400, "text/html", "Missing username or password. <a href='/login.html'>Back</a>");
      return;
    }
    String user = request->getParam("username", true)->value();
    String pass = request->getParam("password", true)->value();
    user.trim();
    if (user.length() == 0 || pass.length() == 0) {
      request->send(400, "text/html", "Username and password are required. <a href='/login.html'>Back</a>");
      return;
    }
    if (findUserPass(PENDING_PATH, user).length() > 0 ||
        findUserPass(USERS_PATH, user).length() > 0 ||
        user == ADMIN_USER || user == TEST_USER || isWhitelisted(user)) {
      request->send(409, "text/html",
        "That username is already taken or reserved. <a href='/login.html'>Back</a>");
      return;
    }
    if (!appendUserPass(PENDING_PATH, user, pass)) {
      request->send(500, "text/html",
        "Could not save signup (SD card error). <a href='/login.html'>Back</a>");
      return;
    }
    String body = "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
                  "<p style='font-family:sans-serif;max-width:420px;margin:40px auto'>"
                  "Signup received for <b>" + user + "</b>. An admin will review your request.<br><br>"
                  "<a href='/login.html'>Back to sign in</a></p></body></html>";
    request->send(200, "text/html", body);
  });

  // --- /admin/login POST ---
  server.on("/admin/login", HTTP_POST, [](AsyncWebServerRequest *request){
    String user = "";
    String pass = "";
    if (request->hasParam("username", true)) user = request->getParam("username", true)->value();
    if (request->hasParam("password", true)) pass = request->getParam("password", true)->value();

    if (!isAdminCreds(user, pass)) {
      String body = "<!DOCTYPE html><html><head><meta http-equiv='refresh' "
                    "content='2;url=/admin?error=1'></head><body>"
                    "<p>Invalid admin credentials. Redirecting...</p></body></html>";
      request->send(401, "text/html", body);
      return;
    }

    String token = makeToken();
    SessionInfo si;
    si.user = String(ADMIN_USER);
    si.isAdmin = true;
    si.createdAt = millis();
    sessions[token] = si;

    AsyncWebServerResponse *res = request->beginResponse(302, "text/plain", "");
    res->addHeader("Location", "/admin");
    res->addHeader("Set-Cookie", String(SESSION_COOKIE) + "=" + token + "; Path=/; HttpOnly");
    request->send(res);
  });

  // --- Dashboard ---
  auto dashboardHandler = [](AsyncWebServerRequest *request){
    SessionInfo* s = lookupSession(request);
    if (!s) {
      AsyncWebServerResponse *r = request->beginResponse(302, "text/plain", "");
      r->addHeader("Location", "/login.html");
      request->send(r);
      return;
    }

    File file = SD.open("/.web/index.html", FILE_READ);
    if (!file) file = SD.open("/web/index.html", FILE_READ);
    if (!file) {
      request->send(404, "text/plain", "Index not found in .web folder");
      return;
    }
    String html = file.readString();
    file.close();
    html.replace("FILE_LIST_HERE", getSDFileList());
    html.replace("SESSION_USER", s->user);
    html.replace("SESSION_IS_ADMIN", s->isAdmin ? "true" : "false");
    request->send(200, "text/html", html);
  };
  server.on("/index.html", HTTP_GET, dashboardHandler);
  server.on("/dashboard",  HTTP_GET, dashboardHandler);

  // --- Admin static login page ---
  server.on("/admin/login.html", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", ADMIN_LOGIN_HTML);
  });

  // --- Admin API: list active sessions ---
  server.on("/admin/api/users", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    String json = "{\"sessions\":[";
    bool first = true;
    int idx = 0;
    for (auto& kv : sessions) {
      if (!first) json += ",";
      first = false;
      json += "{\"id\":" + String(idx++) +
              ",\"user\":\"" + kv.second.user + "\"" +
              ",\"admin\":" + (kv.second.isAdmin ? "true" : "false") +
              ",\"token\":\"" + kv.first + "\"}";
    }
    json += "],\"revoked\":[";
    first = true;
    for (auto& t : revokedTokens) {
      if (!first) json += ",";
      first = false;
      json += "\"" + t + "\"";
    }
    json += "]}";
    request->send(200, "application/json", json);
  });

  // --- Admin API: revoke a session ---
  server.on("/admin/api/revoke", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    if (!request->hasParam("token", true)) {
      request->send(400, "application/json", "{\"error\":\"missing token\"}");
      return;
    }
    String token = request->getParam("token", true)->value();
    if (sessions.count(token)) {
      sessions.erase(token);
      revokedTokens.insert(token);
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(404, "application/json", "{\"error\":\"not found\"}");
    }
  });

  // --- Admin API: whitelist CRUD ---
  server.on("/admin/api/whitelist", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    String raw = getWhitelistRaw();
    String json = "{\"emails\":[";
    bool first = true;
    int start = 0;
    while (start < (int)raw.length()) {
      int nl = raw.indexOf('\n', start);
      if (nl < 0) nl = raw.length();
      String line = raw.substring(start, nl);
      line.trim();
      if (line.length() > 0 && !line.startsWith("#")) {
        if (!first) json += ",";
        first = false;
        json += "\"" + line + "\"";
      }
      start = nl + 1;
    }
    json += "]}";
    request->send(200, "application/json", json);
  });

  server.on("/admin/api/whitelist/add", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    if (!request->hasParam("email", true)) {
      request->send(400, "application/json", "{\"error\":\"missing email\"}");
      return;
    }
    String email = request->getParam("email", true)->value();
    if (addToWhitelist(email)) {
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(500, "application/json", "{\"error\":\"could not write to SD\"}");
    }
  });

  server.on("/admin/api/whitelist/remove", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    if (!request->hasParam("email", true)) {
      request->send(400, "application/json", "{\"error\":\"missing email\"}");
      return;
    }
    String email = request->getParam("email", true)->value();
    if (removeFromWhitelist(email)) {
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(404, "application/json", "{\"error\":\"not found\"}");
    }
  });

  // --- Admin API: pending signups ---
  server.on("/admin/api/pending", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    request->send(200, "application/json", "{\"pending\":" + userListJson(PENDING_PATH) + "}");
  });

  server.on("/admin/api/approve", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    if (!request->hasParam("user", true)) {
      request->send(400, "application/json", "{\"error\":\"missing user\"}");
      return;
    }
    String user = request->getParam("user", true)->value();
    String pass = findUserPass(PENDING_PATH, user);
    if (pass.length() == 0) {
      request->send(404, "application/json", "{\"error\":\"no such pending user\"}");
      return;
    }
    if (!appendUserPass(USERS_PATH, user, pass)) {
      request->send(500, "application/json", "{\"error\":\"could not write to users.txt\"}");
      return;
    }
    removeUser(PENDING_PATH, user);
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/admin/api/deny", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }
    if (!request->hasParam("user", true)) {
      request->send(400, "application/json", "{\"error\":\"missing user\"}");
      return;
    }
    String user = request->getParam("user", true)->value();
    if (removeUser(PENDING_PATH, user)) {
      request->send(200, "application/json", "{\"ok\":true}");
    } else {
      request->send(404, "application/json", "{\"error\":\"no such pending user\"}");
    }
  });

  // --- Admin HTML panel ---
  server.on("/admin", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isAdminRequest(request)) {
      AsyncWebServerResponse *r = request->beginResponse(302, "text/plain", "");
      r->addHeader("Location", "/admin/login.html");
      request->send(r);
      return;
    }
    request->send(200, "text/html", ADMIN_HTML);
  });

  // --- Download handler ---
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String path = request->getParam("file")->value();
      if (SD.exists(path)) {
        AsyncWebServerResponse *res = request->beginResponse(SD, path, "application/octet-stream");
        res->addHeader("Content-Disposition", "attachment; filename=\"" + path + "\"");
        request->send(res);
      } else {
        request->send(404, "text/plain", "File not found: " + path);
      }
    }
  });

  // --- Upload handler with buffered 16 KB chunks ---
  // The response handler flushes any remaining bytes and closes the file.
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *request){
      // Flush leftover bytes that didn't fill a full buffer.
      if (uploadBufLen > 0 && uploadFile) {
        uploadFile.write(uploadBuf, uploadBufLen);
        uploadBufLen = 0;
      }
      if (uploadFile) uploadFile.close();
      request->send(200, "text/html", "Upload complete. <a href='/index.html'>Back</a>");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
      // Open the file on first chunk.
      if (!index) {
        uploadBufLen = 0;
        uploadFile = SD.open("/" + filename, FILE_WRITE);
      }

      if (uploadFile) {
        // Fill the buffer; flush to SD whenever it is full.
        size_t remaining  = len;
        size_t dataOffset = 0;

        while (remaining > 0) {
          size_t space  = UPLOAD_BUF_SIZE - uploadBufLen;
          size_t toCopy = min(remaining, space);
          memcpy(uploadBuf + uploadBufLen, data + dataOffset, toCopy);
          uploadBufLen  += toCopy;
          dataOffset    += toCopy;
          remaining     -= toCopy;

          // Write a full buffer's worth to the SD card in one call.
          if (uploadBufLen == UPLOAD_BUF_SIZE) {
            uploadFile.write(uploadBuf, uploadBufLen);
            uploadBufLen = 0;
          }
        }
      }

      // On the last chunk, flush whatever is left and close.
      // (The response handler above also does this as a safety net.)
      if (final) {
        if (uploadBufLen > 0 && uploadFile) {
          uploadFile.write(uploadBuf, uploadBufLen);
          uploadBufLen = 0;
        }
        if (uploadFile) uploadFile.close();
      }
    }
  );

  server.begin();
}

void loop() {}
