# GameHQ 0.8.0 (2026-10-26)

## Specjalne podziękowania

- Wielkie podziękowania dla [@galcn10](https://github.com/galcn10), twórcy [Console Overlay for Playnite](https://github.com/galcn10/ConsoleOverlay), za testowanie GameHQ, cenne uwagi i stworzenie świetnej integracji z Playnite. Twoje wsparcie pomogło ulepszyć GameHQ 0.8.0!

## Najważniejsze zmiany

- Klipy mogą nagrywać dźwięk samej gry. Dzięki nowemu źródłu Dźwięk gry GameHQ nagrywa grę niezależnie od urządzenia, na którym gra odtwarza dźwięk (słuchawki, Bluetooth, HDMI lub głośniki), i pomija inne aplikacje, takie jak Discord czy Spotify.
- Problemy z nagrywaniem dźwięku są teraz wykrywane i zgłaszane. Jeśli wybranego źródła dźwięku nie da się nagrać, bufor powtórek zatrzymuje się i informuje o przyczynie oraz o tym, które źródło wybrać zamiast niego, zamiast po cichu zapisywać klipy bez dźwięku. GameHQ nigdy samodzielnie nie zmienia źródła dźwięku.
- Dźwięk i obraz pozostają zsynchronizowane. Klipy zapisane po ekranie ładowania, pauzie lub długim przestoju zachowują prawidłową synchronizację czasową, a bufor powtórek sam uruchamia się ponownie, gdy gra zmienia rozdzielczość.
- Zrzuty ekranu HDR już się nie zawieszają. Zrzut ekranu wstrzymanej gry lub statycznego menu w HDR jest zapisywany od razu. Jeśli nie nadejdzie żadna klatka, próba po kilku sekundach bezpiecznie kończy się niepowodzeniem, a kolejne naciśnięcie działa.
- Bezpieczniejsze usuwanie kontrolerem. Krzyżyk w potwierdzeniu usunięcia naciska teraz podświetlony przycisk, a Kółko anuluje. Wcześniej Krzyżyk mógł usunąć plik nawet wtedy, gdy podświetlony był przycisk Anuluj.

## Nowości

- W sekcji Ustawienia › Powtórki › Źródło dźwięku dostępne są opcje Dźwięk gry, Dźwięk pulpitu i Wyłączone. Dźwięk gry jest domyślny w Windows 11. Dźwięk pulpitu nagrywa wszystko, co trafia na domyślne wyjście Windows, w tym dźwięk innych aplikacji, i jest domyślny w starszych wersjach Windows.
- Dźwięk gry korzysta z wbudowanego w Windows przechwytywania dźwięku poszczególnych aplikacji i nigdy nie ingeruje w proces gry. Jeśli we wcześniejszej wersji dźwięk systemowy został celowo wyłączony, pozostanie wyłączony.
- Opcjonalne aktualizacje beta: nowy przełącznik Uwzględniaj wersje beta w sekcji Ustawienia › Ogólne oraz w sekcji Info pozwala aktualizatorowi proponować wersje beta. Jest domyślnie wyłączony, więc stabilne instalacje otrzymują tylko stabilne aktualizacje.
- Gdy GameHQ ulegnie awarii, próbuje zapisać niewielki zrzut awaryjny w swoim folderze dzienników, aby pomóc zdiagnozować problem. Nic nie jest wysyłane. Niektóre awarie, na przykład takie, w których Windows natychmiast kończy program, nadal mogą zamknąć GameHQ bez zapisania zrzutu.
- Dziennik zawiera więcej szczegółów o zapisanych zrzutach ekranu i błędach uruchamiania, dzięki czemu pojedynczy plik dziennika częściej wystarcza do zrozumienia zgłoszonego problemu.

## Poprawki i ulepszenia

- Klipy nie są już nagrywane bez dźwięku z powodu starego wewnętrznego ustawienia, które domyślnie wyłączało dźwięk pulpitu.
- Dźwięk w dowolnym formacie odtwarzania Windows jest poprawnie konwertowany, a ścieżka dźwiękowa nie ginie.
- Ciche fragmenty są nagrywane jako cisza, więc dźwięk po dłuższej ciszy pozostaje zsynchronizowany z obrazem.
- Nagrywanie dźwięku gry niezawodniej wznawia się po ponownym uruchomieniu gry lub przełączeniu na inną grę.
- Fragmenty bufora powtórek, których nie udało się zapisać lub nie da się ponownie odczytać, są pomijane, więc nie mogą uszkodzić zapisanego klipu.
- Własne powiadomienia GameHQ, takie jak informacja o zapisaniu zrzutu ekranu, nie pojawiają się już na zrzutach ekranu ani w nagraniach i transmisjach całego ekranu.
- Zrzut ekranu zastąpiony innym obrazem o tej samej nazwie pliku otrzymuje nową miniaturę przy następnym skanowaniu biblioteki.
- Foldery z materiałami są skanowane w tle, więc GameHQ reaguje płynnie nawet przy dużych kolekcjach.
- Przeglądarki, edytory i inne okna, które nie są grami, nie pojawiają się już jako gry w bibliotece.
- Podczas usuwania wielu plików zaznaczenie pozostaje niezmienione, dopóki potwierdzenie jest otwarte, więc działania kontrolera w tle nie mogą zmienić tego, które pliki zostaną usunięte.
- Okno usuwania w nakładce przejmuje sygnały kontrolera, dopóki jest otwarte, i zamyka się, gdy nakładka zostanie ukryta.
- Nakładka pomija okna, które przestały odpowiadać, zamiast na nie czekać.
- Ustawienia nie przeskakują już na górę, gdy przewijasz je myszą po powrocie do GameHQ.
- Wybranie opcji Wbudowane ustawienia domyślne lub usunięcie presetu w edytorze mapowań kontrolera nie blokuje już wyboru presetu.

## Kontrolery i Playnite

- Skróty kontrolera nadal działają w grach uruchamianych poza Steam, takich jak lokalna gra uruchomiona z Playnite. Jeśli Windows przestaje przekazywać do GameHQ podłączony DualSense, gdy gra jest na pierwszym planie, GameHQ odczytuje kontroler bezpośrednio, w trybie tylko do odczytu, i wraca do zwykłej obsługi sygnałów, gdy Windows znów zacznie go przekazywać.
- Skróty i powtórzenia przytrzymanego przycisku zawsze kończą się po zwolnieniu tego samego naciśnięcia, także gdy zwolnienie zgłasza GameInput lub gdy kontroler zostanie odłączony w trakcie przytrzymywania przycisku.
- Dane kontrolerów odłączonych na stałe są czyszczone, sprawdzanie HidHide nie spowalnia już interfejsu, a urządzenia GameInput są sprawdzane ponownie tylko wtedy, gdy coś się zmieni.
- Gry z Playnite pozostają rozpoznawane, nawet gdy Playnite nie potrafi zidentyfikować procesu gry.
- Pełnoekranowe środowisko Xbox w Windows: gdy powłoka Xbox jest na pierwszym planie, GameHQ używa gry uruchomionej z Playnite do zrzutów ekranu i klipów. GameHQ Playnite Integration 0.4.13, dostępna osobno jako wersja przedpremierowa, uruchamia też GameHQ, gdy tryb Xbox opóźnia jego start po zalogowaniu.

## Stabilność i niezawodność

- Czyszczenie niedokończonych nagrań usuwa tylko pliki tymczasowe utworzone przez sam GameHQ.
- Foldery materiałów poszczególnych gier zawsze pozostają wewnątrz twojego folderu materiałów.
- Uruchamianie nie kończy się już błędem, gdy biblioteka zawiera zduplikowane ścieżki folderów; zamiast tego wpisy są scalane.
- Jeśli GameHQ nie może się uruchomić, wyjaśnia przyczynę w twoim języku i zapisuje ją w dzienniku.
- Import profilu przenośnego do zainstalowanego GameHQ działa teraz także wtedy, gdy zainstalowana kopia była już uruchamiana.
- Wzmocnione zabezpieczenia: bezpieczniejsze ładowanie bibliotek DLL, ostrzejsze sprawdzanie lokalnych połączeń integracji i bezpieczniejsza obsługa plików aktualizacji.
- Aktualizator lepiej radzi sobie z powolnym pierwszym uruchomieniem i zachowuje swoje zapisy w nienaruszonym stanie, jeśli zostanie przerwany.
- Udostępnianie przez Telegram jest bezpieczniej zamykane.

## Instalacja i aktualizacja

- Aktualizacja z wersji 0.7.9: wbudowany aktualizator proponuje wersję 0.8.0; możesz też uruchomić instalator na obecnej instalacji. Ustawienia, biblioteka i materiały zostają zachowane.
- Instalator: instaluje program dla bieżącego użytkownika i zastępuje zainstalowaną wersję 0.7.9 lub wcześniejszą betę.
- Wersja przenośna: rozpakuj archiwum ZIP do nowego folderu i uruchom GameHQ.exe.
- Po testach wersji 0.8.0 Beta 2, 3 lub 4 zainstaluj 0.8.0 za pomocą instalatora lub nowego archiwum ZIP wersji przenośnej, ponieważ te bety nie były oferowane przez aktualizator.

## Znane ograniczenia

- Dźwięk gry wymaga systemu Windows 11. W starszych wersjach Windows GameHQ używa źródła Dźwięk pulpitu, które nagrywa też inne aplikacje odtwarzające dźwięk na domyślnym wyjściu.
- Gra, która przejmuje wyłączną kontrolę nad kontrolerem, nadal może blokować skróty działające w tle.
- Obsługa pełnoekranowego środowiska Xbox w Windows nie została jeszcze potwierdzona przez testerów na komputerze, na którym jest ono używane.
- Pliki binarne dla Windows nie są podpisane Authenticode; Windows SmartScreen może wyświetlić ostrzeżenie Nieznany wydawca.
