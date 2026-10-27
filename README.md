# YouCam per Windows

Webcam flottante e registratore dello schermo per **Windows 11 x64**. Si esegue dai sorgenti con Miniforge: niente installer, compilatore C/C++ o privilegi amministrativi.

Port indipendente di [YouCam di antirez](https://github.com/antirez/youcam), a partire dal port locale Python/Qt per Linux. Conserva la licenza MIT e l'attribuzione originali. Non è una versione Windows ufficiale di antirez.

## Preparazione e avvio

Scarica i sorgenti o clona questa repository privata sul PC Windows. Apri **Miniforge Prompt**, entra nella cartella e lancia:

```bat
cd /d "%USERPROFILE%\Desktop\youcam-windows"
setup.cmd
run.cmd
```

Usa il percorso effettivo se il Desktop è in OneDrive o in un'altra posizione. `setup.cmd` va eseguito una sola volta: crea l'ambiente separato `youcam-windows`, con Python 3.12, FFmpeg e tre dipendenze Python (PySide6, NumPy, MSS). L'installazione iniziale richiede Internet; l'applicazione funziona localmente.

Per gli avvii successivi basta `run.cmd` da Miniforge Prompt. In alternativa:

```bat
conda activate youcam-windows
python -m youcam
```

Non serve una compilazione nativa dell'applicazione: il codice Python è eseguito direttamente e le librerie sono distribuite già compilate. Modifica i sorgenti e riavvia. Dopo un aggiornamento delle dipendenze:

```bat
conda env update --file environment.yml --prune
```

## Uso

- Trascina la webcam per spostarla; usa la rotellina per ridimensionarla. Clic destro sulla webcam o icona nella tray per il menu. Cerchio, quadrato e rettangolo 16:9.
- **Recording Source → Select Area…** seleziona una regione di un monitor. Gli angoli bianchi la ridimensionano; **Move** la sposta. **Esc** annulla una nuova selezione conservando la precedente. Sono disponibili anche intero schermo e scelta del monitor.
- **Window** registra il contenuto della singola finestra, senza cornice, desktop o webcam flottante. La finestra può essere spostata, coperta e ridimensionata; il filmato conserva le dimensioni iniziali con eventuali bande nere. Minimizzarla o nasconderla mette in pausa: ripristinala e premi **Resume**. Chiuderla interrompe la registrazione e salva i fotogrammi già acquisiti. Il cambio sorgente durante la registrazione propone di salvare prima il video corrente.
- **Camera Only (Full Frame)** registra tutta l'immagine della webcam, con il microfono scelto. **Hide Camera Preview** nasconde solo l'anteprima. La specchiatura si applica anche al filmato.
- **Audio Input** sceglie microfono, guadagno e indicatore di livello. L'audio di sistema non viene registrato.
- **Ctrl+Alt+Shift+R** avvia/ferma; **Ctrl+Alt+Shift+P** mette in pausa/riprende. Se la combinazione è occupata, usa il menu o il pannello.
- I video H.264/AAC `.mov` vengono salvati sul Desktop individuato da Windows, incluso un Desktop reindirizzato su OneDrive. L'area e la webcam scelte vengono ricordate; le finestre vanno scelte nuovamente dopo il riavvio.

Windows esclude i controlli di YouCam dalla cattura tramite `SetWindowDisplayAffinity`; la webcam rimane visibile nei video dello schermo. Se Windows rifiuta l'esclusione, menu e pannello usano l'immagine dello sfondo precedente alla loro apertura: sotto quel controllo lo sfondo rimane fermo finché viene chiuso.

La cattura delle finestre usa `PrintWindow` con `PW_RENDERFULLCONTENT`. Alcune applicazioni con rendering particolare o contenuti protetti possono restituire immagini nere o incomplete: in questi casi usa schermo/area. Una chiamata bloccata viene interrotta dal punto di vista del registratore dopo tre secondi. La cattura non ripiega automaticamente sul desktop, per evitare di includere altre finestre.

## GPU AMD e periferiche

Con `YOUCAM_ENCODER=auto` (predefinito), viene provata una vera codifica H.264 tramite AMD AMF; se non riesce, viene usato x264 sulla CPU. La disponibilità dipende sia dal driver Radeon sia dalla build di FFmpeg. Per forzare una scelta nella sessione corrente:

```bat
set YOUCAM_ENCODER=software
run.cmd
```

Usa `amf` per richiedere esplicitamente la GPU, oppure `auto` per ripristinare la scelta automatica. Se una diversa build di FFmpeg è necessaria, `YOUCAM_FFMPEG` può indicare il percorso completo del suo `ffmpeg.exe`.

In **Impostazioni → Privacy e sicurezza → Fotocamera / Microfono**, consenti l'accesso alle applicazioni desktop. Se una periferica è occupata, chiudi l'altra applicazione e scegli **Retry Camera**. La prova sul PC Windows con webcam, microfono e Radeon reali rimane necessaria: i test automatici con periferiche sintetiche non la sostituiscono.

## Verifiche

```bat
test.cmd
test-native.cmd
run.cmd --smoke-test
```

`test.cmd` controlla geometria, interfaccia, guadagno, gestione sorgenti e filmati realmente codificati/decodificati, inclusa pausa/ripresa. Usa rendering senza schermo e codifica software.

`test-native.cmd` apre una finestra di prova su un desktop Windows attivo e verifica cattura dietro altre finestre, ridimensionamento, minimizzazione/ripresa, salvataggio, esclusione dei controlli e registrazione/rilascio delle scorciatoie. Non richiede webcam. Durante questi test lascia il desktop sbloccato e attendi la chiusura delle finestre di prova.

`run.cmd --smoke-test` verifica disegno dell'overlay e codifica H.264/AAC con l'encoder selezionato, senza registrare il desktop o la webcam. I test Windows vengono eseguiti anche da GitHub Actions; il runner usa Windows Server e non verifica la GPU AMD del tuo PC.

## Impostazione del codice

Lo spirito del progetto originale resta quello di un piccolo strumento locale e leggibile:

- una funzione precisa: webcam sopra lo schermo e registrazione, con controlli essenziali;
- nessun servizio, account applicativo, telemetria o aggiornamento automatico;
- moduli Python diretti, senza framework aggiuntivi di packaging o architetture a plugin;
- tre dipendenze Python e FFmpeg; le chiamate Windows usano `ctypes` della libreria standard;
- commenti che spiegano le scelte e i vincoli, soprattutto coordinate DPI, handle nativi e acquisizione bloccante.

Il progetto macOS originale usa le API del sistema senza dipendenze esterne. Qui Qt e FFmpeg sono un compromesso esplicito per riutilizzare il port Python e avviare dai sorgenti con Miniforge. L'attenzione ai commenti segue anche quanto descritto da antirez in [Writing system software: code comments](https://antirez.com/news/124).

`app.py` collega i controlli; `camera.py` e `audio.py` gestiscono le periferiche; `recorder.py` scrive segmenti FFmpeg; `sources.py` acquisisce le finestre; `win32.py` contiene le firme delle API native; `display.py` converte le coordinate logiche in pixel. I sorgenti sono nella cartella `youcam/`, le verifiche in `tests/`.

[Licenza MIT](LICENSE).
