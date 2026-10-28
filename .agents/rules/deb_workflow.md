# Regola di compilazione e installazione automatica .deb

Ogni volta che si compila, modifica o aggiorna il pacchetto `.deb` di P2C Downloader:
1. Compilare il `.deb` tramite `make deb` (o `./build-deb.sh` in `gtk-app/`).
2. **Installare sempre direttamente il pacchetto `.deb` aggiornato sul PC dell'utente** senza che debba farlo manualmente:
   ```bash
   pkexec apt-get install -y --reinstall /home/gab/Documents/GitHub/PS-FTP-Downloader/p2c-downloader_*.deb
   ```
3. Verificare che l'installazione sia avvenuta con successo.
