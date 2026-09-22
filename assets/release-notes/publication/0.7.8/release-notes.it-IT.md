# GameHQ 0.7.8 (2026-09-22)

## In evidenza

- L'overlay ora funziona in modo molto più affidabile sopra i giochi in modalità senza bordi. Compare sopra il gioco, riceve l'input del controller per la navigazione nell'overlay sui percorsi GameInput supportati e il gioco resta visibile. Verificato con un DualSense cablato in Indiana Jones and the Great Circle in modalità senza bordi.
- Sui percorsi GameInput supportati, la navigazione nell'overlay non controlla più il gioco sottostante. Alla chiusura dell'overlay, GameHQ aspetta che tu rilasci i pulsanti tenuti premuti prima di restituire il controller al gioco.
- Le clip di replay sono più sicure: i salvataggi rapidi ripetuti non sovrascrivono mai una clip esistente e puoi salvare una clip subito dopo l'avvio della registrazione.
- Nuove preimpostazioni di mappatura: crea, modifica e assegna layout del controller per ogni controller e per ogni gioco.
- GameHQ ricorda dove eri rimasto: l'ultima pagina, la categoria delle impostazioni, il filtro della galleria e la categoria dell'overlay di ogni gioco.

## Overlay

- L'overlay può prendere il focus del controller restando sopra un gioco visibile in modalità senza bordi, senza ridurlo a icona.
- Sui percorsi GameInput supportati, la navigazione nell'overlay non controlla più anche il gioco.
- Quando chiudi l'overlay, attende brevemente che i pulsanti premuti e gli stick tornino a riposo prima di restituire il controller, così un input tenuto premuto non passa al gioco.
- L'apertura e la chiusura dell'overlay sono più rapide e affidabili, e una pressione ripetuta veloce non lo chiude più subito dopo l'apertura. Alt-Tab e gli altri passaggi intenzionali a un'altra app vengono rispettati.
- L'overlay segue il gioco corretto quando il gioco ricrea la propria finestra, la perde o perde brevemente il focus. Si chiude in modo pulito quando un'altra app passa in primo piano.
- L'overlay si riapre sullo screenshot o sulla clip selezionati per ultimi. Dopo una nuova acquisizione parte dall'elemento più recente.
- Le clip iniziano la riproduzione nell'overlay senza che l'anteprima scompaia per un attimo.
- Lo sfondo oscurato ora compare insieme ai menu dell'overlay invece di apparire in dissolvenza dopo di essi.
- I suggerimenti dei comandi in basso ora hanno un piccolo sfondo che segue l'impostazione di oscuramento dell'overlay, così restano leggibili sopra il gioco.
- Il gioco a cui stai giocando ora viene evidenziato correttamente quando scorri la barra laterale.
- La navigazione con il controller funziona meglio nei menu dell'overlay, nella galleria e nella riproduzione video, e i pulsanti di navigazione restano separati dalle scorciatoie di acquisizione.

## Acquisizione e replay

- I salvataggi rapidi ripetuti ricevono nomi file univoci invece di sovrascrivere una clip esistente. Un'esportazione non riuscita non rimuove mai le clip precedenti e le miniature corrispondono sempre alla clip giusta.
- Il salvataggio di un replay usa il materiale registrato fino a quel momento, anche prima che il buffer abbia raggiunto la durata impostata.
- Un'esportazione del replay conserva il materiale di cui ha bisogno anche se cambi gioco o il buffer si riavvia, e GameHQ attende il completamento di un'esportazione in corso prima di chiudersi.
- Screenshot e salvataggi dei replay vengono confermati subito, poi mostrano chiaramente se il salvataggio è riuscito o meno. Le notifiche si aggiornano senza duplicarsi e ne viene mostrato solo un numero limitato alla volta.
- Le acquisizioni non riuscite o saltate ora spiegano il motivo, anche con le notifiche di successo disattivate.
- Lo stato del replay ora indica se la registrazione è davvero iniziata e se c'è materiale utilizzabile, con messaggi chiari quando il buffer è in avvio, è vuoto o c'è un'esportazione in corso.
- Le sessioni di replay manuali ora restano attive dopo le modifiche alle impostazioni e con screenshot HDR acquisiti contemporaneamente. Una sessione manuale avviata ma mai usata si disattiva dopo un po'.
- Il salvataggio di un replay non blocca più l'acquisizione durante la creazione della miniatura, e gli screenshot acquisiti nello stesso momento creano la propria cartella in modo più affidabile.
- Nuova opzione per il bordo giallo di acquisizione di Windows, con informazioni più chiare su autorizzazioni e supporto del sistema. La registrazione funziona anche quando Windows non riesce a nascondere il bordo.

## Controller e input

- Rilevamento dei controller e instradamento dell'input più affidabili quando sono collegati più controller o più sorgenti di input.
- Il cambio di sorgente di input, la riconnessione dei controller e il monitoraggio dei pulsanti tenuti premuti ora evitano pressioni perse, pressioni doppie e input bloccati.
- Corrette le pressioni del pulsante PS che non venivano rilevate e le pressioni ripetute in ritardo che potevano riaprire l'overlay subito dopo averlo chiuso.
- Grilletti, clic degli stick e altri pulsanti ora si comportano allo stesso modo su tutte le sorgenti di input supportate, così le associazioni funzionano in modo più prevedibile.
- I controller che usano GameInput continuano a funzionare normalmente, incluso l'isolamento dell'overlay, quando Windows non può fornire il supporto opzionale ai pulsanti Guide/Share.
- Il controller che stai modificando nelle impostazioni resta selezionato quando diventa attivo un altro controller.

## Preimpostazioni di mappatura

- Nuova libreria di preimpostazioni per i layout del controller, in cui puoi creare, rinominare, duplicare, modificare ed eliminare le preimpostazioni.
- Le preimpostazioni possono essere assegnate a controller e giochi, con una scelta di riserva e la selezione automatica per il gioco in esecuzione.
- Le tue associazioni personalizzate vengono trasferite automaticamente nel sistema delle preimpostazioni e i dati originali vengono conservati come backup.
- Il cambio di preimpostazione tiene conto dei pulsanti tenuti premuti e dei gesti in corso, così il cambio non attiva azioni per errore.
- L'interfaccia distingue la preimpostazione assegnata da quella che stai modificando, protegge le modifiche non salvate e ti permette di creare una copia per un singolo controller.
- Se elimini una preimpostazione in uso, ti viene prima chiesta una sostituta o una scelta di riserva, e le modifiche alle preimpostazioni condivise sono indicate chiaramente.

## Interfaccia, impostazioni e audio

- GameHQ si riapre sull'ultima pagina, sulla categoria delle impostazioni e sul filtro della galleria che hai usato. L'overlay ricorda la sua ultima categoria separatamente per ogni gioco.
- La finestra ora viene ripristinata correttamente sui monitor posizionati a sinistra o sopra lo schermo principale. Una finestra che si aprirebbe completamente fuori dallo schermo viene riportata su uno schermo collegato.
- La finestra principale e l'overlay hanno ciascuno la propria scala, dal 100% al 200%, mantenuta tra un riavvio e l'altro. Le finestre piccole hanno un layout migliore.
- I suoni di acquisizione sono più forti e più distinguibili, e hanno un proprio controllo del volume e un'anteprima. Il volume dell'interfaccia e dell'acquisizione può arrivare al 300%.
- Modificare le impostazioni di notifiche, suoni, bordo di acquisizione o sessioni manuali non azzera più il buffer di replay.
- All'apertura, la galleria mantiene il filtro salvato. Un filtro salvato per un gioco che non esiste più torna in sicurezza a un'impostazione predefinita. Le categorie delle impostazioni restano al loro posto anche se il loro ordine cambia.
- Corretti i contenuti vuoti e i problemi di sovrapposizione degli elementi quando si cambia la scala dell'interfaccia; i menu ora compaiono sopra il contenuto principale.
- I controlli delle associazioni mostrano quanto dura una pressione prolungata e spiegano come funzionano i gesti di pressione prolungata.

## Aggiornamenti, lingue e diagnostica

- Le note di aggiornamento compaiono nella lingua selezionata, passano all'inglese quando una traduzione non è disponibile e restano leggibili offline una volta caricate.
- Le note di aggiornamento possono includere un link facoltativo alla release su GitHub, raggiungibile anche con il controller.
- Le traduzioni sono state estese in tutte le lingue supportate per i nuovi messaggi su acquisizione, suoni, bordo, pressione prolungata, preimpostazioni e focus.
- La diagnostica copiata ora include le associazioni attive, la preimpostazione selezionata, i cambi di sorgente di input e lo stato del focus dell'overlay, con gli identificatori dei dispositivi resi anonimi.
- La diagnostica di acquisizione segue ogni richiesta dalla pressione del pulsante fino al file salvato, rendendo più facile individuare i problemi. I problemi audio vengono segnalati con un avviso chiaro.

## Limitazioni note

- Il fatto che l'overlay isoli il controller dal gioco dipende dal gioco e da come legge il controller. Un DualSense cablato che usa GameInput è stato ampiamente testato. Non è garantito con XInput, Raw Input, HID diretto, Steam Input o controller virtuali.
- Il cambio di controller con DSX in esecuzione è verificato solo in parte. Le configurazioni di DSX restano sotto il tuo controllo; GameHQ non installa né gestisce driver per controller virtuali o per nascondere i dispositivi.
- Alcuni giochi si mettono in pausa o reagiscono quando perdono il focus. Windows o un'altra app di acquisizione possono lasciare visibile il bordo giallo di registrazione.
- Se una nota di rilascio localizzata per la versione 0.7.8 non è disponibile, GameHQ mostra quella in inglese.
