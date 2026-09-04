// --- Theme toggle (shared across pages) ---
const btn = document.getElementById('btn-theme');
const body = document.body;

function applyTheme(theme) {
    if (theme === 'dark') {
        body.classList.add('dark-mode');
        if (btn) btn.innerText = 'Light Mode';
    } else {
        body.classList.remove('dark-mode');
        if (btn) btn.innerText = 'Dark Mode';
    }
}

applyTheme(localStorage.getItem('theme') === 'dark' ? 'dark' : 'light');

if (btn) {
    btn.addEventListener('click', () => {
        const next = body.classList.contains('dark-mode') ? 'light' : 'dark';
        body.classList.toggle('dark-mode', next === 'dark');
        localStorage.setItem('theme', next);
        applyTheme(next);
    });
}

// --- Login page logic ---
const loginForm = document.getElementById('login-form');
const loginError = document.getElementById('login-error');

function showLoginError(msg) {
    if (!loginError) return;
    loginError.textContent = msg;
    loginError.hidden = false;
}

function clearLoginError() {
    if (!loginError) return;
    loginError.textContent = '';
    loginError.hidden = true;
}

if (loginForm) {
    loginForm.addEventListener('submit', () => {
        clearLoginError();
        // Server is the source of truth — let every submit reach /login.
        // It accepts Admin/pcloud, test/pcloud, and any whitelisted email.
        // An invalid attempt just gets a 401 with "Invalid credentials."
    });
}
