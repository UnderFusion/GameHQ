# GameHQ 0.7.9 (2026-10-02)

## Die wichtigsten Neuerungen

- Aufnahmen teilen. Senden Sie einen Screenshot oder Clip aus der Galerie, der Vollbildansicht oder dem Overlay an Telegram Desktop, Discord, einen Discord-Kanal oder in die Zwischenablage, ohne sich über GameHQ irgendwo anmelden zu müssen.
- Zuverlässiger DualSense über USB und Bluetooth. Tasten und Sticks werden über Bluetooth korrekt gelesen, der Wechsel zwischen Kabel und Bluetooth erfordert keinen Neustart mehr, und das Trennen des Controllers während eines Spiels bringt GameHQ nicht mehr zum Absturz. Von HidHide ausgeblendete DualSense- und DualShock-4-Controller lassen sich weiterhin verwenden, sobald GameHQ in HidHide zugelassen ist.
- Ein Overlay nach Ihren Wünschen. Ein Live-Bereich Overlay-Optionen legt Ränder, Abstände, Miniaturgröße, Oberflächengröße und Steuerungshinweise fest, und die Seitenleiste lässt sich ausklappen, einklappen, auf Automatisch stellen oder in der Breite ändern.
- Vollbildansicht im Overlay. Öffnen Sie einen Screenshot im Vollbild über Ihrem Spiel, blättern Sie mit L1/R1 durch Screenshots und Clips und markieren Sie Favoriten direkt in der Ansicht.
- Eine Galerie, die Ihren Dateien folgt. Im Explorer gelöschte, wiederhergestellte oder kopierte Aufnahmen erscheinen und verschwinden sofort, und Spiele lassen sich oben in der Liste anheften.
- Kleinere Screenshot-Dateien. JPEG mit 90 % Qualität ist jetzt die Standardeinstellung, PNG bleibt weiterhin verfügbar.
- Mehr Kontrolle über Benachrichtigungen. Die Hinweise zu Aufnahmeanfragen, Steam-Input-Konflikten und ausgeblendeten Controllern lassen sich einzeln steuern, und Mausnutzer können Benachrichtigungen vorzeitig schließen.
- Weitere Teile der Oberfläche lassen sich mit dem Controller bedienen, darunter die Sprachauswahl, GameHQ unterstützen, die Werkzeuge und jeder Schritt beim Teilen.

## Teilen

- Teilen Sie jeden gespeicherten Screenshot oder Clip: Drücken Sie auf einer Aufnahme Quadrat und wählen Sie Teilen, klicken Sie auf das Teilen-Symbol einer Aufnahmekachel oder verwenden Sie die Schaltfläche Teilen in der Vollbildansicht. Das funktioniert in der Galerie, in der Ansicht und im Overlay gleich und ist vollständig mit dem Controller bedienbar (Kreuz wählt aus, Kreis geht zurück).
- Telegram Desktop öffnet seine eigene Chatauswahl mit genau dieser einen Datei. GameHQ meldet sich nie bei Telegram an.
- Discord kopiert die Aufnahme, öffnet Discord und lässt Sie sie in einen beliebigen Chat einfügen. GameHQ meldet sich nie bei Discord an.
- Discord-Kanäle veröffentlichen eine Aufnahme direkt in einem Kanal, und zwar über einen Webhook-Link, den Sie unter Einstellungen › Teilen hinzufügen. Heften Sie die am häufigsten genutzten Kanäle an. Der Beitrag erscheint unter dem Namen des Webhooks, nicht unter Ihrem Discord-Konto, und der Link wird in der Windows-Anmeldeinformationsverwaltung gespeichert.
- In die Zwischenablage kopieren steht als eigenes Ziel zur Verfügung.
- Wenn Sie aus dem Overlay teilen, wird Telegram oder Discord über Ihrem Spiel in den Vordergrund geholt, damit Sie den Versand abschließen können.
- GameHQ meldet Gesendet nur, wenn ein Dienst die Zustellung bestätigt, und fragt nach, bevor dieselbe Aufnahme zweimal an dasselbe Ziel geteilt wird.
- Einstellungen › Teilen ist eine neue Seite mit einem Schalter für jedes Ziel; wenn Sie eines ausschalten, bleibt seine Konfiguration erhalten.
- Andere Programme auf Ihrem PC können eigene Ziele zum Teilen hinzufügen. Das ist standardmäßig ausgeschaltet: Aktivieren Sie Erweiterungen anderer Programme zulassen unter Einstellungen › Teilen und starten Sie GameHQ neu.

## Overlay

- Kreuz auf einem Screenshot öffnet ihn im Vollbild. Mit L1/R1 oder D-Pad links/rechts blättern Sie durch Screenshots und Clips (Clips werden im Vollbild abgespielt), und Kreis kehrt zur Aufnahmeleiste zurück, und zwar zu dem Element, bei dem Sie zuletzt waren. Kreuz auf einem Clip spielt ihn weiterhin in der Vorschau ab, und links/rechts spulen ihn dann vor oder zurück.
- Ein Zahnrad über dem Schalter der Seitenleiste öffnet die Overlay-Optionen: Steuerungshinweise ein oder aus, die vier äußeren Ränder (bis hinunter auf 0), die Oberflächengröße (jetzt auch mit 75 % und 90 %), die Miniaturgröße und den Abstand zwischen Seitenleiste, Miniaturen und Vorschau. Änderungen werden sofort angezeigt und gespeichert; Overlay-Layout zurücksetzen stellt die Standardwerte wieder her. Funktioniert mit Controller, Maus und Tastatur.

## Galerie und Oberfläche

- Spiele anheften: Bewegen Sie den Mauszeiger in der Seitenleiste über ein Spiel und klicken Sie auf die Stecknadel, oder bewegen Sie den Controller-Cursor darauf und drücken Sie Dreieck. Angeheftete Spiele bleiben sowohl im Hauptfenster als auch im Overlay oben in der Liste.
- In der Vollbildansicht markiert Dreieck (oder die Taste, die Sie für Favoriten zugewiesen haben) den angezeigten Screenshot oder Clip als Favorit und entfernt die Markierung wieder. Ein Herz zeigt den Status an und kann angeklickt werden.
- Einklappbare Seitenleiste im Hauptfenster und im Overlay. Eine Schaltfläche wechselt zwischen Ausgeklappt (Standard), Automatisch und Eingeklappt. Automatisch zeigt beim Durchblättern eine schmale Symbolleiste und öffnet die vollständige Seitenleiste, sobald Sie hineinwechseln. Jedes Fenster merkt sich seine eigene Wahl.
- Ziehen Sie den Rand der Seitenleiste, um sie schmaler oder breiter zu machen; ein Doppelklick auf den Rand stellt die Standardbreite wieder her.
- Ein kleiner Pfeil über Werkzeuge klappt Hilfe, Info, GameHQ unterstützen und die Sprachauswahl ein, sodass nur Einstellungen und die Schaltfläche für den Seitenleistenmodus sichtbar bleiben.

## Änderungen und Verbesserungen

- Screenshots werden standardmäßig als JPEG mit 90 % Qualität statt als PNG gespeichert, sodass jede Datei um ein Vielfaches kleiner ist. Wenn Sie das Format nie geändert haben, erhalten Sie automatisch JPEG; PNG ist weiterhin unter Einstellungen › Aufnahme verfügbar.
- Galerie und Overlay folgen Ihren Aufnahmeordnern live. Eine im Explorer gelöschte Aufnahme verschwindet sofort, und wenn sie aus dem Papierkorb wiederhergestellt wird, kehrt sie mit Favoritenmarkierung und Verlauf zurück. Neue Dateien, die in einen Aufnahmeordner kopiert werden, erscheinen ohne erneutes Durchsuchen. Eine geöffnete Ansicht bleibt bei der angezeigten Aufnahme, und eine Löschbestätigung löscht immer die Aufnahme, die sie genannt hat.
- Einstellungen › Benachrichtigungen & Sound bietet eigene Schalter für die Hinweise Aufnahmeanfrage empfangen, Steam-Input-Konflikt und Controller ausgeblendet.
- Wenn Sie die Maus verwenden, zeigen Benachrichtigungen ein X, mit dem Sie sie vorzeitig schließen können, und solange der Mauszeiger darauf ruht, bleiben sie auf dem Bildschirm. Beim reinen Spielen mit dem Controller ändert sich nichts, und in einem Spiel, das den Mauszeiger ausblendet, erscheint das X nie.
- Warnungen bleiben 10 Sekunden lang auf dem Bildschirm und tragen ein Warnsymbol.
- Die neuen Texte zu Teilen, Oberfläche, Controller und Benachrichtigungen wurden in allen unterstützten Oberflächensprachen überprüft.
- Die Vollbildansicht im Hauptfenster verwendet nur noch halb so breite Ränder, sodass Aufnahmen größer erscheinen.
- Die Hinweise in der Vollbildansicht wechseln zu Pfeiltasten und Esc, sobald Sie Tastatur oder Maus verwenden.
- Die Sprachauswahl, GameHQ unterstützen und die Gruppe Werkzeuge in der Hauptseitenleiste sind mit dem Controller erreichbar. In der eingeklappten Seitenleiste bleibt die Sprachauswahl als Symbol verfügbar.
- Aufnahmemenüs, Teilen und Overlay-Optionen haben im Overlay jetzt Vorrang vor der Videowiedergabe, sodass Kreuz den Menüeintrag auswählt, statt den Clip dahinter abzuspielen.

## Fehlerbehebungen

- DualSense über Bluetooth: Sticks und Tasten werden in GameHQ korrekt gelesen. Bluetooth-Berichte wurden um ein Byte versetzt gelesen, sodass Stickbewegungen nach oben/unten als links/rechts erkannt und Tasten falsch gelesen wurden. USB war nicht betroffen.
- L3 und R3 (Stick-Klicks) funktionieren jetzt bei DualSense- und DualShock-4-Controllern, die GameHQ direkt liest.
- Wenn Sie einen Controller bei laufendem GameHQ zwischen USB und Bluetooth wechseln, bleibt er danach nicht mehr ohne Reaktion.
- Das Trennen eines Controllers während einer Aufnahme im Spiel bringt GameHQ nicht mehr zum Absturz.
- Kurzes Antippen wird bei Controllern, die den Windows-Joystick-Fallback verwenden, nicht mehr verschluckt.
- Das Schließen des Overlays hängt nicht mehr, wenn eine Taste bereits vor dem Öffnen gedrückt gehalten wurde.
- Einstellungen › Eingabe › Zuordnungsvoreinstellungen: Die Liste Belegungen zeigt und bearbeitet jetzt die unter Bearbeitete Voreinstellung gewählte Voreinstellung. Bisher ließen sich Voreinstellungen, die von einem Spiel verwendet werden oder diesem Controller nicht zugewiesen sind, nicht bearbeiten, und eigene Zuordnungen wie ein Doppeltippen ließen sich weder entfernen noch zurücksetzen.
- Mit Windows starten: Eine andere GameHQ-Kopie (ein portabler Build oder eine Kopie mit eigenen Einstellungen) entfernt den Autostart-Eintrag der installierten Kopie nicht mehr und übernimmt ihn auch nicht mehr. Das konnte verhindern, dass das installierte GameHQ nach einem Neustart startete.

## Controller und Kompatibilität

- Sony-Controller werden wie bisher über den Standard-Eingabepfad von Windows gelesen.
- Ein von HidHide ausgeblendeter DualSense oder DualShock 4 wird über USB oder Bluetooth erkannt und gemeldet. Sobald GameHQ in HidHide zugelassen ist (Automatisch beheben erledigt das), liest GameHQ den Controller direkt, genauso wie DSX, während er für Spiele ausgeblendet bleibt.
- Wenn ein Treiber zum Ausblenden von Controllern (HidHide, installiert mit DSX, DS4Windows oder reWASD) Ihren Controller vor GameHQ verbirgt, erklärt eine Leiste am oberen Rand des Hauptfensters die Ursache und bietet Automatisch beheben, Einstellungen und Nicht jetzt an.
- GameHQ installiert oder entfernt HidHide nicht und ändert nicht, welche Controller es ausblendet. Wenn Sie Automatisch beheben wählen, fügt GameHQ sich lediglich zu den zugelassenen Anwendungen von HidHide hinzu.
- Hinweise zu Steam Input: Wenn ein Steam-Spiel läuft und eine GameHQ-Verknüpfung Create / Share oder PS verwendet, erklärt GameHQ, dass Steam Input diese Taste möglicherweise ebenfalls an das Spiel sendet, und bietet Schaltflächen an, die das Steam-Controller-Layout des Spiels oder die Controllereinstellungen von Steam öffnen. GameHQ liest oder ändert die Einstellungen von Steam nie. Die Hinweise lassen sich ausschalten oder pro Spiel ausblenden.

## Bekannte Einschränkungen

- Ob das Overlay Controllereingaben vom Spiel fernhält, hängt vom Spiel und davon ab, wie es den Controller liest. Ein kabelgebundener DualSense mit GameInput ist gut getestet; für XInput, Raw Input, direktes HID, Steam Input oder virtuelle Controller ist das nicht garantiert.
- Bei Telegram Desktop und Discord öffnet GameHQ die App, und Sie schließen den Versand dort ab; GameHQ kann nicht erkennen, ob Sie die Aufnahme tatsächlich gesendet haben, und meldet für diese Apps daher nie Gesendet. Bei Discord fügen Sie die Aufnahme selbst in einen Chat ein.
- Uploads in Discord-Kanäle sind durch die Dateigrößenbeschränkung des Servers begrenzt.
- Manche Spiele pausieren oder reagieren, wenn sie den Fokus verlieren, zum Beispiel wenn Telegram oder Discord nach dem Teilen in den Vordergrund kommt. Windows oder eine andere Aufnahme-App kann den gelben Aufnahmerahmen weiterhin anzeigen.
