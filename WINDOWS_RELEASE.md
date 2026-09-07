# Signierter Cross-Platform-Releasepfad

`.github/workflows/windows-release.yml` ist der gemeinsame Produktionspfad für
Windows und macOS. Das Einchecken dieser Datei veröffentlicht jetzt nichts: Der
Workflow besitzt ausschließlich `workflow_dispatch`, verlangt
`confirm_release=true` und akzeptiert nur einen bereits auf `origin` vorhandenen
Tag im Format `vMAJOR.MINOR.PATCH`. Der Tag muss exakt zur CMake-Projektversion
passen und ein Vorfahr des aktuellen Default-Branch-Standes sein. Der Dispatch
muss außerdem aus dem Default Branch des kanonischen Repositories
`TheWhykiki/SubLab808` stammen. Der aktuelle Workflow ist absichtlich nur für
den ersten signierten Windows-Release freigeschaltet und verlangt zusätzlich
`confirm_first_windows_release_bootstrap=true`.

Ein Release darf niemals nur Windows enthalten, weil auch der macOS-Updater
`/releases/latest` auswertet. Der Publish-Job benötigt deshalb sowohl beide
Windows-Jobs als auch den signierten und notarisierten macOS-Job und die
nachgelagerte native Intel-Abnahme genau dieses macOS-Kandidaten.

## Geschützte Konfiguration

Vor dem ersten echten Lauf müssen im GitHub-Repository diese Werte eingerichtet
werden:

| Typ | Name | Inhalt |
| --- | --- | --- |
| Secret | `WINDOWS_CODE_SIGNING_PFX_BASE64` | Base64-kodierte PFX-Datei mit genau einem privaten Code-Signing-Schlüssel |
| Secret | `WINDOWS_CODE_SIGNING_PFX_PASSWORD` | Passwort der PFX-Datei |
| Variable | `WINDOWS_CODE_SIGNING_CERT_SHA256` | Öffentlicher, 64-stelliger SHA-256-Fingerprint des aktuellen Windows-Leaf-Zertifikats |
| Variable | `WINDOWS_NEXT_CODE_SIGNING_CERT_SHA256` | Optionaler, vom aktuellen Pin verschiedener 64-stelliger SHA-256-Fingerprint des nächsten Windows-Leaf-Zertifikats |
| Variable | `WINDOWS_RFC3161_TIMESTAMP_URL` | Absolute HTTPS-URL des RFC-3161-Zeitstempeldienstes |
| Secret | `MACOS_DEVELOPER_ID_APPLICATION_P12_BASE64` | Base64-kodiertes Developer-ID-Application-Zertifikat samt privatem Schlüssel |
| Secret | `MACOS_DEVELOPER_ID_APPLICATION_P12_PASSWORD` | Passwort der Application-P12-Datei |
| Secret | `MACOS_DEVELOPER_ID_INSTALLER_P12_BASE64` | Base64-kodiertes Developer-ID-Installer-Zertifikat samt privatem Schlüssel |
| Secret | `MACOS_DEVELOPER_ID_INSTALLER_P12_PASSWORD` | Passwort der Installer-P12-Datei |
| Secret | `MACOS_NOTARY_PRIVATE_KEY_P8_BASE64` | Base64-kodierter App-Store-Connect-API-Schlüssel |
| Variable | `MACOS_DEVELOPER_ID_APPLICATION_IDENTITY` | Vollständige `Developer ID Application: …`-Identität |
| Variable | `MACOS_DEVELOPER_ID_INSTALLER_IDENTITY` | Vollständige `Developer ID Installer: …`-Identität |
| Variable | `MACOS_DEVELOPER_ID_APPLICATION_CERT_SHA256` | Öffentlicher SHA-256-Fingerprint des Application-Leaf-Zertifikats |
| Variable | `MACOS_DEVELOPER_ID_INSTALLER_CERT_SHA256` | Öffentlicher SHA-256-Fingerprint des Installer-Leaf-Zertifikats |
| Variable | `MACOS_NOTARY_KEY_ID` | Zehnstellige App-Store-Connect-Key-ID |
| Variable | `MACOS_NOTARY_ISSUER_ID` | App-Store-Connect-Issuer-UUID |

Die SHA-256-Pins sind absichtlich öffentliche Repository-Variablen. Der aktuelle
Windows-Pin wird vor dem Build exakt in Updater und Plug-in-Launcher kompiliert.
Nur der Updater erhält zusätzlich den optionalen nächsten Pin als eng begrenzte
Payload-Allowlist für einen Zertifikatswechsel; der Launcher und die
Selbstprüfung des Updaters akzeptieren weiterhin ausschließlich den aktuellen
Pin. Beide Pins müssen eindeutig und jeweils exakt 64 Hex-Zeichen lang sein.

Die Windows-PFX wird in einen zufälligen laufbezogenen `CurrentUser`-Store
importiert. Eine restriktive Datei-ACL gibt nur dem Runner-Benutzer Zugriff. Der
Workflow verlangt genau ein gültiges privates Leaf-Zertifikat mit
Code-Signing-EKU und vergleicht dessen SHA-256-Fingerprint bytegenau mit der
Variable. Ein `if: always()`-Schritt entfernt PFX und genau diesen Store; der Job
scheitert, falls der Store danach noch existiert.

Auf macOS liegen beide P12-Dateien und der Notary-P8-Schlüssel ausschließlich in
einem zufälligen Verzeichnis mit Modus 0700. Beide Zertifikate werden in einen
eigenen temporären Keychain importiert und gegen die öffentlichen Pins geprüft.
Das Notary-Profil wird in genau diesem Keychain gespeichert. Dessen absoluter
Pfad wird dem Packaging-Skript explizit übergeben und von `notarytool submit`
und `notarytool log` verwendet; es gibt keinen stillen Rückfall auf den
Data-Protection-Keychain. Sobald das validierte Profil gespeichert ist, löscht
der Workflow beide P12-Dateien und den P8-Schlüssel und entfernt alle fünf
Credential-Variablen aus der Umgebung, bevor Build oder Tests starten. Ein Trap stellt Default-/Such-Keychains wieder her und
löscht Keychain sowie Credential-Verzeichnis bei Erfolg und Fehler.

## Build- und Assetvertrag

Windows baut und testet in getrennten nativen Jobs:

- x64 auf `windows-2022` mit Visual Studio 2022 und `-A x64`;
- ARM64EC auf einem nativen Windows-on-Arm-Runner mit Visual Studio 2026 und
  `-A ARM64EC`.

CMake erhält bereits vor dem Build exakt
`SUBLAB808_WINDOWS_UPDATER_SIGNER_SHA256` und optional den davon verschiedenen
`SUBLAB808_WINDOWS_UPDATER_NEXT_SIGNER_SHA256`. Die Production-VST3 enthält genau
den Updater unter `Contents\Helpers\SubLab808Updater.exe`. Der MSI-Packager
erhält den exakten Updaterpfad, nativen Hosttest, `SignTool`, Zeitstempel-URL,
Zertifikats-Store/-Thumbprint, `ExpectedSignerSha256` und optional
`ExpectedNextSignerSha256`. Die PFX und sämtliche tatsächlich erzeugten
Signaturen müssen immer dem aktuellen Pin entsprechen; der nächste Pin erteilt
keine Berechtigung zum Signieren dieses Builds. Der Packager signiert sämtliche
PE-Dateien im Payload, prüft die administrativ extrahierte MSI-Nutzlast mit dem
Hosttest und signiert zuletzt das MSI. Die Windows-Evidence in Schema 3 bindet
aktuellen Pin, optionalen nächsten Pin und die daraus geordnete Payload-Allowlist
`[current]` beziehungsweise `[current, next]`. Sie enthält zusätzlich den exakten
40-stelligen Tag-Commit. Der Publish-Job akzeptiert x64 und ARM64EC nur, wenn
beide Commitwerte mit seinem erneut von `origin` geprüften Tag übereinstimmen.

Bevor ein Windows-Kandidat als Actions-Artefakt hochgeladen wird, führt derselbe
native Runner eine echte, stille Installations-Abnahme aus. Der Gate prüft Hash,
Authenticode-Signer und Evidence erneut. Ein nicht schreib- oder löschbar geteiltes
Lesehandle hält die exakt geprüfte MSI bis zum Ende der Installer-Prozesse stabil.
Version, Hersteller, MSI-Architektur und beide UpgradeCodes werden außerdem direkt
aus den MSI-Tabellen gegen unabhängige Workflow-Werte geprüft. Danach installiert
der Gate mit `msiexec /i`, verlangt
`INSTALLSTATE_DEFAULT`, vergleicht den vollständigen installierten VST3-Baum
bytegenau mit der Evidence und lädt genau dieses Bundle mit dem nativen Hosttest.
Danach deinstalliert er produktcodegenau mit `msiexec /x` und verlangt
`INSTALLSTATE_UNKNOWN` sowie einen entfernten Bundlepfad. Die Bereinigung läuft
auch bei Fehlern in einem `finally`-Pfad; ein nicht sauberer Ausgangszustand oder
eine nicht beweisbar vollständige Entfernung blockiert den Upload. Der Gate führt
keine manuelle rekursive Löschung im systemweiten VST3-Ziel aus, sondern verwendet
ausschließlich die ProductCode-genaue MSI-Deinstallation.

Der Matrix-Gate deckt den aktuellen Kandidaten auf einem sauberen Runner ab. Da
vor dem ersten Windows-Release noch keine signierte Vorversion existiert, kann
dieser Bootstrap keinen echten N→N+1-Lauf des ausgelieferten Updaters beweisen.
Der Autorisierungsjob liest deshalb die vollständige öffentliche Release-Liste
fail-closed und erlaubt den Bootstrap nur, wenn noch kein stabiles Windows-MSI
oder zugehöriges Evidence-Asset existiert. Derselbe Zustand wird vor Erzeugen
des Drafts und unmittelbar vor dessen Sichtbarkeit erneut geprüft. Eine
gebundene `--paginate --slurp`-Abfrage erfasst dabei alle REST-Seiten; zu viele,
unvollständige oder strukturell mehrdeutige Seiten sowie doppelte Release-IDs
werden abgelehnt.
Zusätzlich bindet `Installer/Windows/bootstrap-policy.json` diese Ausnahme
dauerhaft an Produkt und Tag `v1.4.0`; ein späterer Versionstag kann sie auch
nach dem Löschen alter Release-Assets nicht erneut verwenden. Autorisierung und
Publish beziehen Checker und Policy aus dem Default-Branch-Commit, aus dem der
Workflow gestartet wurde, nicht aus frei wählbarem Candidate-Code.
Dieser historische Bootstrap-Tag darf bei einem späteren Versions-Bump niemals
mit der CMake-Projektversion weitergeschoben werden.

Nach diesem einmaligen Bootstrap blockiert der Workflow absichtlich jeden
weiteren Release. Vor einer Nachfolgeversion muss der Release-DAG erweitert
werden: Die exakt installierte signierte Vorversion muss den vollständig
signierten Kandidaten über ihren normalen GitHub-, Download-, Resume-, UAC- und
Installationspfad auf x64 und ARM64EC übernehmen, bevor genau dieser Kandidat zu
`latest` wird. Ein neu kompilierter Test-Helper oder ein anderes Repository ist
kein Ersatz. Die reale Downgrade-Ablehnung und x64↔ARM64EC-Wechsel bleiben
ebenfalls separate Abnahmen.

Der macOS-Job führt den bestehenden, fail-closed Packagingpfad
`scripts/package-release.sh Release <Version>` aus. Er baut eine Universal-VST3
mit exakt `arm64` und `x86_64`, signiert verschachtelten Helper und VST3 mit
Hardened Runtime und Zeitstempel, signiert das PKG, wartet auf eine akzeptierte
Notarisierung, stapelt Tickets und prüft PKG sowie ZIP-Roundtrip mit Gatekeeper
und Hosttest. Source-Manifest, Tag-Commit, `dirty=false`, Signer-Pins,
Notary-Submission-ID und Artefakthashes werden in eine kleine Evidence-Datei
gebunden.

Ein separater Job auf `macos-15-intel` erhält keine Signing- oder
Notarisierungs-Secrets. Er checkt denselben unveränderlichen Tag aus, bestätigt
den Tag-Commit erneut gegen `origin`, baut daraus nur den Universal-Hosttest und
lädt das anhand Run-ID und Run-Attempt eindeutig benannte Actions-Artefakt des
macOS-Jobs herunter. Nach erneuter Prüfung des exakten Dreiersets aus PKG, ZIP
und Evidence sowie der Signer-Pins, Staple-Tickets und Gatekeeper-Entscheidungen
werden sowohl die VST3 aus dem veröffentlichten ZIP als auch die aus dem
veröffentlichten PKG expandierte Nutzlast mit `arch -x86_64` in den Hosttest
geladen. Damit laufen Scan, Instanziierung, State-Roundtrip und Audio-Render des
signierten/notarisierten Release-Builds wirklich auf Intel und nicht nur gegen
einen separat erzeugten CI-Build.

Ein vollständiger Release enthält exakt diese Plattformdateien:

- `SubLab808-<Version>-Windows-x64.msi`
- `SubLab808-<Version>-Windows-x64.evidence.json`
- `SubLab808-<Version>-Windows-arm64ec.msi`
- `SubLab808-<Version>-Windows-arm64ec.evidence.json`
- `SubLab808-<Version>-macOS-universal.pkg`
- `SubLab808-<Version>-macOS-universal-VST3.zip`
- `SubLab808-<Version>-macOS-universal.evidence.json`
- `SubLab808-<Version>-SHA256SUMS.txt`

Die drei Build-Kandidaten sind Actions-Artefakte mit nur einem Tag
Aufbewahrung. Ihre Container-Namen enthalten Run-ID und Run-Attempt, damit ein
erneuter Lauf niemals Kandidaten eines früheren Versuchs übernimmt. Im
Intel-Gate wird zusätzlich ein deterministischer SHA-256-Wert über alle drei
macOS-Dateien ausgegeben. Der Publish-Job hängt zwingend von diesem Gate ab,
berechnet denselben Wert aus seinem erneut heruntergeladenen Kandidaten und
verweigert bei jeder Abweichung die Veröffentlichung. Außerdem werden dort die Windows-Evidence und sämtliche Hashes
erneut geprüft. Auf einem macOS-Runner werden zusätzlich die heruntergeladenen
Signaturen, tatsächlichen Zertifikat-Fingerprints, Hardened Runtime,
Zeitstempel, Universal-Slices, Staple-Tickets und Gatekeeper-Entscheidungen
erneut geprüft.

## Gestufte Veröffentlichung und Fehlergrenzen

Der Publish-Job besitzt als einziger `contents: write`. Er verweigert vorhandene
Releases einschließlich Drafts, erstellt einen neuen Draft und merkt sich direkt
dessen exakte Release-ID. Erst nach Upload aller acht geprüften Dateien müssen
API-Assetnamen und serverseitige SHA-256-Digests vollständig passen. Unmittelbar
vor dem Publish werden Origin-Tag und die zu Beginn gemerkte Latest-Release-ID
erneut verglichen. Eine zwischenzeitliche manuelle oder fremde Veröffentlichung
blockiert damit den Sichtbarkeitsschritt.

Erst danach wird exakt dieser Draft mit einem einzelnen API-Aufruf
`draft=false`, `prerelease=false` und `make_latest=true` sichtbar. Der Tag muss
semantisch neuer als das bisherige Latest-Release sein. Anschließend werden die
eigene ID, Tag, Sichtbarkeit und Latest-Position erneut gelesen. Außerdem muss
die vollständige öffentliche Historie weiterhin frei von jedem anderen stabilen
Windows-Release sein; nur die soeben publizierte exakte ID und ihr festgelegter
Bootstrap-Tag sind bei dieser Nachprüfung erlaubt. Auch die acht Assetnamen und
ihre serverseitigen SHA-256-Digests müssen weiterhin exakt zu den lokalen,
bereits geprüften Dateien passen. Die gesamte Nachprüfung wird bei transienten
API-Fehlern bis zu dreimal wiederholt. Ein produktweiter
Concurrency-Lock serialisiert diesen Workflow. Manuelle oder andere API-Clients
kann GitHub damit nicht atomar sperren; die Nachprüfung schließt das relevante
Fenster um den Publish-Aufruf so weit wie die Release-API erlaubt.

Vor dem Sichtbarkeitsversuch entfernt der Fehler-Trap ausschließlich die in
diesem Lauf erzeugte ID, und auch nur wenn drei Identitätsprüfungen Tag,
eindeutige Run-Markierung und `draft=true` bestätigen. Lesen und Löschen werden
bis zu dreimal versucht; ein Fehler wird im Step Summary als Quarantänefall
gemeldet. Sobald der Publish-Aufruf versucht wurde, verweigert der Trap bewusst
eine automatische Löschung: Eine möglicherweise bereits von Updatern gesehene
öffentliche Version darf nicht still verschwinden. Jede nicht vollständig
verifizierbare Veröffentlichung schlägt stattdessen fehl und muss vor weiteren
Release-Aktionen manuell geprüft und gegebenenfalls quarantänisiert werden.
Fremde oder bereits vorhandene Drafts werden nie gesucht oder entfernt.

## Windows-Zertifikatswechsel

Ein Zertifikatswechsel von A nach B verwendet einen expliziten Bridge-Release:

1. Solange A noch gültig und verfügbar ist, bleiben PFX und
   `WINDOWS_CODE_SIGNING_CERT_SHA256` auf A. Zusätzlich wird
   `WINDOWS_NEXT_CODE_SIGNING_CERT_SHA256` auf B gesetzt. Der so gebaute
   Bridge-Release ist vollständig mit A signiert; nur sein Updater akzeptiert
   für ein später heruntergeladenes MSI A oder B.
2. Der Bridge-Release wird veröffentlicht und auf beiden Windows-Architekturen
   installiert sowie als Updatequelle geprüft. A bleibt aktueller Pin und B
   bleibt nächster Pin, bis die vorgesehene Client-Population sicher auf dieser
   Bridge-Version oder neuer angekommen ist. Der nächste Pin darf nicht vorher
   aus der Release-Konfiguration zurückgenommen werden.
3. Erst danach wechseln PFX und `WINDOWS_CODE_SIGNING_CERT_SHA256` gemeinsam auf
   B. Für den ersten vollständig mit B signierten Release wird
   `WINDOWS_NEXT_CODE_SIGNING_CERT_SHA256` geleert, sofern nicht bereits ein
   davon verschiedener Pin C für die nächste geplante Rotation benötigt wird.
   Das Leeren verändert den bereits veröffentlichten, unveränderlichen
   A/B-Bridge-Updater nicht.
4. Der A/B-Bridge-Release bleibt dauerhaft als stabiler Release verfügbar. Er
   darf weder gelöscht noch als Draft oder Prerelease umklassifiziert werden,
   solange ältere Installationen noch existieren können.

Der Windows-Updater fragt dafür höchstens 100 veröffentlichte Releases ab und
wählt den semantisch kleinsten stabilen Release, der neuer als seine installierte
Version ist. So erreicht auch ein länger offline gewesener A-Client zuerst die
A/B-Bridge und erst beim folgenden Update einen B-signierten Release. Liefert die
API exakt 100 Einträge, fehlt die Bridge in dieser begrenzten Historie, ist die
Versionsfolge mehrdeutig oder schlägt eine Signaturprüfung fehl, beendet der
Updater den Vorgang fail-closed. Dann ist eine manuelle Installation eines
vertrauenswürdig bezogenen, signierten Pakets erforderlich.

Launcher und Updater-Selbstprüfung bleiben in jedem Release auf den jeweiligen
aktuellen Pin festgelegt. Nur das heruntergeladene MSI darf current oder next
verwenden. Nach dessen Prüfung müssen MSI und ausnahmslos alle PE-Dateien des
Payloads denselben tatsächlich ermittelten Leaf-Fingerprint besitzen. Es gibt
keinen Subject-/Issuer-Fallback und keine gemischten Signer innerhalb eines
Pakets.

## Normaler CI-Pfad

Pull Requests und der Default Branch bauen weiterhin ausschließlich klar
markierte, unsignierte Prüfartefakte. Datei und Actions-Container heißen für
beide Produkte einheitlich
`<Product>-Windows-{x64|arm64ec}-VST3-UNSIGNED-NOT-FOR-DISTRIBUTION`. Diese
Artefakte sind keine Release-Kandidaten und dürfen nicht veröffentlicht werden.
