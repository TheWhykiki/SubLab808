# SubLab808

Eigenständiger, nativer 808-Bass-Synth für Apple Silicon und Cubase (VST3), Entwicklungsstand 1.4.0.
Die Klangerzeugung ist neu implementiert und verwendet keine Binärdaten oder Samples von 808 Lab.

## Regler

- Decay: Ausklingzeit
- Release: Loslasszeit im Gate-Modus
- Pitch Punch: anfänglicher Pitch-Abfall in Halbtönen
- Pitch Decay: Dauer des Pitch-Abfalls
- Glide: Portamento zwischen MIDI-Noten; wird eine neue Note gespielt, während die vorige gehalten ist, gleitet die Tonhöhe ohne Neuauslösung (Legato)
- Tune: Stimmung in Halbtönen
- Body: zweite Harmonische für mehr Durchsetzung
- Click: kurzer Attack-Transient
- Drive: Sättigung
- Tone: Tiefpass
- Output: Ausgangspegel
- Velocity: Stärke der Anschlagdynamik
- One Shot: vollständiges Abspielen oder Gate-Steuerung über Note-Off

Pitch Bend wird mit einem Bereich von ±2 Halbtönen unterstützt.

Die Oberfläche ist skalierbar und enthält eine Ausgangspegelanzeige. Parameterzustände werden im Cubase-Projekt gespeichert.

## Render- und Click-Verhalten

Der Click verwendet eine feste pseudozufällige Sequenz. Ein vollständiger Soundreset
(`prepareToPlay`) beginnt diese Sequenz wieder am selben Seed. Auch ein erfolgreicher
Projekt-/State-Restore startet sie im nächsten konsistenten Audio-Block neu, ohne eine
noch klingende Stimme zusätzlich abzubrechen. Normale Note-Ons, Factory-Presetwechsel und
MIDI All Sound Off lassen die Sequenz innerhalb einer Performance weiterlaufen;
aufeinanderfolgende Anschläge dürfen daher unterschiedliche Click-Transienten haben.
Das Laden eines User-Presets über State-Restore beginnt die Click-Sequenz dagegen neu.

Identische MIDI-Daten, Parameter und Samplerate ergeben nach einem vollständigen
Render-Neustart dieselben Samples, auch bei Click > 0. Ein State speichert Parameter,
nicht die aktuelle Position einer klingenden Stimme oder die laufende Zufallsfolge.
Ein Host, der einen Bounce ohne Soundreset oder State-Restore erneut startet, kann
deshalb weiterhin einen anderen Click-Verlauf liefern.

Decay und Release werden auch dann geglättet, wenn gerade die jeweils andere
Envelope-Phase aktiv ist. Änderungen sind nach der 20-ms-Glättung für den nächsten
Phasenwechsel bereit und beginnen nicht erst beim Note-Off oder Retrigger zu wirken.

## Factory Presets

64 eigene Factory-Presets sind über Cubase oder den Preset-Browser erreichbar. Die bisherigen acht Sounds bleiben mit ihren ursprünglichen Programmnummern und Klangeinstellungen erhalten.


## Eigene Presets speichern

Den Preset-Namen anklicken, um den Browser mit Suche, Kategorien und Favoriten zu öffnen.
**Save** speichert Änderungen an einem eigenen Preset; **Save As** legt eine neue Variante mit Namen und Kategorie an.
Unter **More** stehen Umbenennen, Löschen, Import und Export bereit. `*` kennzeichnet ungespeicherte Änderungen.

Eigene Dateien liegen auf macOS unter `~/Library/Whykiki Audio/SubLab808/Presets` und verwenden
`.sublab808preset`. Factory-Sounds bleiben geschützt. Der DAW-Projektzustand enthält zusätzlich den aktuellen
Klang einschließlich ungespeicherter Änderungen und des eigenen Preset-Namens.

Siehe [Preset-Katalog](Presets/CATALOG.md) und [Umsetzungs- und Abnahmeplan](PRESET_PLAN.md).
