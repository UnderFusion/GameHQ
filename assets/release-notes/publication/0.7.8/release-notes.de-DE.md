# GameHQ 0.7.8 (2026-09-22)

## Die wichtigsten Neuerungen

- Das Overlay funktioniert über randlosen Spielen jetzt deutlich zuverlässiger. Es erscheint über dem Spiel, übernimmt auf unterstützten GameInput-Pfaden die Controllereingaben für die Overlay-Navigation, und das Spiel bleibt sichtbar. Bestätigt mit einem kabelgebundenen DualSense in Indiana Jones and the Great Circle im randlosen Fenstermodus (Borderless).
- Auf unterstützten GameInput-Pfaden steuert die Navigation im Overlay nicht mehr das Spiel darunter. Beim Schließen wartet das Overlay, bis Sie alle gedrückten Tasten losgelassen haben, bevor es den Controller an das Spiel zurückgibt.
- Replay-Clips sind sicherer: Schnell aufeinanderfolgende Speichervorgänge überschreiben nie einen vorhandenen Clip, und ein Clip lässt sich direkt nach Aufnahmebeginn speichern.
- Neue Zuordnungsvoreinstellungen: Erstellen und bearbeiten Sie Controller-Belegungen und weisen Sie sie einzelnen Controllern und Spielen zu.
- GameHQ merkt sich, wo Sie aufgehört haben: die letzte Seite, die Kategorie in den Einstellungen, den Galeriefilter und für jedes Spiel die Overlay-Kategorie.

## Overlay

- Das Overlay kann den Controllerfokus übernehmen und bleibt dabei über einem sichtbaren randlosen Spiel, ohne es zu minimieren.
- Auf unterstützten GameInput-Pfaden steuert die Navigation im Overlay nicht mehr gleichzeitig das Spiel.
- Beim Schließen wartet das Overlay kurz, bis gedrückte Tasten losgelassen und die Sticks in die Ruheposition zurückgekehrt sind, bevor es den Controller zurückgibt. So wird eine gehaltene Eingabe nicht ins Spiel übertragen.
- Das Overlay öffnet und schließt schneller und zuverlässiger, und ein schneller wiederholter Tastendruck schließt es nicht mehr direkt nach dem Öffnen. Alt-Tab und andere bewusste Wechsel zu einer anderen App werden berücksichtigt.
- Das Overlay folgt dem richtigen Spiel, wenn das Spiel sein Fenster neu erstellt, sein Fenster verliert oder kurz den Fokus verliert. Wenn eine andere App in den Vordergrund tritt, schließt es sich sauber.
- Das Overlay öffnet sich wieder mit dem zuletzt ausgewählten Screenshot oder Clip. Nach einer neuen Aufnahme beginnt es mit dem neuesten Element.
- Clips starten im Overlay, ohne dass die Vorschau kurz verschwindet.
- Der abgedunkelte Hintergrund erscheint jetzt gemeinsam mit den Overlay-Menüs, statt erst danach eingeblendet zu werden.
- Die Steuerungshinweise am unteren Rand liegen jetzt auf einem kleinen Hintergrund, der Ihrer Einstellung für die Overlay-Abdunkelung folgt, damit sie über dem Spiel lesbar bleiben.
- Das aktuell gespielte Spiel wird beim Durchblättern der Seitenleiste jetzt korrekt hervorgehoben.
- Die Controller-Navigation funktioniert in Overlay-Menüs, in der Galerie und bei der Videowiedergabe besser, und Navigationstasten bleiben von Aufnahme-Tastenkombinationen getrennt.

## Aufnahme und Replay

- Schnell aufeinanderfolgende Speichervorgänge erhalten eindeutige Dateinamen, statt einen vorhandenen Clip zu überschreiben. Ein fehlgeschlagener Export entfernt nie frühere Clips, und Miniaturansichten passen immer zum richtigen Clip.
- Beim Speichern eines Replays wird das bisher aufgezeichnete Material verwendet, auch wenn die eingestellte Pufferdauer noch nicht erreicht ist.
- Ein Replay-Export behält das benötigte Material, wenn Sie das Spiel wechseln oder der Puffer neu startet, und GameHQ wartet vor dem Beenden, bis ein laufender Export abgeschlossen ist.
- Screenshots und Replay-Speicherungen werden sofort bestätigt und zeigen danach eindeutig an, ob sie gespeichert wurden oder fehlgeschlagen sind. Benachrichtigungen werden an Ort und Stelle aktualisiert, und es werden nur begrenzt viele gleichzeitig angezeigt.
- Fehlgeschlagene oder übersprungene Aufnahmen nennen jetzt den Grund, auch wenn Erfolgsbenachrichtigungen ausgeschaltet sind.
- Der Replay-Status zeigt jetzt, ob die Aufnahme tatsächlich begonnen hat und ob verwendbares Material vorhanden ist, mit klaren Meldungen, wenn der Puffer startet, leer ist oder ein Export läuft.
- Manuelle Replay-Sitzungen bleiben jetzt bei Änderungen an den Einstellungen und bei gleichzeitig aufgenommenen HDR-Screenshots erhalten. Eine manuell gestartete, aber nie genutzte Sitzung wird nach einer Weile beendet.
- Das Speichern eines Replays hält die Aufnahme nicht mehr an, während die Miniaturansicht erstellt wird, und gleichzeitig aufgenommene Screenshots legen ihren Ordner zuverlässiger an.
- Neue Option für den gelben Windows-Aufnahmerahmen, mit klareren Informationen zu Berechtigungen und Systemunterstützung. Die Aufnahme funktioniert weiterhin, auch wenn Windows den Rahmen nicht ausblenden kann.

## Controller und Eingabe

- Zuverlässigere Controllererkennung und Eingabeweiterleitung, wenn mehrere Controller oder Eingabequellen verbunden sind.
- Beim Wechsel der Eingabequelle, beim erneuten Verbinden von Controllern und beim Verfolgen gehaltener Tasten werden verpasste, doppelte und hängende Eingaben jetzt vermieden.
- Behoben: Drücke auf die PS-Taste wurden teilweise nicht erkannt, und verzögerte Wiederholungen konnten das Overlay direkt nach dem Schließen erneut öffnen.
- Trigger, Stickklicks und andere Tasten verhalten sich jetzt bei allen unterstützten Eingabequellen gleich, sodass Belegungen vorhersehbarer funktionieren.
- Controller, die GameInput verwenden, funktionieren weiterhin normal, einschließlich der Overlay-Isolierung, wenn Windows keine optionale Unterstützung für die Guide/Share-Tasten bereitstellen kann.
- Der Controller, den Sie in den Einstellungen bearbeiten, bleibt ausgewählt, wenn ein anderer Controller aktiv wird.

## Zuordnungsvoreinstellungen

- Neue Bibliothek für Controller-Belegungen, in der Sie Voreinstellungen erstellen, umbenennen, duplizieren, bearbeiten und löschen können.
- Voreinstellungen lassen sich Controllern und Spielen zuweisen, mit einer Ausweichoption und automatischer Auswahl für das laufende Spiel.
- Ihre bisherigen benutzerdefinierten Belegungen werden automatisch in das Voreinstellungssystem übernommen, und die ursprünglichen Daten bleiben als Sicherung erhalten.
- Beim Wechsel der Voreinstellung werden gehaltene Tasten und laufende Gesten berücksichtigt, damit der Wechsel nicht versehentlich Aktionen auslöst.
- Die Oberfläche trennt die zugewiesene Voreinstellung von der gerade bearbeiteten, schützt ungespeicherte Änderungen und ermöglicht eine Kopie für einen einzelnen Controller.
- Wenn Sie eine verwendete Voreinstellung löschen, werden Sie zuerst nach einem Ersatz oder einer Ausweichoption gefragt, und Änderungen an gemeinsam genutzten Voreinstellungen sind deutlich gekennzeichnet.

## Oberfläche, Einstellungen und Ton

- GameHQ öffnet sich wieder mit der zuletzt verwendeten Seite, Einstellungskategorie und dem zuletzt verwendeten Galeriefilter. Das Overlay merkt sich seine letzte Kategorie für jedes Spiel separat.
- Das Fenster wird jetzt auch auf Monitoren links neben oder oberhalb des Hauptbildschirms korrekt wiederhergestellt. Ein Fenster, das vollständig außerhalb des sichtbaren Bereichs geöffnet würde, wird auf einen angeschlossenen Bildschirm zurückverschoben.
- Hauptfenster und Overlay haben jeweils eine eigene Skalierung von 100 % bis 200 %, die auch nach einem Neustart erhalten bleibt. Kleine Fenster werden besser dargestellt.
- Aufnahmetöne sind lauter und besser unterscheidbar und haben eine eigene Lautstärkeregelung mit Vorschau. Oberflächen- und Aufnahmelautstärke lassen sich bis auf 300 % erhöhen.
- Änderungen an den Einstellungen für Benachrichtigungen, Töne, Aufnahmerahmen oder manuelle Sitzungen verwerfen Ihren Replay-Puffer nicht mehr.
- Beim Öffnen der Galerie bleibt der gespeicherte Filter erhalten. Ein gespeicherter Filter für ein nicht mehr vorhandenes Spiel wird sicher zurückgesetzt. Einstellungskategorien bleiben an ihrem Platz, auch wenn sich ihre Reihenfolge ändert.
- Leere Inhalte und Überlagerungsprobleme beim Ändern der Oberflächenskalierung wurden behoben, und Menüs erscheinen jetzt über dem Hauptinhalt.
- Die Belegungseinstellungen zeigen, wie lange ein Halten dauert, und erklären, wie Haltegesten funktionieren.

## Updates, Sprachen und Diagnose

- Update-Hinweise erscheinen in Ihrer ausgewählten Sprache, greifen auf Englisch zurück, wenn keine Übersetzung verfügbar ist, und bleiben nach dem Laden auch offline lesbar.
- Update-Hinweise können einen optionalen Link zum GitHub-Release enthalten, den Sie auch mit dem Controller erreichen.
- Die Übersetzungen wurden in allen unterstützten Sprachen um die neuen Meldungen zu Aufnahme, Ton, Aufnahmerahmen, Halten, Voreinstellungen und Fokus erweitert.
- Kopierte Diagnosedaten enthalten jetzt aktive Belegungen, die Auswahl der Voreinstellung, Wechsel der Eingabequelle und den Fokusstatus des Overlays; Gerätekennungen werden dabei anonymisiert.
- Die Aufnahmediagnose verfolgt jede Anfrage vom Tastendruck bis zur gespeicherten Datei, wodurch sich Probleme leichter eingrenzen lassen. Tonprobleme werden mit einer deutlichen Warnung gemeldet.

## Bekannte Einschränkungen

- Ob das Overlay den Controller vom Spiel isoliert, hängt vom Spiel ab und davon, wie es den Controller ausliest. Ein kabelgebundener DualSense über GameInput ist gut getestet. Für XInput, Raw Input, direktes HID, Steam Input oder virtuelle Controller ist dies nicht garantiert.
- Der Controllerwechsel bei laufendem DSX ist nur teilweise überprüft. DSX-Konfigurationen bleiben unter Ihrer Kontrolle; GameHQ installiert und verwaltet keine Treiber für virtuelle Controller oder zum Ausblenden von Geräten.
- Manche Spiele pausieren oder reagieren anderweitig, wenn sie den Fokus verlieren. Windows oder eine andere Aufnahme-App kann den gelben Aufnahmerahmen weiterhin anzeigen.
- Wenn für 0.7.8 keine lokalisierten Versionshinweise verfügbar sind, zeigt GameHQ die englische Fassung an.
