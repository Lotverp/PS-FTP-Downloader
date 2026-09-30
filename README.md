<div align="center">

# PS FTP Downloader

**Download PKG and files directly from the network to your PS — zero local storage.**

[![License: MIT](https://img.shields.io/badge/License-MIT-violet.svg)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![GTK: 4](https://img.shields.io/badge/GTK-4-green.svg)](https://gtk.org/)
[![Built with: C](https://img.shields.io/badge/Built%20with-C-orange.svg)]()

</div>

---

**PS FTP Downloader** downloads PKG files (and any other file) directly from a URL and sends them to your PlayStation via FTP in real-time, without ever saving anything on your PC's disk. Data flows directly from the internet -> RAM ring-buffer -> PS.

Compatible with any PS FTP server: **GoldHen (PS4)**, **ps4-hen**, **ps3-hen**, etc.

---

## Features

| Feature | Description |
|----------|-------------|
| **Pure Streaming** | Simultaneous HTTP download and FTP upload via 2 MB ring-buffer |
| **Zero Storage** | No temporary files saved on your PC |
| **Auto-Resume**    | Seamlessly recovers from dropped HTTP connections (like `wget -c`) |
| **Live Progress Bar**| Speed in MB/s, transferred size, percentage, and ETA |
| **Multi-download** | Multiple concurrent transfers from the same window |
| **Auto-detection** | File extension and size detected via HTTP HEAD request |
| **Cancel / Remove**| Cancel active transfers or remove completed ones |
| **FTP Settings**   | Change host, port, directory, user, and password live |
| **Premium UI**     | Modern GTK4 interface with dark indigo/violet theme |

---

## Installation

### Option A — `.deb` Package (Recommended for Debian/Ubuntu)

Check the **[Releases](../../releases)** tab for the pre-compiled `.deb` package.

```bash
sudo apt install ./ps-ftp-downloader_1.1.0_amd64.deb
```

Then launch from your application menu or terminal:
```bash
ps-ftp-downloader
```

### Option B — Build from Source

```bash
# Dependencies
sudo apt-get install -y build-essential libgtk-4-dev libcurl4-openssl-dev

# Clone and build
git clone https://github.com/Lotverp/PS-FTP-Downloader.git
cd PS-FTP-Downloader/gtk-app
make
./ps-ftp-downloader
```

### Option C — Build local `.deb`

```bash
cd PS-FTP-Downloader/gtk-app
./build-deb.sh
sudo apt install ../ps-ftp-downloader_1.1.0_amd64.deb
```

---

## Usage — PS Setup

Before using the application, you must enable the FTP server on your console:

**For PS4 (GoldHen):**
1. Enable **GoldHen**.
2. Go to PS4 **Settings**.
3. Scroll down and enter **GoldHen** settings (or **Debug Settings**).
4. Navigate to **Server Settings**.
5. Check **Enable FTP Server**.
6. The PS4 will display an IP address and Port (usually `2121`). Write them down.
7. Check that the `pkg` folder is present in `/data`. If it is not present, create it (the `.pkg` files will be copied there to be installed later via GoldHen).

---

## Usage — PC Application

1. Launch **PS FTP Downloader**.
2. Click **Settings** and insert the **FTP Host (IP address)** provided by your PS and the **FTP Port** (default `2121`).
3. Click **New Download**.
4. Paste the direct download URL of your PKG. The app will automatically analyze the URL to detect the file extension and size.
5. Enter the **Destination filename** (the app adds the extension automatically if detected).
6. Click **Start** — the streaming will begin immediately.
7. Once the transfer is **DONE**, go to your PS.
8. (PS4) Navigate to **GoldHen** -> **Debug Settings** -> **Package Installer**.
9. Select and install the PKG file you just transferred.

---

## CLI Script (Terminal)

For terminal users, a standalone bash script is available:

```bash
chmod +x ps-ftp-download.sh
./ps-ftp-download.sh
```

**CLI Dependencies:** `wget`, `curl`, `pv`
```bash
sudo apt-get install -y wget curl pv
```

---

## Default Configuration

| Parameter | Value |
|-----------|--------|
| **PS IP** | `192.168.1.111` |
| **FTP Port** | `2121` |
| **Directory** | `data/pkg` |
| **Username** | `anonymous` |

> Change the IP in **Settings** within the GUI app, or by editing the `FTP_HOST` variable at the top of the bash script.

---

## How it Works

```
PC                         RAM (2 MB ring buffer)              PS
────────────────────────────────────────────────────────────────────
  Internet  ──► HTTP thread ──────────────► FTP thread ──► PS FTP
  (wget/libcurl)       POSIX ring buffer         (libcurl STOR)
```

1. **HTTP Thread** downloads the file in chunks using libcurl, writing to the ring buffer.
2. **FTP Thread** reads from the ring buffer and uploads via `CURLOPT_READFUNCTION`.
3. The **progress** is updated every 350ms through `g_idle_add()` on the GTK thread.

---

## Project Structure

```
PS-FTP-Downloader/
├── gtk-app/
│   ├── main.c                     # GTK4 C source code (~1600 lines)
│   ├── style.css                  # Dark UI theme
│   ├── Makefile                   # Build system
│   ├── ps-ftp-downloader.desktop  # App menu integration
│   └── build-deb.sh               # .deb package builder
├── ps-ftp-download.sh             # Bash CLI script
├── README.md
└── LICENSE
```

---

## System Requirements

- **OS**: Linux (Ubuntu 22.04+, Debian 12+, or compatible)
- **GTK**: 4.6 or higher
- **PS**: Firmware with active HEN/GoldHen and running FTP server
- **Network**: PS and PC connected to the same local network (LAN/WiFi)
- **Architecture**: x86_64 (amd64)

---

## License

MIT License — see [LICENSE](LICENSE)

---

<div align="center">
Made for the PS homebrew community
</div>
