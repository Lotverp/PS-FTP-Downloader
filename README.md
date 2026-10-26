<div align="center">

# PS FTP Downloader

**Download diretto di file PKG da Internet alla PlayStation in Pure Streaming — Zero spazio occupato sul PC.**

[![License: MIT](https://img.shields.io/badge/License-MIT-violet.svg)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![GTK: 4](https://img.shields.io/badge/GTK-4-green.svg)](https://gtk.org/)
[![Built with: C](https://img.shields.io/badge/Built%20with-C-orange.svg)]()

</div>

---

## Panoramica del Progetto

**PS FTP Downloader** è un'applicazione nativa per Linux (scritta in C con interfaccia grafica moderna in GTK4) concepita per semplificare e velocizzare al massimo l'installazione di giochi, aggiornamenti, DLC, homebrew e archivi su console **PlayStation 4 (PS4), PlayStation 5 (PS5), PlayStation 3 (PS3) e PlayStation Vita**.

A differenza dei metodi tradizionali — che costringono a scaricare decine o centinaia di gigabyte sul disco del computer per poi copiarli su chiavette USB o inviarli via FTP in un secondo momento — PS FTP Downloader adotta una tecnologia proprietaria di **Pure RAM Ring-Buffer Streaming**:

```
┌──────────────┐         HTTP/HTTPS          ┌──────────────────────┐         FTP Socket         ┌──────────────────────┐
│   INTERNET   │ ──────────────────────────► │    RAM RING-BUFFER   │ ─────────────────────────► │     PLAYSTATION      │
│  (Direct/MF) │   Download asincrono (C)    │ (2 MB circolare atom)│    Upload asincrono (C)    │ (GoldHEN /data/pkg)  │
└──────────────┘                             └──────────────────────┘                             └──────────────────────┘
```

* **Zero spazio consumato su PC**: Anche un pacchetto da 60 GB viene trasferito senza salvare un singolo byte di file temporaneo sul disco del tuo computer.
* **Velocità massima di rete**: Il download da Internet e l'invio FTP alla PlayStation avvengono in parallelo su thread indipendenti.
* **Niente chiavette USB formattate exFAT**: Nessun limite di capienza e nessun rischio di corruzione dei file.

---

## Indice dei Contenuti
1. [Funzionalità Chiave](#funzionalità-chiave)
2. [Configurazione Dettagliata della Console](#configurazione-dettagliata-della-console)
   - [PlayStation 4 con GoldHEN](#1-configurazione-su-ps4-con-goldhen)
   - [PlayStation 5 con etaHEN](#2-configurazione-su-ps5-con-etahen)
   - [PlayStation 3 con webMAN-MOD / HEN](#3-configurazione-su-ps3-con-webman-mod--hen)
3. [Installazione e Avvio su Linux](#installazione-e-avvio-su-linux)
4. [Guida Completa all'Interfaccia Grafica](#guida-completa-allinterfaccia-grafica)
   - [4.1 Barra Superiore (Header Bar)](#41-barra-superiore-header-bar)
   - [4.2 Finestra: "+ Nuovo Download"](#42-finestra--nuovo-download)
   - [4.3 Finestra: "Archivio Diviso" (Split Archive .rar)](#43-finestra-archivio-diviso-split-archive)
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

* **Architettura Pure Streaming in RAM**: Un buffer circolare atomico in memoria (circa 2 MB) fa da ponte tra il download HTTP e l'upload FTP alla console. Nessuna usura dell'SSD e nessun file residuo.
* **Catalogo Giochi Integrato (oltre 6.500 titoli)**: Sfoglia, cerca per nome o CUSA/PPSA e scarica giochi direttamente dall'app con copertine, generi e screenshot.
* **Algoritmo di Download Automatico Intelligente (1-Click)**: L'applicazione seleziona in autonomia la sorgente migliore con ordine di preferenza: Link Diretto PKG > Mediafire > 1Fichier > Real-Debrid > Container Protetti.
* **Motore Anti-Bot Undetected ChromeDriver**: Bypass automatico in background di countdown timer, controlli Cloudflare e captcha (CutCaptcha con eventi mouse fisici simulati via CDP).
* **Protezione da Pubblicità Invasiva e Popunder**: Blocco completo a livello di browser di finestre popup ingannevoli e chiusura istantanea di schede ad clickjacking.
* **Supporto Debrid 1-Click**: Risoluzione immediata dei link 1Fichier, Rapidgator e Mega tramite API Real-Debrid, aggirando i tempi di attesa IP forzati per gli utenti non paganti.
* **Auto-Resume dei Trasferimenti**: Se la connessione di rete cade temporaneamente o l'FTP si riavvia, l'app riprende il trasferimento dal byte esatto in cui si era interrotto (`Range: bytes` + `REST`).
* **Gestore Archivi Divisi Multi-Volume (.part1.rar)**: Scarica automaticamente le parti, ne estrae il file `.pkg` con `7zip` locale, lo invia alla PlayStation ed elimina all'istante i file temporanei.
* **App & Homebrew Store**: Installazione rapida con un clic degli homebrew più famosi (Itemzflow, Apollo Save Tool, PS4-Xplorer, GoldHEN Cheats Manager) con feed JSON espandibili.

---

## Configurazione Dettagliata della Console

Affinché l'applicazione possa inviare i file alla PlayStation, la console e il computer devono essere collegati alla **stessa rete locale (stessa LAN o Wi-Fi)** e il server FTP della console deve essere attivo.

### 1. Configurazione su PS4 con GoldHEN

GoldHEN integra nativamente un server FTP robusto e performante (`ftpsrv`).

#### Passo 1: Attivare il server FTP su PS4
1. Accendi la PS4 e avvia l'exploit GoldHEN (firmware 5.05, 6.72, 7.02, 7.55, 9.00, 11.00 o superiori).
2. Dalla dashboard della PS4, entra in **Impostazioni**.
3. In cima alla schermata seleziona la voce **GoldHEN** (oppure entra in **Debug Settings**).
4. Entra nel sottomenu **Server Settings**.
5. Trova la voce **Enable FTP Server** e **spunta la casella**.
6. Non appena attivato, apparirà una notifica in alto a sinistra del tipo:
   ```text
   FTP Server: 192.168.1.111:2121
   ```
   * **Indirizzo IP**: È l'indirizzo della tua console (es. `192.168.1.111`).
   * **Porta FTP**: GoldHEN utilizza come porta predefinita la **`2121`** (e NON la 21 standard, per evitare conflitti di privilegi).

#### Passo 2: La cartella di destinazione `/data/pkg`
Il Package Installer di GoldHEN cerca i file `.pkg` da installare all'interno della cartella interna:
```text
/data/pkg
```
* **Creazione automatica**: Non devi preoccuparti di crearla a mano: aprendo le **Impostazioni** di PS FTP Downloader e premendo il tasto **`Test Connessione & Crea Cartella`**, il programma si connette alla console e crea automaticamente la cartella `/data/pkg` se non è ancora presente!
* **Creazione manuale (alternativa)**: Puoi crearla anche usando un homebrew file manager sulla PS4 (come *PS4-Xplorer* o *Itemzflow*), creando la sottocartella `pkg` dentro `/data`.

#### Passo 3: Come installare il gioco sulla PS4 al termine del download
1. Quando PS FTP Downloader segnala **COMPLETATO** (100%), il file `.pkg` si trova già interamente nella memoria della console.
2. Sulla PS4 vai in:
   * **Impostazioni** ➔ **GoldHEN** ➔ **Package Installer** (oppure **Debug Settings** ➔ **Game** ➔ **Package Installer**).
3. Vedrai comparire il file del gioco appena trasferito.
4. Seleziona il pacchetto e premi **X** sul controller: la PS4 installerà il gioco in pochi secondi.
5. Al termine dell'installazione, l'icona del gioco apparirà sulla schermata principale della PS4 pronta per essere avviata.

---

### 2. Configurazione su PS5 con etaHEN

1. Avvia l'exploit PS5 e carica l'homebrew enabler **etaHEN** (oppure invia il payload `ftpserver`).
2. Nelle impostazioni di etaHEN, assicurati che la voce **FTP Server** sia abilitata.
3. Annota l'indirizzo IP e la porta visualizzati (su PS5 la porta è solitamente **`1337`** oppure **`2121`**).
4. La cartella di installazione su PS5 è identica: `/data/pkg`.
5. A download ultimato, installa il file tramite il **Package Installer** di etaHEN.

---

### 3. Configurazione su PS3 con webMAN-MOD / HEN

1. Avvia la console con HEN o Custom Firmware (Evilnat / Rebug).
2. Assicurati che **webMAN-MOD** sia attivo (il server FTP parte automaticamente in background all'avvio della PS3).
3. La porta FTP standard di webMAN-MOD è la **`21`**.
4. La cartella standard dei pacchetti su PS3 è **`/dev_hdd0/packages`**. Nelle Impostazioni di PS FTP Downloader inserisci `dev_hdd0/packages` nel campo *Directory*.
5. A trasferimento completato, installa il file dal menu XMB della PS3 in **Gioco** ➔ **Package Manager** ➔ **Install Package Files** ➔ **Standard**.

---

## Installazione e Avvio su Linux

### Metodo 1: Pacchetto precompilato `.deb` (Consigliato per Ubuntu, Debian, Mint, Zorin)

Scarica il pacchetto `.deb` rilasciato nella sezione [Releases](../../releases) oppure compilalo localmente, quindi installalo con:
```bash
sudo apt install -y ./ps-ftp-downloader_1.3.10_amd64.deb
```
L'applicazione viene integrata nel menu di sistema tra i giochi e gli strumenti di rete. Puoi avviarla dall'elenco applicazioni o digitando da terminale:
```bash
ps-ftp-downloader
```

### Metodo 2: Compilazione dai sorgenti

Installa le dipendenze di sistema richieste per lo sviluppo GTK4 e networking:
```bash
sudo apt update
sudo apt install -y build-essential libgtk-4-dev libcurl4-openssl-dev libjson-glib-dev 7zip
```
Clona il repository ed effettua la compilazione:
```bash
git clone https://github.com/Lotverp/PS-FTP-Downloader.git
cd PS-FTP-Downloader/gtk-app
make
./ps-ftp-downloader
```

### Configurazione del Risolutore Anti-Bot (Consigliato)
Per sbloccare automaticamente i link protetti da timer, Cloudflare e CutCaptcha:
```bash
pip install --user undetected-chromedriver selenium
```
> *Nota: L'applicazione dispone anche di un tasto dedicato nelle **Impostazioni** che esegue questa installazione in automatico con un solo clic.* Assicurati che sul tuo PC sia installato **Google Chrome** oppure **Chromium** (`sudo apt install -y chromium-browser`).

---

## Guida Completa all'Interfaccia Grafica

Di seguito viene analizzata dettagliatamente ogni schermata, spiegando la funzione precisa di ciascun pulsante, campo di testo, menu a tendina e opzione.

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

* **Tasto `+ Nuovo Download`**: Apre la finestra modale per inserire un link di download singolo (URL diretto, link hoster, pagina di DLPSGame o file `.pkg` locale presente sul PC).
* **Tasto `Archivio Diviso`**: Apre la schermata dedicata ai giochi distribuiti in più parti compresse (`.part1.rar`, `.part2.rar`...).
* **Selettore Principale a Schede**:
  * **`Download`**: Mostra la dashboard dei trasferimenti in corso, con barre di avanzamento, velocità e statistiche in tempo reale.
  * **`Catalogo`**: Apre lo store degli homebrew e delle utility pronte all'installazione con 1 clic sulla console.
  * **`Giochi DLPS`**: Apre il catalogo interattivo con oltre 6.500 titoli, copertine, filtri per piattaforma/regione e ricerca rapida.
* **Tasto `Sorgenti`**: Apre la finestra di gestione dei repository JSON da cui vengono lette le applicazioni del catalogo homebrew.
* **Tasto `Impostazioni` (icona ingranaggio ⚙)**: Apre il pannello di configurazione per definire l'IP della console, la porta FTP, i percorsi, il token Real-Debrid e le opzioni del browser anti-bot.
* **Tasto `GitHub`**: Apre la pagina ufficiale del progetto nel browser web predefinito.

---

### 4.2 Finestra: "+ Nuovo Download"

Consente di aggiungere qualsiasi trasferimento manuale verso la console.

```
┌──────────────────────────────────────────────────────────────────────┐
│  Nuovo Trasferimento                                                 │
├──────────────────────────────────────────────────────────────────────┤
│  URL File o Percorso Locale:                                         │
│  [ https://... o percorso file                          ] [Sfoglia..]│
│  (Rilevamento automatico tipo link o pagina gioco)                   │
│                                                                      │
│  Nome file destinazione:                                             │
│  [ MioGioco.pkg                                         ]            │
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

#### Descrizione dettagliata dei campi e dei tasti:

1. **Casella `URL File o Percorso Locale`**:
   * **Link Diretto PKG**: Incolla qualsiasi URL che punti a un file `.pkg` (es. `https://server.com/gioco.pkg`). L'app avvia direttamente il ring-buffer in RAM.
   * **Link Hoster**: Incolla collegamenti di Mediafire, 1Fichier, Pixeldrain o Datanodes. L'app risolve lo stream sottostante e lo invia alla console.
   * **Container Protetti**: Incolla link protetti da captcha (es. `filecrypt.cc`). L'app attiva il motore UC Resolver per estrarre il mirror.
   * **Auto-Rilevamento Pagine DLPSGame**: Se incolli l'indirizzo di una pagina gioco (es. `https://dlpsgame.com/peggle-2-magical-masters-edition-ps4-pkg/`), l'app riconosce immediatamente il link, interroga il database interno del catalogo e fa partire in automatico lo sblocco del miglior mirror e il download verso la PlayStation senza alcun intervento manuale!
   * **File Locale**: Puoi digitare il percorso di un file presente sul tuo PC.
2. **Tasto `Sfoglia...`**:
   * Apre il selettore file nativo del sistema operativo per selezionare un file `.pkg`, `.rar`, `.7z` o `.zip` già presente sul disco del computer e inviarlo direttamente via FTP alla console.
3. **Casella `Nome file destinazione`**:
   * Definisce con quale nome il file verrà memorizzato nella cartella della PlayStation (es. `Bloodborne.pkg`).
   * Se lasciata vuota, il programma invia una richiesta HTTP di intestazione (`HEAD`) al server web per ricavare il nome originale del file. L'estensione `.pkg` viene garantita in automatico.
4. **Menu a tendina / Casella `Password`**:
   * Se il file da scaricare è un archivio compresso protetto da chiave, inserisci qui la password.
   * Il menu a tendina propone la cronologia delle ultime password usate (come `DLPSGAME.COM`).
5. **Etichetta `Destinazione → ftp://...`**:
   * Mostra in anteprima l'indirizzo IP, la porta e il percorso remoto su cui verrà effettuato il caricamento, per verificare al volo la corretta configurazione.
6. **Tasto `Avvia Download`**:
   * Convalida i dati, crea i thread asincroni di ricezione HTTP e invio FTP e aggiunge il processo alla scheda dei download attivi.
7. **Tasto `Annulla`**:
   * Chiude la finestra senza avviare alcuna operazione.

---

### 4.3 Finestra: "Archivio Diviso" (Split Archive)

Molti portali distribuiscono giochi di grandi dimensioni suddivisi in molteplici volumi RAR (es. `gioco.part1.rar`, `gioco.part2.rar`... fino a 10 o 20 parti da 4 GB ciascuna). Questa schermata automatizza l'intero processo.

#### Come si comporta l'app con gli archivi divisi:
1. Scarica in sequenza o in parallelo le parti nella cartella temporanea (`Temp Dir`).
2. Quando tutte le parti sono disponibili, lancia internamente `7zip` per estrarre il file `.pkg`.
3. Non appena il file `.pkg` è pronto, avvia il caricamento via FTP sulla console.
4. **Auto-Pulizia Completa**: Appena l'upload su PlayStation è terminato con successo, l'app cancella automaticamente tutti i file RAR scaricati e i file estratti temporanei, liberando all'istante lo spazio sul tuo computer.

#### Campi e pulsanti disponibili:
* **Slot `Part 1`, `Part 2`...**: Caselle individuali dove incollare i singoli URL di ciascuna parte dell'archivio.
* **Tasto `+ Add URL`**: Aggiunge una nuova riga numerata per archivi composti da 3, 4 o decine di parti.
* **Tasto `Incolla da Appunti`**: Se hai copiato un blocco di righe contenenti gli indirizzi delle varie parti, questo tasto popola automaticamente tutti gli slot con un solo clic.
* **Tasto `Sfoglia File...`**: Consente di selezionare parti già salvate in locale sul computer.
* **Casella `Destination .pkg filename`**: Il nome desiderato per il gioco estratto (es. `GranTurismo7.pkg`).
* **Casella `Archive password`**: Password dell'archivio compresso (se presente).
* **Tasto `Start` / `Avvia su PS`**: Dà inizio alla catena di download, estrazione, streaming e pulizia.

---

### 4.4 Scheda: "Sfoglia Giochi" (Catalogo DLPSGame)

Offre una panoramica visuale e completa di oltre **6.500 giochi** per console PlayStation e retro-console.

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
* **Casella di Ricerca**: Digita il titolo del gioco (es. `Spider-Man`) o il codice identificativo della versione (es. `CUSA02299`, `PPSA01456`). Premendo `Invio` o il tasto `Cerca`, i risultati compaiono istantaneamente.
* **Menu a tendina `Piattaforma`**: Permette di filtrare i giochi per sistema di appartenenza:
  * **Tutte le Piattaforme**
  * **PS5** (PlayStation 5)
  * **PS4** (PlayStation 4)
  * **PS3** (PlayStation 3)
  * **PS2** (PlayStation 2 Classics)
  * **PS1 / PSX** (Classici PlayStation 1)
  * **PSP** (PlayStation Portable)
  * **PS Vita**
  * **Nintendo Switch / Xbox / PC**
* **Menu a tendina `Regione`**: Filtra per mercato di distribuzione:
  * **Tutte le Regioni**
  * **EUR** (Europa / Multilingua con Italiano)
  * **USA** (Nord America)
  * **JPN / ASIA** (Giappone e Asia)
* **Tasto `Cerca`**: Esegue l'interrogazione filtrata.
* **Tasto `Aggiorna` (🔄)**: Riscarica l'indice più recente dal catalogo remoto.
* **Pulsanti di Paginazione (`◀ Precedente` / `Successivo ▶`)**: Naviga tra le pagine del catalogo con contatore totale di titoli e pagine.
* **Tasto `Torna ai Download`**: Torna istantaneamente alla scheda dei trasferimenti attivi.

#### Azioni sulle Card dei Giochi:
* **Tasto `Scarica Subito`**: Avvia immediatamente l'algoritmo intelligente di download 1-Click: seleziona il mirror migliore, bypassa le protezioni e comincia a trasferire il file sulla console senza mostrare ulteriori dialoghi.
* **Tasto `Dettagli`**: Apre la finestra informativa avanzata del titolo.

---

### 4.5 Finestra: "Dettagli Gioco" e Selezione Mirror

Si apre cliccando su un gioco per esaminarne i dettagli e scegliere manualmente la sorgente desiderata.

* **Banner e Screenshot**: Mostra la copertina e una galleria di screenshot promozionali ad alta definizione.
* **Metadati**: Riporta il genere (es. *Action-RPG*), l'anno di uscita, la regione, il codice Title ID (`CUSAxxxxx`) e l'eventuale password dell'archivio.
* **Tasto `Copia Password`**: Copia con un clic la password del gioco (es. `DLPSGAME.COM`) negli appunti di sistema.
* **Tasto `Scarica Automatico su PS`**:
  * Il pulsante più potente della schermata: avvia la cascata di risoluzione intelligente che testa i link disponibili in ordine di affidabilità e velocità, risolve i captcha se presenti e fa partire lo stream verso la console.
* **Tasto `Apri Pagina Web`**: Apre la pagina ufficiale del gioco sul sito aggregatore nel browser predefinito.
* **Sezione Mirror & Hoster (Elenco Link)**:
  * Ciascun link disponibile è catalogato con un'etichetta visiva colorata:
    * `DIRETTO`: Link diretto a un file `.pkg`. Include il tasto **`Scarica .PKG`**.
    * `MEDIAFIRE`: Hosting ad altissima velocità. Include il tasto **`Risolvi e Scarica`** (estrae lo stream diretto in 0.6 secondi) e il tasto **`Apri nel Browser`**.
    * `1FICHIER`: Hosting popolare. Include il tasto **`Risolvi 1Fichier`**, il tasto **`Debrid 1-Click`** (se configurato Real-Debrid) e **`Apri Pagina`**.
    * `PROTETTO` / `HOSTER`: Link protetto da container (es. FileCrypt) o link shortener. Include il tasto **`Sblocca con UC`** (avvia Undetected ChromeDriver), **`Debrid 1-Click`**, **`Apri Pagina`** e **`Copia`**.
* **Sezione "Avvia Download Diretto .PKG verso PlayStation"**:
  * Permette di incollare un link sbloccato manualmente o generato esternamente. Dispone del tasto **`Incolla da Appunti`**, del tasto **`Sfoglia File...`** e del tasto **`Avvia su PS`**.

---

### 4.6 Scheda: "Tools" (Store Homebrew & Applicazioni)

Consente di installare sulla PlayStation gli strumenti e gli homebrew essenziali senza dover cercare i file PKG su Internet:

* **Applicazioni incluse**:
  * **Itemzflow Game Manager**: Il più celebre backup loader e dashboard alternativa con supporto a cover 3D, trailer e patch 60 FPS.
  * **Apollo Save Tool**: Strumento indispensabile per gestire, sbloccare, esportare e scaricare salvataggi di gioco e trofei.
  * **PS4-Xplorer**: File manager completo per esplorare l'hard disk interno della console, dispositivi USB e partizioni di sistema.
  * **GoldHEN Cheats Manager**: Gestore e scaricatore automatico di cheat code e patch a 60 FPS per migliaia di giochi PS4.
  * **RetroArch**: Frontend universale per emulare PS1, PSP, SNES, Megadrive, GBA, Arcade e decine di altri sistemi.
* **Tasto `Installa`**: Ogni applicazione ha un pulsante dedicato che scarica l'ultima versione ufficiale rilasciata dagli sviluppatori e la invia direttamente via streaming FTP alla console.
* **Tasto `Aggiorna`**: Ricarica la lista dei tool dai repository ufficiali.
* **Tasto `Sorgenti`**: Permette di aggiungere repository personalizzati.

---

### 4.7 Finestra: "Gestione Sorgenti" (Repository Feed)

Accessibile premendo il tasto **`Sorgenti`** nell'header bar o nella scheda dei Tools.
* **Elenco Sorgenti Attive**: Mostra gli indirizzi dei file feed JSON attualmente caricati.
* **Casella di Testo per Nuovo Repository**: Inserisci l'URL di un feed JSON compatibile (creato da te o dalla community).
* **Tasto `Aggiungi`**: Memorizza la nuova sorgente nell'elenco.
* **Tasto `Rimuovi`**: Elimina un repository selezionato.
* **Tasto `Aggiorna Tools Adesso`**: Interroga immediatamente tutte le sorgenti attive e aggiorna l'elenco delle applicazioni disponibili.

---

### 4.8 Scheda: "Download Attivi" (Monitoraggio e Coda)

La centrale di controllo dove seguire l'avanzamento dei trasferimenti in corso:

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│  Download attivi: 1    •    Velocità totale: 48.2 MB/s    •    Avanzamento: 65%        │
├────────────────────────────────────────────────────────────────────────────────────────┤
│  Bloodborne_GOTY_v1.09.pkg                                                             │
│  [Download in corso...]                                                                │
│  [█████████████████████████████████████████░░░░░░░░░░░░] 65%                           │
│  24.8 GB / 38.2 GB  •  Velocità: 48.2 MB/s  •  ETA: 4 min 38 sec           [Annulla]   │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

#### Elementi della Dashboard:
* **Barra di Riepilogo Globale**:
  * Numero di download simultanei attivi.
  * **Velocità aggregata di trasmissione** verso la PlayStation espressa in MB/s.
  * Percentuale complessiva della coda.
* **Scheda del Singolo Trasferimento**:
  * **Titolo del Gioco e Nome File**: L'identificativo esatto del pacchetto in trasferimento.
  * **Badge di Stato Dinamico**:
    * `In coda`: Il trasferimento attende che uno slot simultaneo si liberi.
    * `Download in corso...`: Streaming attivo tra Internet, RAM e PlayStation.
    * `Estrazione archivio...`: Decompressione locale delle parti RAR in corso.
    * `Invio FTP...`: Upload del file estratto verso la console.
    * `Completato`: Il file è stato inviato al 100% ed è pronto per l'installazione su PS4/PS5.
    * `Errore`: Segnala eventuali problemi (connessione persa, spazio su disco console esaurito, ecc.).
  * **Barra di Avanzamento Grafica**: Indicatore percentuale fluido.
  * **Statistiche di Trasferimento**: Dimensione trasferita rispetto al totale (es. `12.4 GB / 40.1 GB`).
  * **Velocità Istantanea ed ETA**: Velocità attuale di upload (MB/s) e stima del tempo rimanente al completamento (minuti e secondi).
  * **Tasto `Annulla`**: Interrompe immediatamente il trasferimento, chiude i socket FTP e libera la memoria RAM.
  * **Tasto `Rimuovi`**: Pulisce la card dall'elenco una volta completato il trasferimento.

---

### 4.9 Finestra: "Impostazioni" (FTP Settings e Anti-Bot)

Si apre cliccando sull'icona dell'ingranaggio **⚙** nella barra superiore. Questa finestra racchiude la configurazione fondamentale dell'applicazione:

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

#### Spiegazione di ogni singolo campo e tasto:

1. **`FTP Host`**:
   * L'indirizzo IP locale assegnato dal router alla tua PlayStation (es. `192.168.1.111`).
   * *Consiglio*: Imposta un indirizzo IP statico o riserva DHCP sul router per la PlayStation, così che l'indirizzo rimanga sempre identico.
2. **`FTP Port`**:
   * La porta su cui il server FTP della console è in ascolto.
   * Per **PS4 con GoldHEN**: inserisci **`2121`**.
   * Per **PS5 con etaHEN**: inserisci **`1337`** o **`2121`**.
   * Per **PS3 con webMAN-MOD**: inserisci **`21`**.
3. **`Directory`**:
   * La cartella remota della console in cui depositare i file `.pkg`.
   * Per **PS4 e PS5**: imposta **`data/pkg`** (è la cartella standard scansionata dal Package Installer di GoldHEN ed etaHEN).
   * Per **PS3**: imposta **`dev_hdd0/packages`**.
4. **`Username`**:
   * Il nome utente per l'accesso FTP. Di default è **`anonymous`** (GoldHEN non richiede credenziali speciali).
5. **`Password`**:
   * La password per l'accesso FTP (lascia vuoto per GoldHEN).
6. **`Temp Dir (Arch)`**:
   * La cartella sul tuo computer utilizzata temporaneamente durante il download e l'estrazione degli archivi `.rar` multi-volume. Di default usa la cache utente (`~/.cache`).
7. **`Saved Passwords`**:
   * Elenco di password predefinite (separate da virgola) che compariranno automaticamente nei menu a tendina delle finestre di download (es. `DLPSGAME.COM, dlpsgame.com`).
8. **`Real-Debrid Token (Opzionale)`**:
   * Incolla qui il tuo token API personale di Real-Debrid per sbloccare istantaneamente gli hoster a pagamento e aggirare i limiti di attesa IP di 1Fichier.
9. **Tasto `Test Connessione & Crea Cartella`**:
   * **Cosa fa**: Tenta una connessione FTP immediata all'IP e alla porta specificati. Esegue il login e verifica se la cartella di destinazione (es. `/data/pkg`) esiste sulla console; se non esiste, la **crea automaticamente da remoto**!
   * Mostra a video l'esito del test (es. *Connesso con successo! Cartella verificata* oppure *Errore: Timeout o porta errata*).
10. **Sezione Undetected ChromeDriver**:
    * **Etichetta di Stato**: Controlla in tempo reale e segnala se Python 3, Selenium, undetected-chromedriver e il browser Google Chrome/Chromium sono installati nel sistema.
    * **Casella di Spunta `Abilita Undetected ChromeDriver`**: Se attivata, consente all'app di lanciare il browser in background per superare Cloudflare, captcha e timer forzati durante il download automatico.
    * **Casella di Spunta `Modalità Headless`**: Se attivata, il browser anti-bot gira in background senza aprire finestre visibili sul monitor. Se disattivata, puoi vedere la finestra del browser mentre naviga e risolve le pagine.
    * **Tasto `Setup Dipendenze Undetected ChromeDriver`**: Installa automaticamente con un clic i pacchetti Python necessari (`pip install undetected-chromedriver selenium`) e verifica la configurazione del browser.
11. **Tasto `Save`**:
    * Salva tutte le impostazioni in modo permanente nel file di configurazione `~/.config/ps-ftp-downloader/config.json`.
12. **Tasto `Cancel`**:
    * Chiude le impostazioni scartando eventuali modifiche non salvate.

---

## Tecnologia Anti-Bot: Undetected ChromeDriver

I siti aggregatori di giochi proteggono spesso i propri mirror con sistemi di protezione avanzati (Cloudflare Turnstile, CutCaptcha, countdown forzati o link-shortener). I tradizionali scraper HTTP falliscono sistematicamente su queste pagine.

**PS FTP Downloader** include un motore intelligente basato su Selenium e ChromeDriver non rilevabile (`gtk-app/uc_resolver.py`):

```
Link Protetto ──► [UC Resolver] ──► Blocco Popunder ──► Evasione Honeypot ──► Input Biometrico CDP ──► Stream Diretto PKG
```

1. **Blocco Rigido dei Popunder Pubblicitari**:
   * I siti di file hosting aprono popunder e pagine pubblicitarie ingannevoli. Il resolver configura Chromium con le preferenze `"profile.default_content_setting_values.popups": 2`, bloccando l'apertura di finestre indesiderate sul nascere.
   * La funzione `close_rogue_ad_windows` chiude attivamente qualsiasi scheda ad estranea, evitando che l'applicazione scambi un sito pubblicitario per un link di download.
2. **Evasione delle Trappole Antibot ("Honeypot")**:
   * Siti come FileCrypt inseriscono link trappola invisibili nel codice HTML (es. `<a href="/Link/1" style="display:none;"></a>`) creati appositamente per bloccare gli scraper automatici. Il resolver riconosce ed elude queste trappole, interagendo solo con i veri bottoni di sblocco.
3. **Input Biometrico Nativo CDP**:
   * L'interazione con i controlli captcha (es. CutCaptcha) non usa injection JavaScript rilevabili, ma invia comandi hardware simulati tramite il Chrome DevTools Protocol (`Input.dispatchMouseEvent`) con coordinate reali ed eventi contrassegnati da `isTrusted: true`.
4. **Rilevamento File Rimosso / DMCA**:
   * Se un file è stato cancellato dal fornitore di hosting (es. Mediafire `error.php?errno=386`), il resolver lo rileva all'istante segnalando `FILE_REMOVED`, permettendo all'applicazione di passare automaticamente al mirror alternativo successivo.
5. **Meccanismo di Riserva "Clipboard Watcher"**:
   * Se una protezione richiede un'interazione visiva umana imprevista, l'app apre la pagina nel browser e attiva il monitor degli appunti: appena l'utente supera il controllo e copia il link finale, **il programma lo intercetta da solo negli appunti e avvia immediatamente lo streaming verso la PlayStation!**

---

## Integrazione Real-Debrid

Se possiedi un account Real-Debrid, puoi sfruttare la massima velocità e il bypass dei limiti di banda:

1. Visita [real-debrid.com/apitoken](https://real-debrid.com/apitoken) e copia il tuo token API personale.
2. Apri le **Impostazioni** di PS FTP Downloader e incolla la chiave nel campo `Real-Debrid Token`. Premi **Save**.
3. **Vantaggi Immediati**:
   * **Bypass del limite di attesa 1Fichier**: 1Fichier impone agli utenti gratuiti un'attesa forzata di 60-90 minuti tra un download e l'altro (*"Without subscription you must wait..."*). Se il token Debrid è presente, PS FTP Downloader rileva il limite e genera istantaneamente il link diretto ad alta velocità tramite Real-Debrid in meno di 0.3 secondi!
   * **Sblocco 1-Click Multi-Hoster**: Dalla scheda dettagli di qualsiasi gioco puoi cliccare sul pulsante **`Debrid 1-Click`** accanto a link Rapidgator, Mega, Datanodes o 1Fichier per avviare il trasferimento alla massima velocità della tua connessione.

---

## Consigli per la Massima Velocità di Trasferimento

La velocità di trasferimento verso la PlayStation dipende dalla connessione locale tra il tuo computer e la console:

| Tipo di Connessione | Velocità Media Tipica | Tempo per un PKG da 40 GB |
|---|---|---|
| **Cavo Ethernet Gigabit (Consigliato)** | **80 – 115 MB/s** | **~ 6 – 8 minuti** |
| Wi-Fi 5 GHz (segnale eccellente) | 25 – 45 MB/s | ~ 15 – 25 minuti |
| Wi-Fi 2.4 GHz | 8 – 15 MB/s | ~ 45 – 80 minuti |

> **Suggerimento d'oro**: Per trasferimenti ultra-rapidi, collega sia il PC sia la PlayStation al router tramite **cavo di rete Ethernet LAN (Cat 5e o Cat 6)**. In questo modo sfrutterai la banda Gigabit piena saturando la velocità massima del disco della console.

---

## Script Standalone da Terminale (CLI)

Per chi utilizza distribuzioni Linux server headless (senza interfaccia grafica) o gestisce i trasferimenti tramite SSH, il repository include lo script standalone bash [`ps-ftp-download.sh`](ps-ftp-download.sh):

```bash
# Rendi lo script eseguibile
chmod +x ps-ftp-download.sh

# Avvia il download streaming interattivo
./ps-ftp-download.sh
```

### Dipendenze richieste per lo script CLI:
```bash
sudo apt install -y curl wget pv
```
Lo script accetta la configurazione della console impostando le variabili `FTP_HOST` e `FTP_PORT` o passando i parametri direttamente da riga di comando.

---

## Risoluzione dei Problemi e Domande Frequenti (FAQ)

### 1. Il programma restituisce "Errore di connessione FTP" o va in timeout
* **Verifica che la console sia accesa**: Il server FTP della PlayStation si interrompe se la console entra in modalità riposo o viene spenta.
* **Verifica che il server FTP sia abilitato**: Su PS4, controlla in *Impostazioni ➔ GoldHEN ➔ Server Settings* che la casella *Enable FTP Server* sia spuntata.
* **Verifica l'indirizzo IP e la porta**: Controlla che l'IP nelle Impostazioni coincida con quello mostrato dalla notifica di GoldHEN e che la porta sia impostata su **`2121`** (su PS4) o **`1337`** (su PS5).
* **Firewall di Linux**: Se hai un firewall attivo sul computer (come `ufw`), consenti il traffico verso la console con `sudo ufw allow out to any port 2121 proto tcp`.

### 2. Il download termina con successo ma il gioco non compare nel Package Installer
* **Verifica il percorso della cartella**: Il Package Installer di GoldHEN cerca i file nella cartella `/data/pkg`. Assicurati che nelle Impostazioni il campo *Directory* sia impostato su `data/pkg`.
* **Aggiorna la schermata**: Esci dal Package Installer e rientra per forzare la riscransione della cartella.
* **Verifica l'estensione**: Il file deve avere estensione `.pkg` (scritta in minuscolo).

### 3. Compare il messaggio "File rimosso" o "Errore 404"
* Sui siti di hosting gratuiti alcuni file possono essere rimossi nel tempo per inattività o copyright.
* Apri la finestra dei **Dettagli** del gioco e prova a utilizzare un mirror alternativo (es. se Mediafire è offline, prova il link 1Fichier o Debrid). Il tasto **`Scarica Automatico su PS`** si occupa di testare i mirror funzionanti in automatico.

### 4. Il risolutore Undetected ChromeDriver segnala che manca Chrome o Chromium
* Installa Chromium o Google Chrome sul tuo sistema Linux:
  ```bash
  sudo apt install -y chromium-browser
  ```
  oppure tramite Snap/Flatpak se la tua distribuzione lo prevede. Nelle Impostazioni dell'app premi il tasto **`Setup Dipendenze Undetected ChromeDriver`** per verificare che tutti i componenti siano pronti.

### 5. Il programma consuma spazio sul mio hard disk o SSD?
* **Per tutti i download diretti (.pkg)**: **Zero byte**. I pacchetti fluiscono direttamente dalla scheda di rete alla memoria RAM del computer e da questa alla PlayStation via FTP. Nessun file temporaneo viene salvato su disco.
* **Per gli archivi divisi (.part1.rar)**: Le parti vengono scaricate nella cartella temporanea (`Temp Dir`) per permettere a 7-Zip di scompattarle. Non appena il file `.pkg` estratto è stato caricato sulla console, tutti i file temporanei vengono **automaticamente cancellati**.

---

## Licenza

Questo progetto è distribuito sotto licenza open source **MIT**. Consulta il file [LICENSE](LICENSE) per i termini completi.

<div align="center">
Realizzato con passione per la community PlayStation Homebrew.
</div>
