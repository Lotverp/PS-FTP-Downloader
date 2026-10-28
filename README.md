<div align="center">

# P2C Downloader
### (PC to Console Downloader)

**Download e streaming diretto di qualsiasi file, gioco o archivio da Internet a qualsiasi Console via FTP — Zero spazio occupato su PC.**

[![License: MIT](https://img.shields.io/badge/License-MIT-violet.svg)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![GTK: 4](https://img.shields.io/badge/GTK-4-green.svg)](https://gtk.org/)
[![Built with: C](https://img.shields.io/badge/Built%20with-C-orange.svg)]()
[![GitHub Repo](https://img.shields.io/badge/GitHub-Lotverp%2FP2C--Downloader-blue.svg)](https://github.com/Lotverp/P2C-Downloader)

</div>

---

## Panoramica del Progetto

**P2C Downloader** (**PC-to-Console Downloader**) è un'applicazione nativa per Linux ad alte prestazioni (scritta in C con interfaccia grafica moderna in GTK4) concepita per trasferire e installare giochi, aggiornamenti, DLC, homebrew, immagini disco, ROM e qualsiasi tipologia di file direttamente dal web alla tua **console o dispositivo di rete**.

A differenza dei metodi tradizionali — che costringono a scaricare decine o centinaia di gigabyte sul disco del computer per poi copiarli faticosamente su chiavette USB o inviarli via FTP in un secondo momento — P2C Downloader adotta una tecnologia proprietaria di **Pure RAM Ring-Buffer Streaming**:

```
┌──────────────┐          HTTP / HTTPS          ┌──────────────────────┐          FTP Socket          ┌──────────────────────┐
│   INTERNET   │ ─────────────────────────────► │    RAM RING-BUFFER   │ ──────────────────────────► │       CONSOLE        │
│ Direct/Hoster│     Download asincrono (C)     │ (2 MB circolare atom)│      Upload asincrono (C)    │ PS3/PS4/PS5/Switch...│
└──────────────┘                                └──────────────────────┘                             └──────────────────────┘
```

> [!IMPORTANT]
> **Massima Versatilità — Non solo PlayStation, non solo .PKG!**
> - **Qualsiasi Console**: P2C Downloader dialoga tramite protocollo standard FTP con qualsiasi console o dispositivo dotato di server FTP attivo: **PlayStation 4, PlayStation 5, PlayStation 3, PlayStation Vita, Nintendo Switch, Xbox, retro-console e dispositivi embedded**.
> - **Qualsiasi Formato File**: L'applicazione non è limitata ai pacchetti `.pkg`. Supporta in streaming puro qualsiasi estensione: **`.pkg`, `.iso`, `.bin`, `.cue`, `.xci`, `.nsp`, `.cia`, `.chd`, `.cso`, `.wbfs`, `.rvz`, `.vpk`, `.zip`, `.7z`, archivi compressi, file multimediali e salvataggi**.

* **Zero spazio consumato su PC**: Anche un file da 80 GB viene trasferito senza salvare un singolo byte di file temporaneo sul disco del tuo computer.
* **Velocità massima di rete**: Il download da Internet e l'invio FTP alla console avvengono simultaneamente in parallelo su thread indipendenti protetti da lock atomici.
* **Niente chiavette USB**: Nessun limite di capienza (come il formato FAT32 a 4 GB), nessun bisogno di partizionamento exFAT e nessun tempo di copia intermediario.

---

## Indice dei Contenuti
1. [Funzionalità Chiave](#funzionalità-chiave)
2. [Compatibilità e Configurazione Console](#compatibilità-e-configurazione-console)
   - [PlayStation 4 (GoldHEN / hen)](#1-playstation-4-con-goldhen)
   - [PlayStation 5 (etaHEN / ftpserver)](#2-playstation-5-con-etahen)
   - [PlayStation 3 (webMAN-MOD / HEN / CFW)](#3-playstation-3-con-webman-mod--hen)
   - [PlayStation Vita (VitaShell FTP)](#4-playstation-vita-con-vitashell)
   - [Nintendo Switch (ftpd / Atmosphere)](#5-nintendo-switch-con-ftpd)
   - [Xbox e Altre Console FTP](#6-xbox-e-altri-sistemi-con-server-ftp)
3. [Installazione e Avvio su Linux](#installazione-e-avvio-su-linux)
4. [Guida Completa all'Interfaccia Grafica](#guida-completa-allinterfaccia-grafica)
   - [4.1 Barra Superiore (Header Bar)](#41-barra-superiore-header-bar)
   - [4.2 Finestra: "+ Nuovo Download"](#42-finestra--nuovo-download)
   - [4.3 Finestra: "Archivio Diviso" (Split Archive .rar/.zip/.7z)](#43-finestra-archivio-diviso-split-archive)
   - [4.4 Scheda: "Sfoglia Giochi" (Catalogo DLPSGame)](#44-scheda-sfoglia-giochi-catalogo-dlpsgame)
   - [4.5 Finestra: "Dettagli Gioco" e Selezione Mirror](#45-finestra-dettagli-gioco-e-selezione-mirror)
   - [4.6 Scheda: "Tools" (Store Homebrew & Applicazioni)](#46-scheda-tools-store-homebrew--applicazioni)
   - [4.7 Finestra: "Gestione Sorgenti" (Repository Feed)](#47-finestra-gestione-sorgenti-repository-feed)
   - [4.8 Scheda: "Download Attivi" (Monitoraggio e Coda)](#48-scheda-download-attivi-monitoraggio-e-coda)
   - [4.9 Finestra: "Impostazioni" (FTP Settings e Anti-Bot)](#49-finestra-impostazioni-ftp-settings-e-anti-bot)
5. [Tecnologia Anti-Bot: Undetected ChromeDriver (UC Resolver)](#tecnologia-anti-bot-undetected-chromedriver)
6. [Integrazione Real-Debrid](#integrazione-real-debrid)
7. [Consigli per la Massima Velocità di Trasferimento](#consigli-per-la-massima-velocità-di-trasferimento)
8. [Script Standalone da Terminale (CLI)](#script-standalone-da-terminale-cli)
9. [Risoluzione dei Problemi e Domande Frequenti (FAQ)](#risoluzione-dei-problemi-e-domande-frequenti-faq)

---

## Funzionalità Chiave

* **Architettura Pure Streaming in RAM**: Un buffer circolare atomico in memoria (circa 2 MB) fa da ponte tra il download HTTP e l'upload FTP alla console. Nessuna usura dell'SSD e nessun file residuo sul PC.
* **Supporto a Qualsiasi Estensione**: Streaming diretto di pacchetti `.pkg`, immagini `.iso`/`.bin`, formati Nintendo `.xci`/`.nsp`, archivi, homebrew e file generici.
* **Catalogo Giochi Integrato (oltre 6.500 titoli)**: Sfoglia, cerca per titolo o codice (CUSA, PPSA, Title ID) con filtri per piattaforma (PS5, PS4, PS3, PS2, PS1, PSP, PS Vita, Nintendo Switch, Xbox, PC) e regione (EUR, USA, ASIA).
* **Algoritmo di Download Automatico Intelligente (1-Click)**: L'applicazione seleziona in autonomia la sorgente migliore con ordine di preferenza: Link Diretto > Mediafire > 1Fichier > Real-Debrid > Container Protetti.
* **Motore Anti-Bot Undetected ChromeDriver**: Bypass automatico in background di countdown timer, controlli Cloudflare e captcha (CutCaptcha con eventi mouse fisici simulati via CDP).
* **Protezione da Pubblicità Invasiva e Popunder**: Blocco completo a livello di browser di finestre popup ingannevoli e chiusura istantanea di schede ad clickjacking.
* **Supporto Real-Debrid 1-Click**: Risoluzione immediata dei link 1Fichier, Rapidgator e Mega tramite API Real-Debrid, aggirando i tempi di attesa forzati per gli utenti gratuiti.
* **Auto-Resume dei Trasferimenti**: Se la connessione cade temporaneamente o l'FTP si riavvia, l'app riprende il trasferimento dal byte esatto in cui si era interrotto (`Range: bytes` + `REST`).
* **Gestore Archivi Divisi Multi-Volume (.part1.rar / .zip / .7z)**: Scarica automaticamente le parti, estrae localmente il file payload contenuto (sia esso `.pkg`, `.iso`, `.bin` o qualsiasi altro formato) con `7zip`, lo invia alla console via FTP ed elimina all'istante i file temporanei.
* **Store Homebrew & Strumenti**: Installazione rapida con un clic degli homebrew più diffusi (Itemzflow, Apollo Save Tool, PS4-Xplorer, GoldHEN Cheats Manager) con feed JSON espandibili.

---

## Compatibilità e Configurazione Console

Affinché l'applicazione possa inviare i file, la console di destinazione e il computer devono essere collegati alla **stessa rete locale (stessa LAN o Wi-Fi)** e il server FTP della console deve essere attivo.

### 1. PlayStation 4 con GoldHEN

GoldHEN integra nativamente un server FTP robusto e performante (`ftpsrv`).

#### Passo 1: Attivare il server FTP su PS4
1. Accendi la PS4 e avvia l'exploit GoldHEN.
2. Dalla dashboard della PS4, entra in **Impostazioni**.
3. In cima alla schermata seleziona **GoldHEN** (oppure entra in **Debug Settings**).
4. Entra nel sottomenu **Server Settings**.
5. Trova la voce **Enable FTP Server** e spunta la casella.
6. Comparirà una notifica in alto a sinistra del tipo:
   ```text
   FTP Server: 192.168.1.111:2121
   ```
   * **Indirizzo IP**: È l'indirizzo della tua console (es. `192.168.1.111`).
   * **Porta FTP**: GoldHEN utilizza come porta predefinita la **`2121`**.

#### Passo 2: La cartella di destinazione `data/pkg`
Il Package Installer di GoldHEN cerca i file `.pkg` all'interno della cartella:
```text
data/pkg
```
* **Creazione automatica**: Nelle **Impostazioni** di P2C Downloader, premendo il tasto **`Test Connessione & Crea Cartella`**, l'app si connette alla console e crea automaticamente la cartella `data/pkg` se non è ancora presente.
* **Installazione del gioco**: A download completato, vai in **Impostazioni** ➔ **GoldHEN** ➔ **Package Installer** e installa il pacchetto trasferito con il tasto **X** del controller.

---

### 2. PlayStation 5 con etaHEN

1. Avvia l'exploit PS5 e carica l'homebrew enabler **etaHEN** (oppure invia il payload `ftpserver`).
2. Nelle impostazioni di etaHEN, verifica che la voce **FTP Server** sia abilitata.
3. Annota l'indirizzo IP e la porta (su PS5 la porta è solitamente **`1337`** oppure **`2121`**).
4. La cartella standard su PS5 è `data/pkg`.
5. A trasferimento completato, installa il file dal **Package Installer** di etaHEN.

---

### 3. PlayStation 3 con webMAN-MOD / HEN

1. Avvia la console con HEN o Custom Firmware (Evilnat / Cobra).
2. Assicurati che **webMAN-MOD** sia attivo (il server FTP si avvia automaticamente in background).
3. La porta FTP standard di webMAN-MOD è la **`21`**.
4. Cartelle consigliate:
   - Per pacchetti `.pkg`: imposta la directory su **`dev_hdd0/packages`**.
   - Per giochi formato cartella / ISO: imposta la directory su **`dev_hdd0/GAMES`** o **`dev_hdd0/PS3ISO`**.
5. A trasferimento completato, installa dal menu XMB in **Gioco** ➔ **Package Manager** ➔ **Install Package Files** ➔ **Standard** (oppure carica la ISO con webMAN-MOD).

---

### 4. PlayStation Vita con VitaShell

1. Avvia l'homebrew **VitaShell** sulla PS Vita.
2. Premi il tasto **SELECT** per avviare il server FTP (se configurato su modalità FTP nelle opzioni di VitaShell con il tasto START).
3. VitaShell mostrerà a video l'IP e la porta (solitamente porta **`1337`**).
4. Imposta la cartella desiderata in P2C Downloader (es. **`ux0:/vpk`** o **`ux0:/data`**).
5. Trasferisci qualsiasi pacchetto `.vpk`, ROM o file multimediale. Al termine, installa il `.vpk` direttamente da VitaShell.

---

### 5. Nintendo Switch con ftpd

1. Avvia l'homebrew **ftpd** (o **ftpd-pro**) dal menu Homebrew Launcher (Atmosphere).
2. Sullo schermo della Switch apparirà l'indirizzo IP e la porta in ascolto (solitamente porta **`5000`**).
3. Nelle Impostazioni di P2C Downloader:
   - Imposta **FTP Host** sull'IP della Switch.
   - Imposta **FTP Port** su **`5000`**.
   - Imposta **Directory** sul percorso della microSD desiderato (es. **`switch/`**, **`install/`** o la root della scheda).
4. Invia qualsiasi file `.nsp`, `.xci`, ROM o payload. Installalo quindi con il tuo title manager preferito (Tinfoil, DBI, Awoo Installer, Goldleaf).

---

### 6. Xbox e Altri Sistemi con Server FTP

P2C Downloader è compatibile con qualsiasi server FTP conforme agli standard RFC 959 / 3659:
* **Xbox Classic / Xbox 360** (con dashboard modificate come UnleashX, Avalaunch, Aurora o FSD): porta standard **`21`**.
* **Dispositivi Raspberry Pi, PC Home Server, RetroPie, Batocera**: con server `vsftpd` o `proftpd`.

---

## Installazione e Avvio su Linux

### Metodo 1: Pacchetto precompilato `.deb` (Consigliato per Ubuntu, Debian, Mint, Zorin)

Scarica il file `.deb` più recente dalla sezione [Releases](../../releases) o compilalo localmente, quindi installalo con:
```bash
sudo apt install -y ./p2c-downloader_1.3.5_amd64.deb
```
L'applicazione si integrerà automaticamente nel menu di sistema tra le categorie Giochi e Rete. Puoi avviarla dall'elenco applicazioni o digitando da terminale:
```bash
p2c-downloader
```

### Metodo 2: Compilazione dai sorgenti

Installa le librerie di sviluppo richieste:
```bash
sudo apt update
sudo apt install -y build-essential libgtk-4-dev libcurl4-openssl-dev libjson-glib-dev 7zip
```
Clona il repository ed effettua la compilazione:
```bash
git clone https://github.com/Lotverp/P2C-Downloader.git
cd P2C-Downloader/gtk-app
make
./p2c-downloader
```

### Configurazione del Risolutore Anti-Bot (Consigliato)
Per sbloccare automaticamente i link protetti da timer, Cloudflare e CutCaptcha:
```bash
pip install --user undetected-chromedriver selenium
```
> [!TIP]
> L'applicazione include un tasto dedicato nelle **Impostazioni** (**`Setup Dipendenze Undetected ChromeDriver`**) che esegue questa configurazione in autonomia con un solo clic. Assicurati che sul tuo PC sia installato **Google Chrome** oppure **Chromium** (`sudo apt install -y chromium-browser`).

---

## Guida Completa all'Interfaccia Grafica

Di seguito viene illustrata ogni schermata dell'applicazione con la spiegazione precisa di comandi, opzioni e pulsanti.

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│  [+ Nuovo Download] [Archivio Diviso]      [Download] [Catalogo] [Giochi]  [Sorgenti] ⚙ │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                        │
│                               SCHERMATA SELEZIONATA                                    │
│                                                                                        │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

---

### 4.1 Barra Superiore (Header Bar)

* **Tasto `+ Nuovo Download`**: Apre la finestra modale per inserire un link di download singolo (URL diretto, link hoster, pagina gioco o file locale presente sul PC).
* **Tasto `Archivio Diviso`**: Apre la schermata dedicata ai giochi o file compressi suddivisi in più parti (`.part1.rar`, `.part2.rar`, `.zip`, `.7z`...).
* **Selettore Principale a Schede**:
  * **`Download`**: Mostra la dashboard dei trasferimenti in corso, con barre di avanzamento, velocità e statistiche in tempo reale.
  * **`Catalogo`**: Apre lo store degli homebrew e delle utility pronte all'installazione rapida sulla console.
  * **`Giochi DLPS`**: Apre il catalogo interattivo con oltre 6.500 titoli, copertine, filtri per piattaforma/regione e ricerca rapida.
* **Tasto `Sorgenti`**: Apre la gestione dei repository JSON da cui vengono lette le applicazioni dello store homebrew.
* **Tasto `Impostazioni` (icona ingranaggio ⚙)**: Apre il pannello di configurazione per definire l'IP della console, la porta FTP, i percorsi remoti, il token Real-Debrid e le opzioni del browser anti-bot.
* **Tasto `GitHub`**: Apre la pagina ufficiale del progetto nel browser web predefinito.

---

### 4.2 Finestra: "+ Nuovo Download"

Consente di avviare qualsiasi trasferimento diretto verso la console.

```
┌──────────────────────────────────────────────────────────────────────┐
│  Nuovo Trasferimento                                                 │
├──────────────────────────────────────────────────────────────────────┤
│  URL File o Percorso Locale:                                         │
│  [ https://... o percorso file                          ] [Sfoglia..]│
│  (Rilevamento automatico tipo link, estensione o pagina gioco)       │
│                                                                      │
│  Nome file destinazione:                                             │
│  [ MioFile.iso                                          ]            │
│  Lascia vuoto per usare il nome originario. Estensione automatica.   │
│                                                                      │
│  Password (solo se archivio compresso .rar/.zip):                    │
│  [ DLPSGAME.COM                                        ▼]            │
│                                                                      │
│  Destinazione → ftp://192.168.1.111:2121/data/pkg/                   │
├──────────────────────────────────────────────────────────────────────┤
│                                           [Annulla] [Avvia Download] │
└──────────────────────────────────────────────────────────────────────┘
```

#### Dettaglio dei campi:
1. **Casella `URL File o Percorso Locale`**:
   * **Link Diretto**: Incolla qualsiasi URL che punti direttamente a un file (`.pkg`, `.iso`, `.bin`, `.zip`, `.xci`, `.nsp`, ecc.). L'app avvia istantaneamente lo streaming dal ring-buffer in RAM alla console.
   * **Link Hoster**: Incolla collegamenti di Mediafire, 1Fichier, Pixeldrain o Datanodes. L'app risolve lo stream sottostante e lo convoglia alla console.
   * **Container Protetti**: Incolla link protetti da captcha (es. FileCrypt). L'app attiva il motore UC Resolver per estrarre il mirror migliore.
   * **Auto-Rilevamento Pagine Web**: Incollando l'indirizzo di una pagina informativa o di un aggregatore (es. DLPSGame), l'app riconosce il link, estrae i mirror e avvia il download senza interventi manuali.
   * **File Locale**: Puoi digitare o selezionare il percorso di un file già presente sul tuo computer.
2. **Tasto `Sfoglia...`**: Apre il selettore file nativo per scegliere un file sul disco del PC e inviarlo direttamente via FTP alla console.
3. **Casella `Nome file destinazione`**: Nome con cui memorizzare il file sulla console. Se lasciata vuota, il nome viene ricavato automaticamente dall'URL o dall'intestazione HTTP `Content-Disposition`.
4. **Casella / Menu `Password`**: Password di scompattazione se il file è un archivio protetto.
5. **Etichetta `Destinazione → ftp://...`**: Anteprima visiva dell'indirizzo IP, porta e directory remota configurati.
6. **Tasto `Avvia Download`**: Inizia lo streaming asincrono.

---

### 4.3 Finestra: "Archivio Diviso" (Split Archive)

Molti archivi e portali distribuiscono giochi di grandi dimensioni suddivisi in molteplici volumi compressi (es. `gioco.part1.rar`, `gioco.part2.rar`... fino a decine di parti). Questa schermata automatizza l'intero ciclo di vita:

1. **Download Sequenziale o Parallelo**: Scarica le parti nella cartella temporanea locale (`Temp Dir`).
2. **Estrazione con 7-Zip**: Scompatta automaticamente l'archivio multi-volume.
3. **Individuazione Intelligente del Payload**: Riconosce automaticamente il file principale estratto (che sia un pacchetto `.pkg`, un'immagine disco `.iso`, un binario `.bin` o qualsiasi altro formato) e ne adotta la relativa estensione.
4. **Upload FTP verso la Console**: Trasferisce il file finale sulla console.
5. **Auto-Pulizia Completa**: Appena l'upload termina con successo, l'app **cancella all'istante tutti i file RAR scaricati e i file estratti temporanei**, restituendo il 100% dello spazio sul PC.

---

### 4.4 Scheda: "Sfoglia Giochi" (Catalogo DLPSGame)

Offre una panoramica visuale e completa di oltre **6.500 giochi** per console moderne e retro-console.

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│ [ Cerca gioco o CUSA/PPSA...  ] [Piattaforma: PS4 ▼] [Regione: EUR ▼] [ Cerca ] [🔄]   │
├────────────────────────────────────────────────────────────────────────────────────────┤
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │    COPERTINA     │  │    COPERTINA     │  │    COPERTINA     │  │    COPERTINA    │ │
│  │   The Witcher 3  │  │   Bloodborne     │  │   Elden Ring     │  │   Tekken 8      │ │
│  │ [CUSA05555][EUR] │  │ [CUSA03173][EUR] │  │ [CUSA28863][EUR] │  │ [PPSA10001][EUR]│ │
│  │ [Scarica Subito] │  │ [Scarica Subito] │  │ [Scarica Subito] │  │ [Scarica Subito]│ │
│  │   [ Dettagli ]   │  │   [ Dettagli ]   │  │   [ Dettagli ]   │  │   [ Dettagli ]  │ │
│  └──────────────────┘  └──────────────────┘  └──────────────────┘  └─────────────────┘ │
├────────────────────────────────────────────────────────────────────────────────────────┤
│  [◀ Precedente]            Pagina 1 di 540 • 6.480 giochi             [Successivo ▶]   │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

#### Filtri e Controlli:
* **Casella di Ricerca**: Ricerca per titolo o codice identificativo (es. `CUSA`, `PPSA`, Title ID).
* **Filtro Piattaforma**:
  * Tutte le Console
  * PlayStation 5 (PS5)
  * PlayStation 4 (PS4)
  * PlayStation 3 (PS3)
  * PlayStation 2 (PS2)
  * PlayStation 1 (PS1 / PSX)
  * PlayStation Portable (PSP)
  * PlayStation Vita (PS Vita)
  * Nintendo Switch
  * Xbox
  * PC Games
* **Filtro Regione**: EUR, USA, JPN/ASIA, Tutte le Regioni.
* **Tasto `Scarica Subito`**: Avvia l'algoritmo 1-Click: seleziona il mirror migliore, bypassa eventuali protezioni e avvia lo streaming alla console senza passaggi intermedi.
* **Tasto `Dettagli`**: Apre la finestra avanzata con screenshot, mirror dettagliati e opzioni Debrid.

---

### 4.5 Finestra: "Dettagli Gioco" e Selezione Mirror

* **Metadati e Copertine**: Dettagli su genere, anno, codice identificativo e password dell'archivio (con tasto per copiarla).
* **Tasto `Scarica Automatico su Console`**: Attiva la cascata intelligente che prova i vari hoster in ordine di qualità e velocità.
* **Elenco Link & Hoster**:
  * `DIRETTO`: Tasto **`Scarica Diretto`**.
  * `MEDIAFIRE`: Tasto **`Risolvi e Scarica`** (estrae lo stream in 0.6 secondi) e tasto **`Apri nel Browser`**.
  * `1FICHIER`: Tasto **`Risolvi 1Fichier`**, tasto **`Debrid 1-Click`** e **`Apri Pagina`**.
  * `PROTETTO` / `HOSTER`: Sblocco con UC Resolver, Debrid o browser.
* **Sezione "Avvia Download Diretto verso Console"**: Casella per incollare link diretti generati esternamente e avviarli all'istante verso la console.

---

### 4.6 Scheda: "Tools" (Store Homebrew & Applicazioni)

Consente di installare sulla console gli strumenti e gli homebrew essenziali con un clic:
* **Itemzflow Game Manager**: Backup loader avanzato con cover 3D e patch.
* **Apollo Save Tool**: Gestore completo di salvataggi, trofei e trucchi.
* **PS4-Xplorer / File Manager**: Gestione del file system della console.
* **GoldHEN Cheats Manager**: Cheat e trainer a 60 FPS.
* **RetroArch**: Frontend universale di emulazione.

---

### 4.7 Finestra: "Gestione Sorgenti" (Repository Feed)

Permette di aggiungere o rimuovere feed JSON personalizzati della community per ampliare il catalogo degli homebrew e delle utility installabili.

---

### 4.8 Scheda: "Download Attivi" (Monitoraggio e Coda)

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│  Download attivi: 1    •    Velocità totale: 52.4 MB/s    •    Avanzamento: 65%        │
├────────────────────────────────────────────────────────────────────────────────────────┤
│  MioGioco_Aggiornato.iso                                                               │
│  [Download in corso...]                                                                │
│  [█████████████████████████████████████████░░░░░░░░░░░░] 65%                           │
│  24.8 GB / 38.2 GB  •  Velocità: 52.4 MB/s  •  ETA: 4 min 12 sec           [Annulla]   │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

* **Statistiche Globali**: Velocità aggregata di rete verso la console (MB/s), percentuale complessiva e numero di download.
* **Stati del Trasferimento**:
  * `In coda`: In attesa del proprio turno.
  * `Download in corso...`: Pure streaming attivo tra Internet, RAM e console.
  * `Estrazione archivio...`: Decompressione locale delle parti multi-volume.
  * `Invio via FTP...`: Caricamento del payload estratto verso la console.
  * `Completato`: File integralmente trasferito e pronto sulla console.
  * `Errore`: Segnalazione dettagliata dell'anomalia (es. spazio esaurito sulla console o timeout).

---

### 4.9 Finestra: "Impostazioni" (FTP Settings e Anti-Bot)

Accessibile tramite l'icona dell'ingranaggio **⚙**:

```
┌──────────────────────────────────────────────────────────────────────┐
│  FTP Settings                                                        │
├──────────────────────────────────────────────────────────────────────┤
│  FTP Host                                                            │
│  [ 192.168.1.111                                                   ] │
│  FTP Port                                                            │
│  [ 2121                                                            ] │
│  Directory                                                           │
│  [ data/pkg                                                        ] │
│  Username                                                            │
│  [ anonymous                                                       ] │
│  Password                                                            │
│  [                                                                 ] │
│  Temp Dir (Arch)                                                     │
│  [ /home/utente/.cache                                             ] │
│  Saved Passwords                                                     │
│  [ DLPSGAME.COM, dlpsgame.com                                      ] │
│  Real-Debrid Token (Opzionale)                                       │
│  [ ****************************************************************] │
│                                                                      │
│  [ Test Connessione & Crea Cartella ]                                │
│                                                                      │
│  ── Undetected ChromeDriver (Automazione Bot / Link Protetti) ────── │
│  Stato: Python OK, Selenium OK, UC OK, Chrome OK                     │
│  [X] Abilita Undetected ChromeDriver (Download Automatico / 1-Click) │
│  [X] Modalità Headless (avvia browser in background)                 │
│  [ Setup Dipendenze Undetected ChromeDriver ]                        │
├──────────────────────────────────────────────────────────────────────┤
│                                                      [Cancel] [Save] │
└──────────────────────────────────────────────────────────────────────┘
```

#### Descrizione opzioni:
* **`FTP Host`**: Indirizzo IP locale assegnato dal router alla console (es. `192.168.1.111`).
* **`FTP Port`**: Porta FTP della console:
  * PS4 GoldHEN: **`2121`**
  * PS5 etaHEN: **`1337`** o **`2121`**
  * PS3 webMAN-MOD: **`21`**
  * PS Vita VitaShell: **`1337`**
  * Nintendo Switch ftpd: **`5000`**
* **`Directory`**: Cartella remota sulla console (es. `data/pkg`, `dev_hdd0/packages`, `switch/`, ecc.).
* **`Username` / `Password`**: Credenziali FTP (di default `anonymous` con password vuota).
* **`Temp Dir (Arch)`**: Cartella locale temporanea utilizzata solo per la scompattazione di archivi multi-volume prima dell'invio.
* **`Test Connessione & Crea Cartella`**: Verifica all'istante la raggiungibilità della console e crea da remoto la cartella di destinazione se non esiste.
* **`Setup Dipendenze Undetected ChromeDriver`**: Configura con un clic l'ambiente Python e i moduli anti-bot.

---

## Tecnologia Anti-Bot: Undetected ChromeDriver

I siti aggregatori proteggono spesso i propri mirror con sistemi di verifica bot avanzati (Cloudflare Turnstile, CutCaptcha, countdown forzati o link shortener). I tradizionali scraper falliscono sistematicamente su queste pagine.

**P2C Downloader** integra un motore intelligente headless basato su Selenium e ChromeDriver non rilevabile (`gtk-app/uc_resolver.py`):

```
Link Protetto ──► [UC Resolver] ──► Blocco Popunder ──► Evasione Honeypot ──► Input Biometrico CDP ──► Stream Diretto Console
```

1. **Blocco Totale dei Popunder**: I popup pubblicitari ingannevoli vengono bloccati a livello del browser con `"profile.default_content_setting_values.popups": 2`, e le schede pubblicitarie ad apertura imprevista vengono chiuse all'istante.
2. **Evasione Honeypot**: Riconosce i link trappola invisibili inseriti dai siti per bloccare i bot automatici e interagisce solo con i pulsanti reali.
3. **Eventi Hardware CDP**: Le interazioni con i captcha non avvengono tramite JavaScript iniettato, ma mediante comandi hardware simulati con Chrome DevTools Protocol (`Input.dispatchMouseEvent`) con coordinate fisiche realistiche ed eventi `isTrusted: true`.
4. **Clipboard Watcher**: Nel caso eccezionale di protezioni con interazione visiva manuale, l'app monitora gli appunti: non appena copi il link finale nel browser, **P2C Downloader lo intercetta da solo e avvia subito lo streaming verso la console!**

---

## Integrazione Real-Debrid

Se possiedi un account Real-Debrid:
1. Visita [real-debrid.com/apitoken](https://real-debrid.com/apitoken) e copia il tuo token API personale.
2. Nelle **Impostazioni** di P2C Downloader, incolla il token nel campo `Real-Debrid Token` e premi **Save**.
3. **Vantaggi**:
   * **Bypass del limite 1Fichier**: 1Fichier impone agli utenti non paganti attese forzate di 60-90 minuti. Con Real-Debrid configurato, P2C Downloader genera istantaneamente il link diretto ad alta velocità in meno di 0.3 secondi!
   * **Velocità Piena Multi-Hoster**: Sblocca link Rapidgator, Mega, Datanodes e 1Fichier alla massima velocità della tua connessione Internet.

---

## Consigli per la Massima Velocità di Trasferimento

La velocità di streaming verso la console dipende dalla connessione locale tra il tuo PC e la console:

| Tipo di Connessione | Velocità Media Tipica | Tempo stimato per 40 GB |
|---|---|---|
| **Cavo Ethernet Gigabit LAN (Consigliato)** | **80 – 115 MB/s** | **~ 6 – 8 minuti** |
| Wi-Fi 5 GHz (segnale eccellente) | 25 – 45 MB/s | ~ 15 – 25 minuti |
| Wi-Fi 2.4 GHz | 8 – 15 MB/s | ~ 45 – 80 minuti |

> [!TIP]
> **Consiglio per il massimo delle prestazioni**: Per saturare la banda Gigabit e la velocità di scrittura del disco della console, collega sia il PC sia la console al router con un **cavo di rete Ethernet LAN (Cat 5e, Cat 6 o superiore)**.

---

## Script Standalone da Terminale (CLI)

Per distribuzioni Linux server headless (senza interfaccia grafica) o per chi gestisce i trasferimenti da remoto tramite SSH, il repository include lo script standalone bash [`p2c-download.sh`](p2c-download.sh):

```bash
# Rendi lo script eseguibile
chmod +x p2c-download.sh

# Avvia il download streaming interattivo
./p2c-download.sh
```

### Dipendenze richieste per lo script CLI:
```bash
sudo apt install -y curl wget pv
```
Lo script consente di configurare la destinazione tramite le variabili d'ambiente `FTP_HOST`, `FTP_PORT` e `FTP_DIR`:
```bash
FTP_HOST=192.168.1.150 FTP_PORT=5000 FTP_DIR="switch/" ./p2c-download.sh
```

---

## Risoluzione dei Problemi e Domande Frequenti (FAQ)

### 1. Il programma restituisce "Errore di connessione FTP" o va in timeout
* **Verifica che la console sia accesa**: Il server FTP della console si interrompe se la console entra in modalità riposo o viene spenta.
* **Verifica che il server FTP sia abilitato**: Controlla le impostazioni dell'exploit o dell'homebrew sulla console (es. GoldHEN su PS4, etaHEN su PS5, ftpd su Switch, VitaShell su PS Vita).
* **Verifica IP e Porta**: Assicurati che l'IP e la porta inseriti nelle Impostazioni corrispondano a quelli mostrati dalla console.
* **Firewall Linux**: Se hai un firewall attivo sul computer (`ufw`), consenti il traffico verso la porta della console:
  ```bash
  sudo ufw allow out to any port 2121 proto tcp
  ```

### 2. Posso scaricare file diversi dai .pkg?
* **Certamente!** P2C Downloader supporta in streaming qualsiasi estensione di file: `.iso`, `.bin`, `.xci`, `.nsp`, `.vpk`, `.zip`, `.7z`, ROM, aggiornamenti, file di configurazione, ecc. È sufficiente specificare la cartella di destinazione corretta per la tua console.

### 3. Compare il messaggio "File rimosso" o "Errore 404"
* Sui siti di hosting gratuiti alcuni file possono essere cancellati nel tempo per inattività o copyright.
* Apri la finestra dei **Dettagli** del gioco e prova un mirror alternativo (es. se Mediafire è offline, seleziona 1Fichier o Debrid). Il tasto **`Scarica Automatico su Console`** testerà automaticamente i mirror funzionanti.

### 4. Il risolutore Undetected ChromeDriver segnala che manca Chrome o Chromium
* Installa Chromium o Google Chrome sul tuo sistema Linux:
  ```bash
  sudo apt install -y chromium-browser
  ```
  Nelle Impostazioni premi **`Setup Dipendenze Undetected ChromeDriver`** per completare la configurazione.

### 5. Il programma consuma spazio su disco sul mio PC?
* **Per tutti i download diretti**: **Zero byte**. I dati fluiscono direttamente dalla scheda di rete alla RAM del computer e da questa alla console via FTP. Nessun file temporaneo viene salvato su disco.
* **Per gli archivi divisi multi-volume (.part1.rar)**: Le parti vengono scaricate nella directory temporanea per permettere a 7-Zip di estrarle. Non appena il file estratto è stato caricato sulla console, tutti i file temporanei vengono **automaticamente cancellati**.

---

## Licenza

Questo progetto è distribuito sotto licenza open source **MIT**. Consulta il file [LICENSE](LICENSE) per i termini completi.

<div align="center">
Realizzato con passione per la community Console & Homebrew.
</div>
