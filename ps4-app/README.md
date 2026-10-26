# PS4 Direct Downloader (Native Homebrew App .PKG)

Applicazione Homebrew nativa per PlayStation 4 jailbroken (firmware **5.05 - 11.00+**) sviluppata in C con **SDL2** e **OpenOrbis Toolchain**.

Elimina completamente la necessità di utilizzare un PC o un server FTP intermedio: l'applicazione gira direttamente sul sistema operativo OrbisOS della PS4, permettendo di cercare i giochi, scaricarli direttamente sull'hard disk interno (`/data/pkg/`) e avviare l'installazione automatica tramite le API interne di **GoldHEN**.

---

## Caratteristiche Principali

- **100% Nativo per PS4**: Gira direttamente sulla console, senza browser web né server intermedi.
- **Nessun FTP richiesto**: I pacchetti `.pkg` vengono scaricati direttamente nella memoria interna della PS4.
- **Installazione Automatica**: Al termine dello scaricamento, il gioco viene inviato automaticamente all'API del Package Installer di GoldHEN (`http://127.0.0.1:12800/api/install`).
- **Verifica Magic Header `0x7fCNT`**: Riconosce e blocca immediatamente pagine HTML o captcha prima che corrompano il disco.
- **Resolver Integrati in C**:
  - Supporto nativo per **1fichier** (con gestione automatica password e token).
  - Supporto per **Mediafire**.
  - Integrazione **Real-Debrid API** (`/rest/1.0/unrestrict/link`) per scaricare alla massima velocità della fibra ottica.
- **UI Televisiva 1080p Fluida (60 FPS)**:
  - Font engine raster 8x8 bitmap integrato (zero dipendenze grafiche esterne).
  - Griglia visuale dei giochi con badge console (PS4, PS5, PS3).
  - Barra di avanzamento in tempo reale con velocità (MB/s), ETA ed errore/successo.
  - Pieno supporto ai gamepad DualShock 4 e DualSense.
- **Architettura Dual-Target**:
  - Può essere eseguita sia come **simulatore desktop su Linux** (per sviluppo rapido e test con mouse/tastiera/gamepad) sia compilata come **EBOOT.BIN nativo per PS4**.

---

## Struttura della Cartella `ps4-app`

```text
ps4-app/
├── src/
│   ├── main.c           # Entry point SDL2, loop a 60 FPS, gestione eventi
│   ├── ui.h / ui.c      # Interfaccia grafica 1080p, font engine, rendering griglia
│   ├── downloader.h/.c  # Download multithread streaming verso /data/pkg/
│   ├── resolver.h/.c    # Debrid, 1fichier e Mediafire resolver in C
│   ├── dlps.h / dlps.c  # Client API catalogo e parser HTML
│   ├── installer.h/.c   # Integrazione API GoldHEN package installer
│   ├── config.h/.c      # Gestione settings.ini (/data/ps4-downloader/)
│   └── cJSON.h / cJSON.c# Parser JSON leggero embedded
├── sce_sys/             # Metadati obbligatori del pacchetto PS4
│   ├── param.sfo        # Title ID: PSFT00001, Categoria: gd, Min FW: 5.05
│   ├── icon0.png        # Icona per la dashboard della PS4 (512x512)
│   └── pic1.png         # Banner sfondo a tutto schermo (1920x1080)
├── tools/
│   ├── gen_sfo.py       # Generatore binario standalone di param.sfo
│   ├── gen_assets.py    # Generatore di icon0.png e pic1.png da icona master
│   └── gen_gp4.py       # Generatore del progetto XML GP4 per il packaging
├── setup-toolchain.sh   # Script di installazione automatica OpenOrbis Toolchain
├── build-pkg.sh         # Script turnkey per creare il file .pkg finale
├── Makefile             # Compilazione duale (make desktop / make ps4)
└── README.md
```

---

## 1. Guida Rapida: Test sul Simulatore Desktop

Puoi avviare e testare l'intera interfaccia grafica, i controlli e i download direttamente sul tuo PC Linux:

```bash
cd ps4-app
make desktop
./ps4-downloader-sim
```

### Controlli nel Simulatore (Tastiera & Gamepad)

| Azione | DualShock 4 / Gamepad | Tastiera PC |
|---|---|---|
| **Spostamento griglia** | D-Pad (Frecce direzionali) | Frecce direzionali / W, A, S, D |
| **Scarica gioco selezionato** | Croce (`X`) | Invio (`Enter`) / `X` |
| **Cambia pagina** | `L1` / `R1` | `[` (prec) / `]` (succ) o `L` / `R` |
| **Filtro Console (PS4/PS5/PS3)** | `L2` / `R2` | `1` (Tutte), `2` (PS4), `3` (PS5) |
| **Annulla / Esci** | Cerchio (`O`) | `ESC` / `Q` |

---

## 2. Compilazione del Pacchetto Nativo `.pkg` per PS4

### Passo A: Installazione della Toolchain OpenOrbis

Per compilare codice C per il processore x86_64 e il kernel BSD (OrbisOS) della PS4, è necessaria la toolchain open-source **OpenOrbis**:

```bash
cd ps4-app
./setup-toolchain.sh
```

Lo script scaricherà ed estrarrà automaticamente la toolchain in `/opt/OpenOrbis/PS4Toolchain` e configurerà la variabile di ambiente `OO_PS4_TOOLCHAIN`.

### Passo B: Compilazione di `EBOOT.BIN`

Compila l'eseguibile nativo per PS4:

```bash
make ps4
```

Verrà generato il file: `build/eboot.bin`.

### Passo C: Generazione del file `.pkg`

Esegui lo script di packaging:

```bash
./build-pkg.sh
```

Lo script genererà:
1. `sce_sys/param.sfo` con Title ID `PSFT00001`
2. `sce_sys/icon0.png` e `sce_sys/pic1.png`
3. Il file di specifica `app.gp4`
4. Il pacchetto finale **`ps4-downloader-v1.0.0.pkg`** tramite `orbis-pub-cmd`.

> **Nota per Windows o Wine**: Se preferisci usare gli strumenti grafici ufficiali di Fake PKG Generator (`orbis-pub-gen.exe`), ti basterà aprire il file `app.gp4` generato nella cartella `ps4-app` e cliccare su **Build**.

---

## 3. Installazione e Utilizzo sulla PS4

1. Copia il file **`ps4-downloader-v1.0.0.pkg`** nella cartella principale di una chiavetta USB formattata in **exFAT** o **FAT32**.
2. Collega la chiavetta USB a una porta della PS4 con jailbreak attivo (**GoldHEN** avviato).
3. Sulla PS4, apri:
   **Impostazioni** ➔ **GoldHEN** ➔ **Package Installer**.
4. Seleziona **PS4 Direct Downloader** e premi `X` per completare l'installazione.
5. L'applicazione apparirà direttamente nella Dashboard principale della PS4 con la sua icona e lo sfondo dedicato.

---

## 4. Configurazione e Real-Debrid

Le impostazioni dell'app vengono salvate automaticamente in un file `.ini`:
- Su PS4: `/data/ps4-downloader/settings.ini`
- Su PC (simulatore): `~/.config/ps4-downloader/settings.ini`

### Esempio di `settings.ini`:

```ini
[general]
download_dir=/data/pkg
auto_install=true
console_filter=1

[debrid]
realdebrid_token=IL_TUO_TOKEN_API_REAL_DEBRID
```

Inserendo il proprio token API Real-Debrid, tutti i link 1fichier, Rapidgator o Nitroflare vengono automaticamente sbloccati ad altissima velocità senza captcha o attese.
