# PocketCloud

A low-cost, self-contained local storage appliance built on the *ESP32-S3* microcontroller. PocketCloud lets you upload, download, and manage files through a secure, browser-accessible web interface over a private Wi-Fi network — no internet connection required.

    Capstone project — Bachelor of Science in Information Technology  
    Aparece · Grave · Manalo · Meneses · Palomata



---

## Features

-  Login-protected file dashboard accessible from any browser
-  Role-based access control (admin vs. regular user)
-  Admin panel to approve signups, revoke sessions, and manage the email whitelist
-  All user data persisted on the MicroSD card — survives reboots without a database
-  Works fully offline after first-time Wi-Fi setup
-  Reachable at 
http://pocketcloud.local on your home network

---

## Hardware Requirements

| Component | Specification |
|---|---|
| Microcontroller | ESP32-S3-USB-OTG development board |
| Storage module | MicroSD card adapter (SPI interface) |
| MicroSD card | 8 GB or larger, FAT32 formatted |
| Power supply | DC-DC buck converter (5V → 3.3V) |
| Filter capacitor | 100 µF electrolytic |
| Miscellaneous | Breadboard, jumper wires, USB-C cable |

---

## Wiring

| ESP32-S3 Pin | MicroSD Module Pin | Wire Color |
|---|---|---|
| GPIO 4 | CS | Yellow |
| GPIO 5 | SCK | Orange |
| GPIO 6 | MOSI | Purple |
| GPIO 7 | MISO | Blue |
| GND | GND | Black |
| Buck converter VOUT+ | VCC | Red |

    **Important:** Power the MicroSD module from the **DC-DC buck converter** (5V → 3.3V), not from the ESP32's onboard 3V3 pin. Powering both from the same rail causes brownout resets during file uploads. Add a **100 µF electrolytic capacitor** across the ESP32's 3V3 and GND rails to absorb Wi-Fi transmission spikes.



---

## Prerequisites

### 1. Arduino IDE

Download and install 
*Arduino IDE 2.x* from:  
https://www.arduino.cc/en/software

### 2. ESP32 Board Package

1. Open Arduino IDE
2. Go to *File → Preferences*
3. Add this URL to the Additional boards manager URLs field:
   
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   
4. Go to *Tools → Board → Boards Manager*
5. Search for esp32 and install *ESP32 by Espressif Systems* (version 3.3.11 or later)

### 3. Required Libraries

Install all of the following via *Tools → Manage Libraries*:

| Library | Author | Notes |
|---|---|---|
| ESPAsyncWebServer | ESP32Async | Search ESPAsyncWebServer |
| AsyncTCP | ESP32Async | Required by ESPAsyncWebServer |
| WiFiManager | tzapu | Search WiFiManager |

    `SD`, `SPI`, and `ESPmDNS` are bundled with the ESP32 core — no separate install needed.



---

## SD Card Setup

1. Format your MicroSD card as 
*FAT32*
2. Create a folder named .web at the root of the card
3. Copy the following files into .web/:

MicroSD card/
└── .web/
    ├── index.html
    ├── login.html
    ├── style.css
    ├── script.js
    ├── whitelist.txt     ← create as empty file
    ├── users.txt         ← create as empty file
    └── pending.txt       ← create as empty file

    The three `.txt` files can be empty — the firmware creates them automatically on first boot if they are missing (as of the latest firmware version). Creating them manually is still recommended.



---

## Installation

1. Clone or download this repository
2. Open 
pocketcloud_final.ino in Arduino IDE
3. Select your board: *Tools → Board → ESP32S3 Dev Module*
4. Select the correct port: *Tools → Port → (your COM port)*
5. Click *Upload*

---

## First Boot

1. After uploading, open the *Serial Monitor* (115200 baud) to see the device output
2. On your phone or laptop, look for a Wi-Fi network named **PocketCloud-Start**
3. Connect to it and open *http://192.168.4.1* in your browser
4. Select your home Wi-Fi network and enter the password
5. The ESP32 will restart and join your home network

From any device on the same network, open:
http://pocketcloud.local

---

## Default Credentials

| Account | Username | Password |
|---|---|---|
| Administrator | Admin | pcloud |
| Test user | test | pcloud |

     These are hardcoded for demonstration purposes. Do not use this device on an untrusted network without changing the credentials in the firmware source code.



---

## Usage

### Signing in
Navigate to 
http://pocketcloud.local and sign in with one of the accounts above, or sign up for a new account (requires admin approval).

### Uploading files
Click *Upload* on the dashboard, select a file, and confirm. Files are stored at the root of the MicroSD card.

### Downloading files
Click any filename in the dashboard to download it directly to your device.

### Admin panel
Sign in as Admin and click *Admin Panel* in the dashboard. From here you can:
- Approve or deny pending signups
- Revoke active sessions
- Add or remove emails from the whitelist

---

## Troubleshooting

| Problem | Likely cause | Fix |
|---|---|---|
| SD Init Failed in Serial Monitor | SD card not detected | Check wiring, reformat card as FAT32 |
| Brownout resets during upload | SD powered from ESP32 3V3 rail | Use a dedicated DC-DC buck converter |
| pocketcloud.local not resolving | Browser mDNS issue (common in Firefox) | Use the IP address shown in Serial Monitor instead |
| Login always fails | .web/users.txt missing | Create the file manually on the SD card |
| Can't connect to PocketCloud-Start | Device already configured | Hold the BOOT button on the ESP32 at power-on to reset Wi-Fi settings |

---

## Project Structure

PocketCloud/
├── pocketcloud_final.ino   Main firmware (ESP32-S3, Arduino framework)
└── .web/
    ├── index.html          File dashboard
    ├── login.html          Login and sign-up page
    ├── style.css           Shared stylesheet
    └── script.js           Frontend JavaScript

---

## License

This project is licensed under the *MIT License* — see [LICENSE](LICENSE) for details.

---

## Acknowledgements

- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) by ESP32Async
- [WiFiManager](https://github.com/tzapu/WiFiManager) by tzapu
- [Arduino ESP32 core](https://github.com/espressif/arduino-esp32) by Espressif Systems
