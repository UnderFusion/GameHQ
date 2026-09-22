# GameHQ 0.7.8 (2026-09-22)

## Najważniejsze zmiany

- Nakładka działa teraz znacznie niezawodniej nad grami w oknie bez ramki. Wyświetla się nad grą, na obsługiwanych ścieżkach GameInput przejmuje sygnały kontrolera do nawigacji po nakładce, a gra pozostaje widoczna. Potwierdzono z przewodowym kontrolerem DualSense w grze Indiana Jones and the Great Circle w trybie okna bez ramki (Borderless).
- Na obsługiwanych ścieżkach GameInput nawigacja po nakładce nie steruje już grą pod spodem. Po zamknięciu nakładki GameHQ czeka, aż puścisz przytrzymane przyciski, i dopiero wtedy oddaje kontroler grze.
- Klipy powtórek są bezpieczniejsze: szybkie, kolejne zapisy nigdy nie nadpisują istniejącego klipu, a klip możesz zapisać zaraz po rozpoczęciu nagrywania.
- Nowe presety mapowań: twórz i edytuj układy kontrolera oraz przypisuj je do poszczególnych kontrolerów i gier.
- GameHQ zapamiętuje ostatni widok: ostatnią stronę, kategorię Ustawień, filtr galerii i kategorię nakładki dla każdej gry.

## Nakładka

- Nakładka może przejąć fokus kontrolera, pozostając nad widoczną grą w oknie bez ramki, bez jej minimalizowania.
- Na obsługiwanych ścieżkach GameInput nawigacja po nakładce nie steruje już jednocześnie grą.
- Po zamknięciu nakładki GameHQ chwilę czeka, aż przytrzymane przyciski zostaną puszczone, a gałki wrócą do pozycji spoczynkowej, i dopiero wtedy oddaje kontroler grze. Dzięki temu przytrzymany sygnał nie przenosi się do gry.
- Otwieranie i zamykanie nakładki jest szybsze i bardziej niezawodne, a szybkie ponowne naciśnięcie nie zamyka jej już zaraz po otwarciu. Alt-Tab i inne celowe przełączenia do innej aplikacji są respektowane.
- Nakładka śledzi właściwą grę, gdy gra tworzy swoje okno od nowa, traci je lub na chwilę traci fokus. Gdy na pierwszy plan przejdzie inna aplikacja, nakładka zamyka się bez problemów.
- Nakładka otwiera się na ostatnio wybranym zrzucie ekranu lub klipie. Po nowym przechwyceniu zaczyna od najnowszego elementu.
- Klipy zaczynają się odtwarzać w nakładce bez chwilowego znikania podglądu.
- Przyciemnione tło pojawia się teraz razem z menu nakładki, a nie z opóźnieniem po nich.
- Podpowiedzi sterowania na dole ekranu mają teraz niewielkie tło zgodne z ustawieniem przyciemniania nakładki, dzięki czemu pozostają czytelne na tle gry.
- Gra, w którą właśnie grasz, jest teraz poprawnie wyróżniona podczas przechodzenia po pasku bocznym.
- Nawigacja kontrolerem działa lepiej w menu nakładki, w galerii i podczas odtwarzania filmów, a przyciski nawigacji są oddzielone od skrótów przechwytywania.

## Przechwytywanie i powtórki

- Szybkie, kolejne zapisy otrzymują unikalne nazwy plików zamiast nadpisywać istniejący klip. Nieudany eksport nigdy nie usuwa wcześniejszych klipów, a miniatury zawsze pasują do właściwego klipu.
- Zapis powtórki wykorzystuje materiał nagrany do tej pory, nawet zanim bufor wypełni się do ustawionej długości.
- Eksport powtórki zachowuje potrzebny materiał, gdy przełączasz gry lub bufor uruchamia się ponownie, a GameHQ przed zamknięciem czeka na zakończenie trwającego eksportu.
- Zrzuty ekranu i zapisy powtórek są potwierdzane natychmiast, a potem wyraźnie pokazują, czy zapis się powiódł, czy nie. Powiadomienia aktualizują się w miejscu, a jednocześnie wyświetla się tylko ograniczona ich liczba.
- Nieudane lub pominięte przechwycenia wyjaśniają teraz przyczynę, nawet gdy powiadomienia o powodzeniu są wyłączone.
- Stan powtórek pokazuje teraz, czy nagrywanie naprawdę się rozpoczęło i czy jest dostępny materiał do zapisania, z czytelnymi komunikatami o uruchamianiu bufora, pustym buforze i trwającym eksporcie.
- Ręczne sesje powtórek przetrwają teraz zmiany ustawień i jednoczesne wykonywanie zrzutów ekranu HDR. Ręczna sesja, którą uruchomisz, ale z niej nie skorzystasz, po pewnym czasie się wyłącza.
- Zapisywanie powtórki nie wstrzymuje już przechwytywania podczas tworzenia miniatury, a zrzuty ekranu wykonane w tym samym czasie niezawodniej tworzą swój folder.
- Nowa opcja dotycząca żółtej ramki przechwytywania systemu Windows, z czytelniejszymi informacjami o uprawnieniach i obsłudze przez system. Nagrywanie działa nadal, nawet gdy Windows nie może ukryć ramki.

## Kontrolery i sterowanie

- Niezawodniejsze wykrywanie kontrolerów i kierowanie sygnałów wejściowych, gdy podłączonych jest kilka kontrolerów lub źródeł sygnału.
- Przełączanie źródeł sygnału, ponowne podłączanie kontrolerów i śledzenie przytrzymanych przycisków nie powodują już pominiętych lub zdublowanych naciśnięć ani zablokowanego sterowania.
- Naprawiono pomijanie naciśnięć przycisku PS oraz opóźnione powtórzenia naciśnięć, które mogły ponownie otworzyć nakładkę zaraz po jej zamknięciu.
- Spusty, wciśnięcia gałek i inne przyciski działają teraz tak samo we wszystkich obsługiwanych źródłach sygnału, dzięki czemu przypisania zachowują się bardziej przewidywalnie.
- Kontrolery korzystające z GameInput nadal działają normalnie, łącznie z izolacją nakładki, gdy Windows nie zapewnia opcjonalnej obsługi przycisków Guide/Share.
- Kontroler edytowany w Ustawieniach pozostaje wybrany, gdy aktywny stanie się inny kontroler.

## Presety mapowań

- Nowa biblioteka presetów układów kontrolera, w której możesz tworzyć, zmieniać nazwy, duplikować, edytować i usuwać presety.
- Presety można przypisywać do kontrolerów i gier, z wyborem presetu zapasowego i automatycznym wyborem dla uruchomionej gry.
- Twoje dotychczasowe własne przypisania automatycznie trafiają do systemu presetów, a oryginalne dane są zachowywane jako kopia zapasowa.
- Przełączanie presetów uwzględnia przytrzymane przyciski i trwające gesty, dzięki czemu zmiana presetu nie wywołuje przypadkowo żadnych akcji.
- Interfejs oddziela przypisany preset od edytowanego, chroni niezapisane zmiany i pozwala utworzyć kopię dla jednego kontrolera.
- Przed usunięciem używanego presetu musisz wybrać zamiennik lub preset zapasowy, a zmiany we współdzielonych presetach są wyraźnie oznaczone.

## Interfejs, ustawienia i dźwięk

- GameHQ otwiera się na ostatnio używanej stronie, kategorii Ustawień i filtrze galerii. Nakładka zapamiętuje ostatnią kategorię osobno dla każdej gry.
- Okno jest teraz poprawnie przywracane na monitorach położonych na lewo od ekranu głównego lub nad nim. Okno, które otworzyłoby się całkowicie poza ekranem, zostaje przeniesione na podłączony wyświetlacz.
- Okno główne i nakładka mają osobne skalowanie, od 100% do 200%, zapamiętywane po ponownym uruchomieniu. Małe okna mają lepszy układ.
- Dźwięki przechwytywania są głośniejsze i bardziej wyraziste, mają też własną regulację głośności i podgląd. Głośność interfejsu i przechwytywania można ustawić nawet na 300%.
- Zmiana ustawień powiadomień, dźwięków, ramki przechwytywania lub sesji ręcznych nie kasuje już zawartości bufora powtórek.
- Galeria po otwarciu zachowuje zapisany filtr. Zapisany filtr gry, która już nie istnieje, bezpiecznie wraca do ustawienia domyślnego. Kategorie Ustawień pozostają na miejscu, nawet gdy zmieni się ich kolejność.
- Naprawiono puste treści i problemy z nakładaniem się elementów przy zmianie skali interfejsu, a menu wyświetlają się teraz nad główną zawartością.
- Ustawienia przypisań pokazują, jak długo trwa przytrzymanie, i wyjaśniają, jak działają gesty przytrzymania.

## Aktualizacje, języki i diagnostyka

- Informacje o aktualizacji są wyświetlane w wybranym języku, a gdy tłumaczenie jest niedostępne — po angielsku. Po wczytaniu pozostają dostępne także offline.
- Informacje o aktualizacji mogą zawierać opcjonalny link do wydania w serwisie GitHub, do którego przejdziesz także kontrolerem.
- Rozszerzono tłumaczenia we wszystkich obsługiwanych językach o nowe komunikaty dotyczące przechwytywania, dźwięków, ramki, przytrzymania, presetów i fokusu.
- Skopiowane dane diagnostyczne zawierają teraz aktywne przypisania, wybór presetu, zmiany źródła sygnału i stan fokusu nakładki, a identyfikatory urządzeń są anonimizowane.
- Diagnostyka przechwytywania śledzi każde żądanie od naciśnięcia przycisku do zapisanego pliku, co ułatwia znajdowanie problemów. Problemy z dźwiękiem są zgłaszane czytelnym ostrzeżeniem.

## Znane ograniczenia

- To, czy nakładka odizoluje kontroler od gry, zależy od gry i sposobu, w jaki odczytuje ona kontroler. Przewodowy DualSense korzystający z GameInput jest dobrze przetestowany. Izolacja nie jest gwarantowana dla XInput, Raw Input, bezpośredniego HID, Steam Input ani kontrolerów wirtualnych.
- Przełączanie kontrolerów przy uruchomionym DSX zostało zweryfikowane tylko częściowo. Konfiguracja DSX pozostaje pod twoją kontrolą; GameHQ nie instaluje sterowników kontrolerów wirtualnych ani sterowników ukrywających urządzenia i nie zarządza nimi.
- Niektóre gry wstrzymują się lub reagują w inny sposób po utracie fokusu. Windows lub inna aplikacja do przechwytywania może pozostawić widoczną żółtą ramkę nagrywania.
- Jeśli informacje o wydaniu 0.7.8 nie są dostępne w twoim języku, GameHQ wyświetla je po angielsku.
