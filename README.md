# YouCam — Windows nativo

Webcam flottante e registratore per **Windows 11 x64**, scritto in C++ con le API di Windows. Sorgenti disponibili, nessun installer e nessuna dipendenza da Python, Qt, Miniforge o FFmpeg. Il programma usa Win32, Windows Graphics Capture, Media Foundation e WASAPI.

Port indipendente ispirato a [YouCam di antirez](https://github.com/antirez/youcam). Conserva licenza MIT, attribuzione e impostazione essenziale dell'originale: un piccolo strumento locale, moduli diretti, API di sistema, nessun servizio, account applicativo o telemetria. Non è una release ufficiale di antirez. I commenti spiegano soprattutto i vincoli delle API e le scelte che non sono evidenti dal codice.

## Compilazione e avvio

Installa **Microsoft C++ Build Tools**, selezionando **Sviluppo desktop con C++**, **Windows SDK** e **strumenti CMake per Windows**. Non serve l'IDE completo di Visual Studio. Apri il Developer Command Prompt nella cartella del progetto:

```bat
build.cmd
run.cmd
```

Il risultato è `build\Release\YouCam.exe`. Puoi avviarlo direttamente con doppio clic o copiarlo sul PC Windows insieme a questa documentazione e alla licenza. Il runtime C++ è collegato staticamente; l'app usa poi le librerie multimediali presenti in Windows. Su Windows N può essere necessario il Media Feature Pack.

Per ricompilare dopo una modifica basta ripetere `build.cmd`. Miniforge non serve. I comandi equivalenti sono:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

## Risoluzione

La **webcam** viene aperta alla risoluzione più alta dichiarata dal driver: prima si confronta il numero di pixel, poi il frame rate. Si provano le rappresentazioni disponibili a quella risoluzione, senza scendere silenziosamente a una modalità inferiore. Le dimensioni scelte sono visibili nel pannello. Il ridimensionamento della webcam flottante modifica soltanto l'anteprima; **Camera only** registra sempre il fotogramma intero alla risoluzione acquisita. Se la webcam compare dentro una registrazione dello schermo, viene naturalmente registrata con la dimensione visibile sullo schermo.

Lo **schermo** viene acquisito nei pixel fisici della modalità attiva di Windows. Un monitor impostato a 3840×2160 viene registrato a 3840×2160 anche con scaling al 150% o al 200%. L'app non cambia la modalità del monitor: per usare una modalità superiore, impostala prima nelle impostazioni Schermo di Windows. Una **regione** mantiene esattamente i propri pixel fisici.

Non c'è un limite artificiale a 4096 pixel. H.264 e, se disponibile, HEVC vengono provati attraverso gli encoder di sistema con accelerazione hardware consentita. La disponibilità effettiva della GPU dipende dagli encoder installati e dal driver. Se nessun encoder accetta le dimensioni richieste, la registrazione segnala l'errore: **non riduce la risoluzione**. Le dimensioni dispari ricevono al massimo una riga/colonna di padding, senza tagliare pixel originali.

I video sono **MP4**, normalmente H.264 con audio AAC mono a 48 kHz, fino a 30 fotogrammi al secondo. Conservare la risoluzione non significa compressione senza perdita. Il bitrate cresce con il numero di pixel. Prestazioni e supporto delle modalità molto grandi vanno verificati sulla macchina reale.

## Uso

- Trascina la webcam per spostarla; usa la rotellina per ridimensionarla. Clic destro o icona nella tray per il menu. Sono disponibili cerchio, quadrato e rettangolo 16:9, con specchiatura.
- **Recording source** sceglie monitor, area, finestra o **Camera only (full frame)**. In modalità solo webcam, **Hide camera preview** nasconde l'anteprima e continua a registrare.
- **Select area…** permette di disegnare una regione su un singolo monitor. **Esc** annulla. Prima di registrare, trascina gli angoli per ridimensionarla o la barra **Move** per spostarla; i pulsanti accanto all'area permettono di selezionarla nuovamente o tornare allo schermo intero.
- La registrazione di una **finestra** usa Windows Graphics Capture e non include le finestre che la coprono. L'area client esclude la cornice quando il sistema ne fornisce la geometria corrispondente. Se ridimensioni la finestra, il video mantiene le dimensioni iniziali e adatta il contenuto con eventuali bande. Minimizzarla/nasconderla mette in pausa; ripristinala e premi **Resume**. Chiuderla interrompe e tenta di salvare i fotogrammi già acquisiti.
- Cambiare sorgente durante la registrazione propone di salvare prima il video corrente. Una modifica della risoluzione del monitor richiede una nuova registrazione.
- **Audio input** sceglie il microfono. Il pannello offre guadagno da −36 a +12 dB e indicatore di livello. L'audio di sistema non viene registrato.
- **Ctrl+Alt+Shift+R** avvia/ferma; **Ctrl+Alt+Shift+P** mette in pausa/riprende. Se un'altra applicazione occupa una combinazione, usa i pulsanti o il menu.
- I file `YouCam-<data>-<ora>.mp4` si salvano sul Desktop indicato da Windows, anche se reindirizzato su OneDrive. Le preferenze sono in `%LOCALAPPDATA%\YouCamNative\settings.ini`.

Pannello e guide vengono esclusi dalla cattura tramite l'API nativa di Windows. Windows può mostrare il bordo di sistema attorno alla sorgente acquisita. Contenuti protetti, sessioni bloccate e desktop sicuri non sono sorgenti normalmente registrabili. Questa versione produce video SDR; la gestione colore HDR non è implementata.

In **Impostazioni → Privacy e sicurezza → Fotocamera / Microfono**, consenti l'accesso alle applicazioni desktop. Se una periferica è occupata, chiudi l'altra app e usa **Retry camera**. Un errore nella modalità massima della webcam può dipendere da driver, formato o collegamento USB; il programma lo segnala senza passare a una risoluzione minore.

## Test

```bat
test.cmd
```

I test portabili verificano scelta della risoluzione massima, dimensioni 8K, conservazione dei pixel nelle immagini dispari, specchiatura, conversione NV12 e continuità temporale dopo le pause. I test Windows codificano e decodificano video con Media Foundation, incluso un filmato 3840×2160, e verificano audio AAC e dimensioni del file salvato. Quando Windows Graphics Capture è disponibile, provano anche monitor, regione, finestra e minimizzazione/ripresa. Una prova di avvio usa una webcam sintetica.

GitHub Actions compila e verifica il progetto su un runner Windows Server. Il runner non verifica la webcam fisica o gli encoder AMD del tuo Windows 11. Sul PC reale controlla la risoluzione mostrata nel pannello e quella del file salvato, oltre a video, microfono, pausa/ripresa e acquisizione con scaling del monitor attivo.

Su Linux si possono eseguire solo i test della logica indipendente dal sistema:

```sh
cmake -S . -B .build/core
cmake --build .build/core
ctest --test-dir .build/core --output-on-failure
```

## Sorgenti

`app.cpp` gestisce l'interfaccia Win32; `camera.cpp` negozia la modalità massima della webcam; `audio.cpp` acquisisce il microfono tramite WASAPI; `capture.cpp` riceve le superfici di Windows Graphics Capture; `recorder.cpp` produce l'MP4 con Media Foundation; `core.h` raccoglie le piccole operazioni verificabili senza Windows.

La precedente implementazione Python è stata rimossa dall'albero corrente del progetto. [Licenza MIT](LICENSE).
