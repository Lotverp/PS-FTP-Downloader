<div align="center">

# P2C Downloader
### 🚀 PC to Console Downloader

**Scarica e trasmetti qualsiasi gioco, file o archivio direttamente dal tuo PC alla tua Console via FTP — in Pure Streaming e con Zero spazio occupato sul computer.**

[![License: MIT](https://img.shields.io/badge/License-MIT-violet.svg)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-blue.svg)](https://www.linux.org/)
[![GTK: 4](https://img.shields.io/badge/GTK-4-green.svg)](https://gtk.org/)
[![Built with: C](https://img.shields.io/badge/Built%20with-C-orange.svg)]()
[![GitHub Repo](https://img.shields.io/badge/GitHub-Lotverp%2FP2C--Downloader-blue.svg)](https://github.com/Lotverp/P2C-Downloader)

</div>

---

## Cos'è P2C Downloader?

**P2C Downloader** sta per **PC to Console Downloader**.

È un'applicazione nativa per Linux ad altissime prestazioni (scritta in C con interfaccia grafica moderna in GTK4) concepita con un obiettivo fondamentale: **eliminare la seccatura di dover scaricare decine o centinaia di Gigabyte sul disco del computer per poi doverli copiare a mano su chiavette USB o trasferirli con lenti client FTP esterni**.

Con P2C Downloader, il tuo computer fa da **ponte intelligente in tempo reale** tra Internet e la tua console:

```
┌──────────────┐          HTTP / HTTPS          ┌──────────────────────┐          FTP Socket          ┌──────────────────────┐
│   INTERNET   │ ─────────────────────────────► │    RAM RING-BUFFER   │ ──────────────────────────► │       CONSOLE        │
│(Direct/Hoster│     Download asincrono (C)     │ (2 MB circolare atom)│      Upload asincrono (C)    │ PS3/PS4/PS5/Switch...│
└──────────────┘                                └──────────────────────┘                             └──────────────────────┘
```

> [!IMPORTANT]
> ### 💡 Non solo PlayStation, non solo file .PKG!
> * **Qualsiasi Console**: Funziona con qualsiasi console o dispositivo su cui sia attivo un server FTP standard: **PlayStation 4, PlayStation 5, PlayStation 3, PlayStation Vita, Nintendo Switch, Xbox, PC Server e retro-console**.
> * **Qualsiasi Tipo di File**: Non serve che sia un file `.pkg`. Puoi trasferire **qualsiasi formato**: `.pkg`, `.iso`, `.bin`, `.cue`, `.xci`, `.nsp`, `.cia`, `.chd`, `.cso`, `.wbfs`, `.rvz`, `.vpk`, `.zip`, `.7z`, ROM per emulatori, salvataggi o file multimediali.

---

## Indice dei Contenuti
1. [Come Funziona (Collegamento e Trasmissione)](#come-funziona-collegamento-e-trasmissione)
2. [Guida all'Uso per l'Utente Standard (Passo dopo Passo)](#guida-alluso-per-lutente-standard)
   - [Fase 1: Configurazione Rapida del Collegamento (2 minuti)](#fase-1-configurazione-rapida-del-collegamento)
   - [Scenario A: Scaricare un gioco dal Catalogo integrato (1-Click)](#scenario-a-scaricare-un-gioco-dal-catalogo-integrato-1-click)
   - [Scenario B: Scaricare un link preso da Internet (Mediafire, 1Fichier, ecc.)](#scenario-b-scaricare-un-link-preso-da-internet)
   - [Scenario C: Scaricare un gioco diviso in più parti (.part1.rar, .part2.rar)](#scenario-c-scaricare-un-archivio-diviso-in-più-parti)
   - [Scenario D: Inviare un file già presente sul tuo PC alla Console](#scenario-d-inviare-un-file-già-presente-sul-pc-alla-console)
   - [Scenario E: Installare Homebrew essenziali con 1 Clic](#scenario-e-installare-homebrew-essenziali-con-1-clic)
   - [Fase Finale: Cosa fare sulla Console al termine del download](#fase-finale-cosa-fare-sulla-console-a-download-ultimato)
3. [Guida Specifica per Console](#guida-specifica-per-console)
   - [PlayStation 4 (GoldHEN)](#1-playstation-4-con-goldhen)
   - [PlayStation 5 (etaHEN / ftpserver)](#2-playstation-5-con-etahen)
   - [PlayStation 3 (webMAN-MOD / HEN)](#3-playstation-3-con-webman-mod--hen)
   - [PlayStation Vita (VitaShell)](#4-playstation-vita-con-vitashell)
   - [Nintendo Switch (ftpd / Atmosphere)](#5-nintendo-switch-con-ftpd)
   - [Xbox e Altre Piattaforme FTP](#6-xbox-e-altri-sistemi-con-server-ftp)
4. [Panoramica delle Schermate dell'App](#panoramica-delle-schermate-dellapp)
5. [Tecnologia Anti-Bot e Risoluzione Link](#tecnologia-anti-bot-e-risoluzione-link)
6. [Integrazione Real-Debrid](#integrazione-real-debrid)
7. [Consigli per la Massima Velocità di Rete](#consigli-per-la-massima-velocità-di-rete)
8. [Installazione su Linux](#installazione-su-linux)
9. [Script CLI da Terminale](#script-cli-da-terminale)
10. [Domande Frequenti (FAQ) e Risoluzione Problemi](#domande-frequenti-faq-e-risoluzione-problemi)

---

## Come Funziona (Collegamento e Trasmissione)

Comprendere come lavora P2C Downloader ti consente di sfruttarne appieno le potenzialità:

### 1. Il Collegamento: Stessa Rete Locale (LAN o Wi-Fi)
Il computer e la console **non richiedono cavi speciali né connessioni dirette USB**. È sufficiente che entrambi siano collegati allo **stesso modem/router di casa**:
- Il PC e la console comunicano attraverso il router tramite il protocollo di rete standard **FTP** (File Transfer Protocol).
- Non appena la console attiva il proprio server FTP (ad es. avviando GoldHEN su PS4 o l'homebrew *ftpd* su Switch), diventa visibile e raggiungibile dal PC tramite il proprio indirizzo IP locale (es. `192.168.1.111`).

### 2. La Trasmissione: Pure RAM Ring-Buffer (Nessun file su PC)
I software tradizionali scaricano prima l'intero file (es. 50 GB) sul disco fisso del computer, esaurendo lo spazio e usurando l'SSD, per poi ricominciare da zero con il trasferimento verso la console.

P2C Downloader adotta invece un'architettura asincrona a **doppio thread con buffer circolare in RAM**:
- **Thread 1 (Ricezione HTTP)**: Scarica i dati direttamente dai server web e li deposita in una porzione minuscola di memoria RAM (circa **2 MB**).
- **Thread 2 (Trasmissione FTP)**: Preleva contemporaneamente i dati dalla RAM e li invia attraverso il socket di rete direttamente sull'hard disk della console.
- **Risultato**: I due processi avvengono **in parallelo alla massima velocità possibile**. Sul tuo computer non viene salvato nessun file temporaneo. Spazio occupato su disco: **0 Byte**.

### 3. Gestione Archivi Divisi Multi-Volume (.part1.rar / .zip / .7z)
Quando un gioco è suddiviso in più parti compresse su diversi link (es. 10 parti da 4 GB l'una):
1. P2C Downloader scarica le parti in sequenza nella cartella temporanea (`Temp Dir`).
2. Scompatta automaticamente l'archivio con 7-Zip in background.
3. Riconosce in autonomia il file principale contenuto (che sia un `.pkg`, una `.iso`, un `.bin`, un `.nsp`, ecc.).
4. Avvia lo streaming FTP del file scompattato verso la console.
5. **Auto-Pulizia Istantanea**: Appena il caricamento sulla console è completato con successo al 100%, l'applicazione **cancella all'istante tutti i file RAR scaricati e i file estratti temporanei**, restituendoti ogni singolo megabyte di spazio sul PC!

### 4. Auto-Ripresa dei Trasferimenti Interrotti (Resume)
Se la connessione Wi-Fi vacilla per un istante o il router riavvia il collegamento, P2C Downloader interroga sia il server web che la console via FTP (`Range: bytes` e comando `REST`): il trasferimento riprende esattamente dal byte in cui era stato sospeso, senza dover riscaricare nulla da capo.

---

## Guida all'Uso per l'Utente Standard

Ecco una guida semplice e concreta per iniziare a usare P2C Downloader da subito.

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│  [+ Nuovo Download] [Archivio Diviso]      [Download] [Catalogo] [Giochi]  [Sorgenti] ⚙ │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                        │
│                               SCHERMATA PRINCIPALE                                     │
│                                                                                        │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

---

### Fase 1: Configurazione Rapida del Collegamento

Questa operazione va fatta **solo la prima volta** (richiede meno di 2 minuti):

1. **Accendi la tua console** e attiva il server FTP:
   - Su **PS4**: Vai in *Impostazioni ➔ GoldHEN ➔ Server Settings* e spunta **Enable FTP Server**. Apparirà una notifica con l'indirizzo (es. `192.168.1.111:2121`).
   - Su **PS5**: Nelle impostazioni di *etaHEN*, attiva **FTP Server** (porta `1337` o `2121`).
   - Su **PS3**: Avvia la console con HEN/CFW (webMAN-MOD attiva l'FTP in automatico sulla porta `21`).
   - Su **Switch**: Apri l'homebrew *ftpd* dall'Homebrew Launcher (porta `5000`).
   - Su **PS Vita**: Apri *VitaShell* e premi *SELECT* (porta `1337`).
2. **Apri P2C Downloader** sul tuo computer Linux.
3. Clicca sull'icona dell'ingranaggio **⚙** in alto a destra per aprire le **Impostazioni**.
4. Compila i campi:
   - **`FTP Host`**: Digita l'IP della tua console (es. `192.168.1.111`).
   - **`FTP Port`**: Digita la porta indicata dalla console (es. `2121` per PS4, `5000` per Switch, `21` per PS3).
   - **`Directory`**: La cartella della console dove vuoi inviare i file:
     - Per PS4 e PS5: lascia `data/pkg` (cartella predefinita).
     - Per Switch: ad es. `switch/` o `install/`.
     - Per PS3: ad es. `dev_hdd0/packages`.
     - Per PS Vita: ad es. `ux0:/vpk`.
5. Clicca sul pulsante **`Test Connessione & Crea Cartella`**:
   - L'app verificherà il collegamento e creerà da sola la cartella remota sulla console se non esiste ancora.
6. Clicca su **`Save`**. La configurazione è completata!

---

### Scenario A: Scaricare un gioco dal Catalogo integrato (1-Click)

Il modo più comodo per installare giochi senza cercare link sul web:

1. Nella barra superiore, clicca sulla scheda **`Giochi`** (o sul tasto *Sfoglia Giochi*).
2. Vedrai comparire oltre **6.500 titoli** completi di copertine e dettagli.
3. Puoi:
   - Digitare il titolo del gioco nella barra di ricerca (es. *Spider-Man*, *Bloodborne*, *Crash Bandicoot*).
   - Filtrare per piattaforma dal menu a tendina (PS5, PS4, PS3, PS2, PS1, PSP, Switch, Xbox...).
   - Filtrare per regione (EUR, USA, JPN/ASIA).
4. Su qualsiasi gioco:
   - **Tasto `Scarica Subito` (Consigliato)**: Cliccandolo, l'app avvia in automatico la procedura 1-Click: seleziona il mirror più veloce, supera eventuali timer o captcha e inizia istantaneamente lo streaming verso la console!
   - **Tasto `Dettagli`**: Ti mostra gli screenshot del gioco, la password dell'archivio (con tasto per copiarla) e l'elenco di tutti i singoli mirror disponibili (Mediafire, 1Fichier, ecc.) per permetterti di scegliere manualmente.
5. Torna nella scheda **`Download`**: potrai vedere la barra di avanzamento, la velocità in MB/s e il tempo rimanente stimato!

---

### Scenario B: Scaricare un link preso da Internet

Se hai trovato un link su un qualsiasi sito web o aggregatore:

1. Clicca sul pulsante **`+ Nuovo Download`** in alto a sinistra.
2. Nella casella **`URL File o Percorso Locale`**, incolla il link.
   - Può essere un link diretto a qualsiasi file (`.pkg`, `.iso`, `.bin`, `.zip`, `.nsp`...).
   - Può essere un link di **Mediafire**, **1Fichier**, **Pixeldrain**, o un link protetto da **FileCrypt**.
   - Può essere l'indirizzo di una pagina gioco (es. `dlpsgame.com/...`): l'app la riconosce in automatico ed estrae il download da sola!
3. Nella casella **`Nome file destinazione`**:
   - Puoi lasciarla **vuota**: il programma interrogherà il server web per ricavare il nome originale del file.
   - Oppure puoi digitare il nome desiderato (es. `MioGioco.iso`).
4. Clicca su **`Avvia Download`**: il trasferimento parte all'istante verso la console.

---

### Scenario C: Scaricare un archivio diviso in più parti

Quando un gioco è composto da molti file RAR (es. `parte1.rar`, `parte2.rar`, `parte3.rar`...):

1. Clicca sul pulsante **`Archivio Diviso`** in alto.
2. Inserisci i link delle parti:
   - Se hai copiato un elenco di link, clicca su **`Incolla da Appunti`**: l'app riempirà tutte le caselle in un istante.
   - Oppure clicca su **`+ Add URL`** per aggiungere le righe che ti servono e incollare i link uno ad uno.
3. Nella casella **`Password`**, scrivi la password (se presente) oppure selezionala dal menu a tendina (es. `DLPSGAME.COM`).
4. Clicca su **`Avvia su Console`**:
   - P2C Downloader scaricherà tutte le parti, estrarrà il file finale con 7-Zip, lo invierà alla console via FTP ed **eliminerà tutti i file temporanei non appena finito**, liberando lo spazio sul tuo PC.

---

### Scenario D: Inviare un file già presente sul PC alla Console

Vuoi mandare un gioco, un firmware, un video o un archivio che hai già sul computer senza aprire complicati programmi FTP come FileZilla?

1. Clicca su **`+ Nuovo Download`**.
2. Clicca sul pulsante **`Sfoglia...`** accanto alla casella dell'URL.
3. Seleziona il file dal tuo computer (qualsiasi estensione: `.pkg`, `.iso`, `.bin`, `.zip`, `.nsp`, `.vpk`, ecc.).
4. Clicca su **`Avvia Download`**: il file viene trasmesso alla console sfruttando la velocità Gigabit della tua rete locale.

---

### Scenario E: Installare Homebrew essenziali con 1 Clic

1. Clicca sulla scheda **`Catalogo`** in alto.
2. Troverai una selezione delle applicazioni homebrew più celebri e indispensabili (*Itemzflow Game Manager*, *Apollo Save Tool*, *PS4-Xplorer*, *VitaShell*, *ftpd*, *GoldHEN Cheats*).
3. Clicca semplicemente su **`Installa`** accanto all'applicazione desiderata: l'app scaricherà l'ultima versione ufficiale rilasciata dagli sviluppatori e la invierà subito alla console.

---

### Fase Finale: Cosa fare sulla Console a download ultimato

Quando P2C Downloader mostra lo stato verde **`Completato`** (100%), il file è già memorizzato sul disco della tua console:

* **Su PlayStation 4**:
  1. Vai in *Impostazioni ➔ GoldHEN ➔ Package Installer* (oppure *Debug Settings ➔ Game ➔ Package Installer*).
  2. Troverai il pacchetto appena scaricato.
  3. Premi **X** sul controller: la console lo installerà nella dashboard in pochi secondi.
* **Su PlayStation 5**:
  1. Apri il *Package Installer* di etaHEN e procedi all'installazione del file.
* **Su PlayStation 3**:
  1. Dalla colonna *Gioco* della XMB, vai in *Package Manager ➔ Install Package Files ➔ Standard*.
  2. Seleziona il pacchetto e premi X (oppure, se hai trasferito una ISO in `dev_hdd0/PS3ISO`, caricala con webMAN).
* **Su PlayStation Vita**:
  1. Apri *VitaShell*, naviga nella cartella in cui hai trasferito il file (es. `ux0:/vpk`), evidenzia il file `.vpk` e premi **X** per installarlo.
* **Su Nintendo Switch**:
  1. Apri il tuo title manager di fiducia (*DBI*, *Tinfoil*, *Awoo Installer* o *Goldleaf*).
  2. Seleziona l'installazione da scheda SD navigando nella cartella scelta (es. `switch/` o `install/`) e installa il gioco `.nsp` o `.xci`.

---

## Guida Specifica per Console

Di seguito i dettagli tecnici di configurazione consigliati per ciascun dispositivo:

| Console | Homebrew / Exploit Richiesto | Porta FTP Tipica | Cartella Consigliata | Come si attiva l'FTP |
|---|---|---|---|---|
| **PS4** | GoldHEN / ps4-hen | **`2121`** | `data/pkg` | *Impostazioni ➔ GoldHEN ➔ Server Settings ➔ Enable FTP* |
| **PS5** | etaHEN / ftpserver | **`1337`** oppure **`2121`** | `data/pkg` | Menu impostazioni etaHEN |
| **PS3** | webMAN-MOD / HEN / CFW | **`21`** | `dev_hdd0/packages` o `dev_hdd0/PS3ISO` | Attivo in automatico all'avvio con webMAN |
| **PS Vita** | VitaShell | **`1337`** | `ux0:/vpk` oppure `ux0:/data` | Apri VitaShell e premi **SELECT** |
| **Nintendo Switch** | Atmosphere + ftpd | **`5000`** | `switch/` o `install/` | Avvia l'app *ftpd* dall'Homebrew Launcher |
| **Xbox Classic / 360** | UnleashX / Aurora / FSD | **`21`** | `Games/` o personalizzata | Attivo in automatico nelle dashboard modificate |

---

## Panoramica delle Schermate dell'App

### 1. Scheda "Download Attivi"
È la cabina di pilotaggio dove monitorare tutti i processi in corso:
- **Barra Riepilogativa Superiore**: Mostra il numero di trasferimenti contemporanei, la velocità aggregata in tempo reale (MB/s) e la percentuale complessiva.
- **Card di Trasferimento Individuale**:
  - Nome del file e console di destinazione.
  - Badge di stato dinamico (`Download in corso...`, `Estrazione archivio...`, `Invio via FTP...`, `Completato`, `Errore`).
  - Barra fluida con indicazione della percentuale.
  - Dati trasferiti rispetto al totale (es. `18.4 GB / 42.1 GB`).
  - Velocità istantanea e stima del tempo rimanente (ETA in minuti e secondi).
  - Tasto **`Annulla`** per interrompere il trasferimento liberando all'istante ogni risorsa di rete e memoria.

### 2. Scheda "Sfoglia Giochi" (DLPSGame)
Offre la navigazione visiva di oltre 6.500 titoli:
- **Ricerca Intelligente**: Cerca istantaneamente per titolo o per codice univoco (CUSA, PPSA, Title ID).
- **Filtro Piattaforma**: Scegli se visualizzare giochi per PS5, PS4, PS3, PS2, PS1, PSP, PS Vita, Nintendo Switch, Xbox o PC.
- **Filtro Regione**: Filtra per edizioni Europee (con lingua italiana), Americane o Giapponesi/Asiatiche.

### 3. Scheda "Tools" (Store Homebrew)
Raccoglie i migliori strumenti per la tua console scaricabili con un solo clic. Tramite il tasto **`Sorgenti`** puoi aggiungere ulteriori feed JSON della community o creati da te.

### 4. Finestra "Impostazioni" (⚙)
Consente di configurare l'indirizzo IP, la porta FTP, le credenziali, la cartella remota, la cartella temporanea per le scompattazioni RAR e il token di Real-Debrid.

---

## Tecnologia Anti-Bot e Risoluzione Link

I siti di hosting moderni proteggono spesso i propri file con sistemi anti-bot (Cloudflare Turnstile, CutCaptcha, countdown timer e link shortener) che bloccano i normali scraper.

P2C Downloader integra un motore dedicato basato su **Selenium e Undetected ChromeDriver** (`gtk-app/uc_resolver.py`):
1. **Blocco dei Popunder Pubblicitari**: Impedisce l'apertura di schede spam o truffaldine tipiche dei siti di file sharing.
2. **Evasione Honeypot**: Riconosce ed evita i finti pulsanti invisibili inseriti nei siti per intrappolare i bot.
3. **Eventi Hardware CDP**: Esegue i clic sui captcha simulando comandi del mouse a livello hardware tramite il protocollo Chrome DevTools, risultando indistinguibile da un utente umano reale.
4. **Clipboard Watcher**: Se una pagina richiede un controllo visivo manuale imprevisto, l'app monitora gli appunti: appena copi il link finale nel browser, **P2C Downloader lo intercetta da solo e avvia subito lo streaming alla console!**

---

## Integrazione Real-Debrid

Se possiedi un account Real-Debrid:
1. Ottieni il tuo token API personale su [real-debrid.com/apitoken](https://real-debrid.com/apitoken).
2. Apri le **Impostazioni** (⚙) di P2C Downloader e incollalo nel campo `Real-Debrid Token`. Clicca su **Save**.
3. **I Vantaggi**:
   - **Bypass dei tempi di attesa di 1Fichier**: 1Fichier impone pause forzate di 60-90 minuti agli utenti gratuiti. Con Real-Debrid configurato, P2C Downloader genera il link diretto ad alta velocità in meno di 0.3 secondi!
   - **Sblocco 1-Click Multi-Hoster**: Dalla finestra dettagli di qualsiasi gioco puoi cliccare sul pulsante **`Debrid 1-Click`** accanto a mirror Rapidgator, Mega o Datanodes per scaricare alla massima velocità possibile della tua fibra ottica.

---

## Consigli per la Massima Velocità di Rete

La velocità con cui il file fluisce dal tuo computer alla console dipende dal tipo di connessione locale:

| Tipo di Connessione | Velocità Tipica | Tempo stimato per un gioco da 40 GB |
|---|---|---|
| **Cavo Ethernet Gigabit LAN (Cat 5e o Cat 6)** | **80 – 115 MB/s** | **~ 6 – 8 minuti** |
| Wi-Fi 5 GHz (segnale eccellente) | 25 – 45 MB/s | ~ 15 – 25 minuti |
| Wi-Fi 2.4 GHz | 8 – 15 MB/s | ~ 45 – 80 minuti |

> [!TIP]
> **Il suggerimento d'oro**: Per trasferimenti ultra-rapidi, collega sia il PC sia la console al router con un **cavo di rete Ethernet LAN**. In questo modo sfrutterai la banda Gigabit piena saturando la velocità di scrittura dell'hard disk o SSD della console!

---

## Installazione su Linux

### Metodo 1: Pacchetto `.deb` (Consigliato per Ubuntu, Debian, Mint, Zorin)
Scarica il pacchetto `.deb` dalla sezione [Releases](../../releases) o compilalo localmente, quindi installalo con:
```bash
sudo apt install -y ./p2c-downloader_1.3.5_amd64.deb
```
L'applicazione comparirà nel menu principale del tuo sistema tra le categorie Giochi e Rete, oppure puoi avviarla digitando da terminale:
```bash
p2c-downloader
```

### Metodo 2: Compilazione dai sorgenti
```bash
sudo apt update
sudo apt install -y build-essential libgtk-4-dev libcurl4-openssl-dev libjson-glib-dev 7zip
git clone https://github.com/Lotverp/P2C-Downloader.git
cd P2C-Downloader/gtk-app
make
./p2c-downloader
```

### Setup Risolutore Anti-Bot (Consigliato)
Per sbloccare automaticamente timer e controlli Cloudflare:
```bash
pip install --user undetected-chromedriver selenium
```
*(Puoi anche cliccare semplicemente sul tasto `Setup Dipendenze Undetected ChromeDriver` nelle Impostazioni dell'app).* Assicurati di avere installato sul PC **Google Chrome** oppure **Chromium** (`sudo apt install -y chromium-browser`).

---

## Script CLI da Terminale

Per chi gestisce server Linux senza interfaccia grafica o desidera effettuare trasferimenti via SSH, il repository include lo script standalone [`p2c-download.sh`](p2c-download.sh):

```bash
chmod +x p2c-download.sh
./p2c-download.sh
```
Puoi personalizzare la destinazione impostando le variabili d'ambiente:
```bash
FTP_HOST=192.168.1.150 FTP_PORT=2121 FTP_DIR="data/pkg" ./p2c-download.sh
```

---

## Domande Frequenti (FAQ) e Risoluzione Problemi

### 1. Il programma dà "Errore di connessione FTP" o va in timeout
- **La console è accesa?**: Il server FTP della console si disattiva se la console va in modalità riposo o viene spenta.
- **L'FTP è abilitato sulla console?**: Controlla di aver attivato il server FTP (su PS4 in *GoldHEN ➔ Server Settings*, su Switch aprendo l'app *ftpd*, su PS Vita aprendo *VitaShell*).
- **IP e Porta corretti?**: Verifica che l'indirizzo IP inserito nelle Impostazioni coincida con quello mostrato dalla console.
- **Firewall Linux**: Se hai un firewall attivo (`ufw`), consenti il traffico verso la porta della console:
  ```bash
  sudo ufw allow out to any port 2121 proto tcp
  ```

### 2. Posso trasferire file diversi dai giochi (.iso, .bin, .nsp, roms...)?
- **Assolutamente sì!** P2C Downloader gestisce lo streaming di qualsiasi formato di file. Ti basta specificare la cartella remota corretta per la tua console nelle Impostazioni.

### 3. Il programma consuma spazio sul disco fisso del mio computer?
- **Per i download diretti**: **Zero Byte**. Il file fluisce direttamente dalla scheda di rete alla RAM del computer e da questa alla console via FTP.
- **Per gli archivi divisi (.part1.rar)**: Le parti vengono salvate nella cartella temporanea locale solo per permettere a 7-Zip di scompattarle. Non appena il file estratto è stato trasferito alla console, tutti i file temporanei vengono **automaticamente cancellati**.

### 4. Il gioco compare come "File rimosso" o dà errore 404
- Sui siti di file hosting gratuito alcuni link possono scadere nel tempo. Apri la finestra dei **Dettagli** del gioco e prova uno degli altri mirror disponibili (Mediafire, 1Fichier, Debrid). Il tasto **`Scarica Automatico su Console`** testa i mirror attivi in autonomia.

---

## Licenza

Questo progetto è distribuito sotto licenza open source **MIT**. Consulta il file [LICENSE](LICENSE) per i dettagli completi.

<div align="center">
Realizzato con passione per la community Console & Homebrew.
</div>
