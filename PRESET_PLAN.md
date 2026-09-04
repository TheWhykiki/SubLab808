# SubLab808 Preset-Ausbau – Version 1.4.0

## Ziel und Umfang

SubLab808 bekommt 64 eigene Factory-Presets und eine dauerhafte Verwaltung eigener Presets.
Die andere Plugin-Bank wird unabhängig entwickelt. Gemeinsam sind Dateiverwaltung und Bedienung.
Die bestehenden 8 Factory-Presets behalten Namen, Werte und Host-Programmnummern.
Neue Kategorien: Deep & Clean, Tight & Punchy, Long & Glide, Warm & Rounded, Driven & Grit, Melodic & Gate, FX & Percussion. Eine eigene Kategorie Classics enthält die bisherigen Presets.

Die vollständigen Rezepte stehen in `Presets/FactoryPresets.json`; `Presets/CATALOG.md` beschreibt die Anwendungen.
Der Generator `scripts/generate-presets.py` erzeugt daraus `Source/FactoryBank.h`.
Alle Presets enthalten sämtliche Parameter; beim Wechsel dürfen keine Werte des vorherigen Sounds übrig bleiben.
Jedes Rezept ist explizit ausgearbeitet. Presets unterscheiden sich in Klangparametern, nicht nur im Namen oder Output.

## Bedienung

- Die Preset-Leiste zeigt Factory/User, Namen und ein Sternchen bei ungespeicherten Änderungen.
- Der Browser bietet Suche, Factory/User/Favourites sowie Kategorien und Anwendungshinweise.
- Vor/Zurück folgt den zuletzt gewählten Browser-Filtern.
- Save überschreibt das geladene eigene Preset. Bei Factory öffnet es Save As.
- Save As speichert den aktuellen Klang unter einem neuen Namen und einer Kategorie.
- More enthält Rename, Delete, Import und Export current sound.
- Beim Laden über den Browser mit ungespeicherten Änderungen: Save As, Discard changes oder Cancel.
- Factory-Presets können weder überschrieben, umbenannt noch gelöscht werden.
- Delete fragt nach und entfernt nur die gespeicherte Datei; der aktuelle Klang bleibt im Projekt.
- Offene Save-As-, Export- und Wechsel-Dialoge erkennen geänderte DAW-Klänge. Eine veraltete Aktion wird mit Erklärung abgebrochen; der aktuelle Klang bleibt erhalten.
- Versteckt, entfernt oder zerstört der Host den Editor, werden dessen eigene Dialoge geschlossen und bereits vorgemerkte Antworten ungültig. Andere Plugin-Instanzen bleiben unberührt. Schließt der Host den Editor synchron während einer bestätigten Preset-Aktion, darf diese fertig werden, aber keine veraltete UI- oder Folgeaktion mehr ausführen.
- Lange Preset-Namen erhalten bei Save As einen gültigen Vorschlag innerhalb der 80-Zeichen-Grenze.

## Speicherung und Kompatibilität

Eigene Presets: JUCE userApplicationDataDirectory / Whykiki Audio / SubLab808 / Presets.
Auf macOS ist dies `~/Library/Whykiki Audio/SubLab808/Presets`.
Dateiendung: `.sublab808preset`. JSON enthält Formatkennung, Version 1, Pluginkennung,
eine zufällige stabile ID, Name, Kategorie, Beschreibung und alle Parameterwerte.
Der Dateiname basiert auf der ID, nicht auf Benutzereingaben. Favoriten liegen separat unter Favourites.

Dateien werden über eine temporäre Datei ersetzt. Gleichzeitige Schreibvorgänge werden gesperrt;
ein veraltetes Preset in einer zweiten Instanz darf neu gespeicherte Klangänderungen nicht unbemerkt überschreiben.
Import validiert Produkt, Version, Namen, Vollständigkeit, Zahlentyp und Parametergrenzen, bevor etwas gespeichert wird.
Import legt eine neue ID an und löst Namenskonflikte mit einem Suffix.
Laden, Speichern und Umbenennen prüfen zusätzlich, ob die Datei-ID zum Bibliothekseintrag passt.
Exportziele innerhalb des verwalteten Preset-Ordners werden auch über Verzeichnis-Symlinks erkannt und abgelehnt; Bibliothekseinträge werden über Save/Save As gespeichert. So bleiben Konfliktprüfung und Identitäten wirksam.
Fehler zeigen eine verständliche Meldung und dürfen keine bestehenden Presets beschädigen.

Der DAW-State enthält weiterhin alle aktuellen Parameter und Fenstermaße. Für eigene Presets enthält er zusätzlich
Name, ID und den gespeicherten Klang als Vergleichsbasis. Ungespeicherte Änderungen und die Dirty-Anzeige
bleiben deshalb nach dem Öffnen erhalten, auch wenn die externe Preset-Datei fehlt.
Ein Preset-Wechsel übernimmt Klangparameter und behält die aktuellen Fenstermaße.
Dateioperationen finden ausschließlich in expliziten UI-/Control-Aktionen statt; Favoriten werden im UI gelesen.
Audio-Callbacks führen keine Preset-Dateizugriffe aus.

## Prüfungen und Abnahme

1. Vorhandene DSP-, Host- und State-Tests bleiben erfolgreich.
2. Alle 64 Rezepte sind vollständig, eindeutig und innerhalb der Parametergrenzen.
3. Save, Save As, Rename, Delete, Import, Export, Favoriten und Instanz-Neustart funktionieren.
4. DAW-State stellt gespeicherte und ungespeicherte Klangzustände einschließlich Namen korrekt wieder her.
5. Beschädigte/fremde Dateien, fehlende Parameter, Namenskonflikte, veraltete Instanzen und Schreibfehler werden geprüft.
6. Jedes Preset wird in drei Szenarien gerendert; Samples müssen endlich, hörbar und innerhalb der Test-Pegelgrenze sein.
7. Die Oberfläche wird in minimaler, normaler und maximaler Größe gerendert und visuell geprüft.
8. Editor-Lebensdauer, wartende Dialogantworten und Instanz-Isolation werden mit echten JUCE-Fenstern geprüft; synchrones Schließen durch einen Host-Listener wird separat getestet. Unveränderte Live-Werte und korrekt gespeicherte Werte werden jeweils gegen ihre eigene Darstellung geprüft, nicht durch pauschale Float-Toleranzen gleichgesetzt.

Automatische Audio-Tests prüfen technische Plausibilität und unterscheiden identische Ausgaben.
Sie ersetzen keine musikalische Hörabnahme im Arrangement. Die Bank bleibt nach persönlichem Feedback nachjustierbar.

## Reproduzieren

```sh
python3 scripts/generate-presets.py --check
cmake --build build-presets --target SubLab808PresetTests -j 4
ctest --test-dir build-presets --output-on-failure
```

Die eigenständige Test-App nimmt optional einen absoluten Ausgabeordner an und schreibt dorthin
`preset-audio-report.csv` sowie Editor-Vorschauen. Tests verwenden einen temporären Preset-Ordner.
