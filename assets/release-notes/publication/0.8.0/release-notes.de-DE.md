# GameHQ 0.8.0 (2026-10-26)

## Besonderer Dank

- Ein großes Dankeschön an [@galcn10](https://github.com/galcn10), den Entwickler von [Console Overlay for Playnite](https://github.com/galcn10/ConsoleOverlay), für das Testen von GameHQ, wertvolles Feedback und eine großartige Playnite-Integration. Ihre Unterstützung hat GameHQ 0.8.0 besser gemacht!

## Die wichtigsten Neuerungen

- Verbesserte automatische Update-Erkennung. GameHQ sucht jetzt kurz nach jedem Start und stündlich während der Ausführung nach neuen Versionen, sodass Sie Updates viel früher entdecken.
- Clips können den eigenen Ton Ihres Spiels aufnehmen. Mit der neuen Audioquelle Spielaudio nimmt GameHQ das Spiel auf dem Gerät auf, über das es wiedergegeben wird (Kopfhörer, Bluetooth, HDMI oder Lautsprecher), und lässt andere Apps wie Discord oder Spotify außen vor.
- Probleme bei der Audioaufnahme werden jetzt erkannt und gemeldet. Wenn die ausgewählte Audioquelle nicht aufgenommen werden kann, stoppt der Replay-Puffer und nennt Ihnen den Grund sowie die Quelle, die Sie stattdessen wählen sollten, statt stillschweigend Clips ohne Ton zu speichern. GameHQ wechselt die Audioquelle nie von selbst.
- Ton und Bild bleiben synchron. Clips, die nach einem Ladebildschirm, einer Pause oder einem langen Hänger gespeichert werden, behalten das richtige Timing, und der Replay-Puffer startet von selbst neu, wenn ein Spiel die Auflösung ändert.
- HDR-Screenshots bleiben nicht mehr hängen. Ein Screenshot eines pausierten Spiels oder eines statischen Menüs in HDR wird sofort gespeichert. Kommt überhaupt kein Bild an, schlägt der Versuch nach einigen Sekunden sauber fehl, und der nächste Tastendruck funktioniert.
- Sichereres Löschen mit dem Controller. Kreuz in einer Löschbestätigung betätigt jetzt die hervorgehobene Schaltfläche, und Kreis bricht ab. Bisher konnte Kreuz die Datei selbst dann löschen, wenn Abbrechen hervorgehoben war.
- Löschen schützt Ihre Originale. Eine von GameHQ erstellte Aufnahme wird beim Löschen jetzt in den Papierkorb verschoben, sodass Sie sie wiederherstellen können. Dateien in überwachten Ordnern werden nie gelöscht.

## Neue Funktionen

- Einstellungen › Replay › Audioquelle bietet Spielaudio, Desktop-Audio und Aus. Spielaudio ist unter Windows 11 die Standardeinstellung. Desktop-Audio nimmt alles auf, was über die Standardausgabe von Windows läuft, einschließlich anderer Apps, und ist unter älteren Windows-Versionen die Standardeinstellung.
- Spielaudio nutzt die in Windows integrierte Audioaufnahme pro App und greift nie in den Spielprozess ein. Wenn Sie den Systemsound in einer früheren Version bewusst ausgeschaltet haben, bleibt er aus.
- Optionale Beta-Updates: Ein neuer Schalter Betaversionen einbeziehen unter Einstellungen › Allgemein und unter Info lässt den Updater Betaversionen anbieten. Er ist standardmäßig ausgeschaltet, sodass stabile Installationen nur stabile Updates erhalten.
- Wenn GameHQ abstürzt, versucht es, ein kleines Absturzabbild (Crash-Dump) in seinem Protokollordner zu speichern, um die Fehlersuche zu erleichtern. Es wird nichts hochgeladen. Manche Abstürze, etwa solche, bei denen Windows GameHQ sofort beendet, können GameHQ weiterhin ohne Abbild schließen.
- Das Protokoll erfasst mehr Details zu gespeicherten Screenshots und Startfehlern, sodass eine einzelne Protokolldatei häufiger ausreicht, um einen Problembericht nachzuvollziehen.

## Fehlerbehebungen und Verbesserungen

- Clips sind nicht mehr stumm wegen einer alten internen Einstellung, die Desktop-Audio standardmäßig ausgeschaltet ließ.
- Audio in jedem Wiedergabeformat von Windows wird korrekt konvertiert, statt dass die Tonspur verloren geht.
- Leise Momente werden als Stille aufgezeichnet, sodass der Ton nach einer ruhigen Phase synchron zum Bild bleibt.
- Die Aufnahme von Spielaudio startet zuverlässiger neu, wenn ein Spiel neu startet oder Sie das Spiel wechseln.
- Teile des Replay-Puffers, die nicht gespeichert werden konnten oder sich nicht wieder einlesen lassen, werden ausgelassen, sodass sie einen gespeicherten Clip nicht beschädigen können.
- Die eigenen Benachrichtigungen von GameHQ, etwa der Hinweis, dass ein Screenshot gespeichert wurde, erscheinen nicht mehr in Ihren Screenshots oder in Aufnahmen und Streams des gesamten Bildschirms.
- Ein Screenshot, der durch ein anderes Bild mit demselben Dateinamen ersetzt wurde, erhält beim nächsten Durchsuchen der Bibliothek eine neue Miniatur.
- Aufnahmeordner werden im Hintergrund durchsucht, sodass GameHQ auch bei großen Sammlungen reaktionsschnell bleibt.
- Browser, Editoren und andere Fenster, die keine Spiele sind, erscheinen nicht mehr als Spiele in Ihrer Bibliothek.
- Beim Löschen mehrerer Dateien bleibt die Auswahl unverändert, solange die Bestätigung geöffnet ist, sodass Controller-Aktionen im Hintergrund nicht ändern können, welche Dateien gelöscht werden.
- Der Löschdialog des Overlays übernimmt die Controllereingaben, solange er geöffnet ist, und schließt sich, wenn das Overlay ausgeblendet wird.
- Das Overlay überspringt Fenster, die nicht mehr reagieren, statt auf sie zu warten.
- Die Seite Einstellungen springt nicht mehr an den Anfang zurück, wenn Sie nach dem Zurückwechseln zu GameHQ mit der Maus scrollen.
- Die Auswahl von Integrierte Standardeinstellungen oder das Entfernen einer Voreinstellung im Editor für die Controllerbelegung friert die Auswahl der Voreinstellung nicht mehr ein.

## Controller und Playnite

- Controller-Tastenkürzel funktionieren weiterhin in Spielen, die außerhalb von Steam gestartet werden, etwa einem lokalen Spiel, das aus Playnite gestartet wurde. Wenn Windows einen verbundenen DualSense nicht mehr an GameHQ weiterleitet, während das Spiel den Fokus hat, liest GameHQ den Controller direkt und nur lesend aus und kehrt zur normalen Eingabe zurück, sobald Windows ihn wieder weiterleitet.
- Tastenkürzel und Wiederholungen bei gehaltener Taste enden immer mit dem Loslassen desselben Tastendrucks, auch wenn das Loslassen über GameInput gemeldet wird oder der Controller bei gedrückter Taste abgezogen wird.
- Dauerhaft getrennte Controller werden bereinigt, die HidHide-Prüfung verlangsamt die Oberfläche nicht mehr, und GameInput-Geräte werden nur dann erneut geprüft, wenn sich etwas ändert.
- Playnite-Spiele werden auch dann weiterhin erkannt, wenn Playnite den Prozess des Spiels nicht ermitteln kann.
- Xbox-Vollbildoberfläche von Windows: Wenn die Xbox-Shell im Vordergrund ist, verwendet GameHQ für Screenshots und Clips das aus Playnite gestartete Spiel. GameHQ Playnite Integration 0.4.13, separat als Vorabversion erhältlich, startet GameHQ außerdem, wenn der Xbox-Modus den Start bei der Anmeldung verzögert.

## Stabilität und Zuverlässigkeit

- Beim Aufräumen unfertiger Aufnahmen werden nur temporäre Dateien entfernt, die GameHQ selbst erstellt hat.
- Die Aufnahmeordner der einzelnen Spiele bleiben immer innerhalb Ihres Aufnahmeordners.
- Der Start schlägt nicht mehr fehl, wenn die Bibliothek doppelte Ordnerpfade enthält; die Einträge werden stattdessen zusammengeführt.
- Wenn GameHQ nicht starten kann, erklärt es den Grund in Ihrer Sprache und hält ihn im Protokoll fest.
- Das Importieren eines portablen Profils in ein installiertes GameHQ funktioniert jetzt auch, wenn die installierte Kopie bereits gestartet wurde.
- Erhöhte Sicherheit: sichereres Laden von DLLs, strengere Prüfungen lokaler Integrationsverbindungen und sicherere Verarbeitung von Updatedateien.
- Der Updater kommt mit einem langsamen ersten Start besser zurecht und hält seine Aufzeichnungen intakt, wenn er unterbrochen wird.
- Das Teilen über Telegram wird sicherer beendet.

## Installation und Aktualisierung

- Aktualisierung von 0.7.9: Führen Sie das Installationsprogramm von 0.8.0 über Ihre bestehende Installation aus.
- Installationsprogramm: Installiert für den aktuellen Benutzer und ersetzt eine installierte 0.7.9 oder eine frühere Beta.
- Portable Version: Entpacken Sie die ZIP-Datei in einen neuen Ordner und führen Sie GameHQ.exe aus.
- Wenn Sie 0.8.0 Beta 2, 3 oder 4 getestet haben, installieren Sie 0.8.0 mit dem Installationsprogramm oder einer frischen portablen ZIP-Datei, da diese Betas nicht über den Updater angeboten wurden.

## Bekannte Einschränkungen

- Spielaudio erfordert Windows 11. Unter älteren Windows-Versionen verwendet GameHQ Desktop-Audio, das auch andere Apps aufnimmt, die über die Standardausgabe wiedergegeben werden.
- Ein Spiel, das die exklusive Kontrolle über einen Controller übernimmt, kann Tastenkürzel im Hintergrund weiterhin verhindern.
- Die Unterstützung der Xbox-Vollbildoberfläche von Windows wurde noch nicht von Testern auf einem PC bestätigt, auf dem sie läuft.
- Die Windows-Programmdateien sind nicht mit Authenticode signiert; Windows SmartScreen zeigt möglicherweise die Warnung Unbekannter Herausgeber an.
