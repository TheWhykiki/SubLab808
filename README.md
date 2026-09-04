# SubLab808

Eigenständiger, nativer 808-Bass-Synth für Apple Silicon und Cubase (VST3), Version 1.3.1.
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

Deep Foundation, Modern Knock, Long Slide, Dirty Trunk, Short Punch, Soft Pillow, Upper Bass und Sub Destroyer sind direkt über Cubase oder die Preset-Auswahl im Plugin erreichbar.
