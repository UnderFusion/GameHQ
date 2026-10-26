# GameHQ 0.8.0 (2026-10-26)

## Ringraziamenti speciali

- Un grande grazie a [@galcn10](https://github.com/galcn10), creatore di [Console Overlay for Playnite](https://github.com/galcn10/ConsoleOverlay), per aver testato GameHQ, aver condiviso feedback preziosi e aver realizzato un'ottima integrazione con Playnite. Il tuo supporto ha contribuito a rendere migliore GameHQ 0.8.0!

## In evidenza

- Le clip possono registrare l'audio del gioco stesso. Con la nuova sorgente Audio del gioco, GameHQ registra il gioco su qualunque dispositivo lo riproduca (cuffie, Bluetooth, HDMI o altoparlanti) ed esclude altre app come Discord o Spotify.
- I problemi di registrazione audio ora vengono rilevati e segnalati. Se la sorgente audio selezionata non può essere registrata, il buffer di replay si ferma e ti spiega il motivo e quale sorgente scegliere al suo posto, invece di salvare silenziosamente clip senza audio. GameHQ non cambia mai la sorgente audio di sua iniziativa.
- Audio e video restano sincronizzati. Le clip salvate dopo una schermata di caricamento, una pausa o un lungo blocco mantengono i tempi corretti, e il buffer di replay si riavvia da solo quando un gioco cambia risoluzione.
- Gli screenshot HDR non si bloccano più. Uno screenshot di un gioco in pausa o di un menu statico in HDR viene salvato subito. Se non arriva alcun fotogramma, il tentativo fallisce in modo pulito dopo qualche secondo e la pressione successiva funziona.
- Eliminazione più sicura con il controller. Nella conferma di eliminazione, Croce ora preme il pulsante evidenziato e Cerchio annulla. In precedenza, Croce poteva eliminare il file anche quando era evidenziato Annulla.
- L'eliminazione protegge i tuoi originali. Un'acquisizione creata da GameHQ ora viene spostata nel Cestino quando la elimini, così puoi ripristinarla. I file nelle cartelle monitorate non vengono mai eliminati.

## Novità

- Impostazioni › Replay › Sorgente audio offre Audio del gioco, Audio del desktop e Disattivato. Audio del gioco è l'impostazione predefinita su Windows 11. Audio del desktop registra tutto ciò che passa per l'uscita predefinita di Windows, incluse altre app, ed è l'impostazione predefinita nelle versioni precedenti di Windows.
- Audio del gioco usa l'acquisizione audio per singola app integrata in Windows e non interviene mai sul processo del gioco. Se in una versione precedente avevi disattivato di proposito l'audio di sistema, resta disattivato.
- Aggiornamenti beta facoltativi: un nuovo interruttore Includi versioni beta in Impostazioni › Generale e in Informazioni consente al programma di aggiornamento di proporre versioni beta. È disattivato per impostazione predefinita, quindi le installazioni stabili ricevono solo aggiornamenti stabili.
- Quando GameHQ va in crash, prova a salvare un piccolo file di dump del crash nella sua cartella dei registri per aiutare a diagnosticare il problema. Non viene caricato nulla. Alcuni crash, come quelli in cui Windows termina immediatamente il programma, possono ancora chiudere GameHQ senza un dump.
- Il registro raccoglie più dettagli sugli screenshot salvati e sugli errori di avvio, così un singolo file di registro basta più spesso per capire una segnalazione di problema.

## Correzioni e miglioramenti

- Le clip non risultano più mute a causa di una vecchia impostazione interna che manteneva l'audio del desktop disattivato per impostazione predefinita.
- L'audio in qualsiasi formato di riproduzione di Windows viene convertito correttamente invece di perdere la traccia audio.
- I momenti silenziosi vengono registrati come silenzio, così l'audio dopo un tratto silenzioso resta allineato all'immagine.
- La registrazione di Audio del gioco si riavvia in modo più affidabile quando un gioco si riavvia o passi a un altro gioco.
- Le parti del buffer di replay che non è stato possibile salvare o che non possono essere rilette vengono escluse, così non possono danneggiare una clip salvata.
- Le notifiche di GameHQ stesso, come l'avviso di screenshot salvato, non compaiono più nei tuoi screenshot né nelle registrazioni e negli streaming dell'intero schermo.
- Uno screenshot sostituito da un'altra immagine con lo stesso nome file riceve una nuova miniatura alla successiva scansione della libreria.
- Le cartelle delle acquisizioni vengono analizzate in background, così GameHQ resta reattivo anche con raccolte di grandi dimensioni.
- Browser, editor e altre finestre che non sono giochi non compaiono più come giochi nella tua libreria.
- Durante un'eliminazione multipla, la selezione resta fissa finché la conferma è aperta, così le azioni del controller in background non possono cambiare quali file vengono eliminati.
- La finestra di eliminazione dell'overlay riceve l'input del controller finché è aperta e si chiude quando l'overlay viene nascosto.
- L'overlay salta le finestre che hanno smesso di rispondere invece di attenderle.
- La pagina Impostazioni non torna più all'inizio quando scorri con il mouse dopo essere tornato a GameHQ.
- Scegliere Valori predefiniti integrati o rimuovere una preimpostazione nell'editor della mappatura del controller non blocca più la selezione della preimpostazione.

## Controller e Playnite

- Le scorciatoie del controller continuano a funzionare nei giochi avviati al di fuori di Steam, come un gioco locale avviato da Playnite. Quando Windows smette di inviare a GameHQ un DualSense collegato mentre il gioco ha il focus, GameHQ legge il controller direttamente, in sola lettura, e torna all'input normale non appena Windows riprende a inviarlo.
- Le scorciatoie e le ripetizioni dei pulsanti tenuti premuti terminano sempre al rilascio della stessa pressione, anche quando il rilascio viene segnalato tramite GameInput o il controller viene scollegato mentre un pulsante è premuto.
- I controller scollegati definitivamente vengono ripuliti, il controllo di HidHide non rallenta più l'interfaccia e i dispositivi GameInput vengono ricontrollati solo quando cambia qualcosa.
- I giochi di Playnite restano riconosciuti anche quando Playnite non riesce a identificare il processo del gioco.
- Esperienza Xbox a schermo intero di Windows: quando l'interfaccia Xbox è in primo piano, GameHQ usa il gioco avviato da Playnite per screenshot e clip. GameHQ Playnite Integration 0.4.13, disponibile separatamente come versione preliminare, avvia anche GameHQ quando la modalità Xbox ne ritarda l'avvio all'accesso.

## Stabilità e affidabilità

- La pulizia delle registrazioni non completate rimuove solo i file temporanei creati da GameHQ stesso.
- Le cartelle delle acquisizioni dei giochi restano sempre all'interno della tua cartella delle acquisizioni.
- L'avvio non fallisce più quando la libreria contiene percorsi di cartella duplicati; le voci vengono invece unite.
- Se GameHQ non riesce ad avviarsi, ne spiega il motivo nella tua lingua e lo conserva nel registro.
- L'importazione di un profilo portatile in un GameHQ installato ora funziona anche quando la copia installata è già stata avviata.
- Sicurezza rafforzata: caricamento delle DLL più sicuro, controlli più rigorosi sulle connessioni di integrazione locali e gestione più sicura dei file di aggiornamento.
- Il programma di aggiornamento gestisce meglio un primo avvio lento e mantiene intatti i propri dati se viene interrotto.
- La condivisione su Telegram si chiude in modo più sicuro.

## Installazione e aggiornamento

- Aggiornamento dalla 0.7.9: esegui il programma di installazione della 0.8.0 sopra l'installazione attuale.
- Programma di installazione: installa per l'utente corrente e sostituisce una 0.7.9 installata o una beta precedente.
- Versione portatile: estrai lo ZIP in una nuova cartella ed esegui GameHQ.exe.
- Se hai testato la 0.8.0 Beta 2, 3 o 4, installa la 0.8.0 con il programma di installazione o con un nuovo ZIP portatile, perché quelle beta non sono state proposte tramite il programma di aggiornamento.

## Limitazioni note

- Audio del gioco richiede Windows 11. Nelle versioni precedenti di Windows, GameHQ usa Audio del desktop, che registra anche le altre app in riproduzione sull'uscita predefinita.
- Un gioco che assume il controllo esclusivo di un controller potrebbe comunque impedire le scorciatoie in background.
- Il supporto per l'esperienza Xbox a schermo intero di Windows non è ancora stato confermato dai tester su un PC che la utilizza.
- I file binari per Windows non sono firmati con Authenticode; Windows SmartScreen potrebbe mostrare un avviso Autore sconosciuto.
