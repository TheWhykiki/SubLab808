# Signierter Cross-Platform-Releasepfad

`.github/workflows/windows-release.yml` ist der gemeinsame Produktionspfad für
Windows und macOS. Das Einchecken dieser Datei veröffentlicht jetzt nichts: Der
Workflow besitzt ausschließlich `workflow_dispatch`, verlangt
`confirm_release=true` und akzeptiert nur einen bereits auf `origin` vorhandenen
Tag im Format `vMAJOR.MINOR.PATCH`. Der Tag muss exakt zur CMake-Projektversion
passen und ein Vorfahr des aktuellen Default-Branch-Standes sein. Der Dispatch
muss außerdem aus dem Default Branch des kanonischen Repositories
`TheWhykiki/SubLab808` stammen.

Ein Release darf niemals nur Windows enthalten, weil auch der macOS-Updater
`/releases/latest` auswertet. Der Publish-Job benötigt deshalb sowohl beide
Windows-Jobs als auch den signierten und notarisierten macOS-Job.

## Geschützte Konfiguration

Vor dem ersten echten Lauf müssen im GitHub-Repository diese Werte eingerichtet
werden:

| Typ | Name | Inhalt |
| --- | --- | --- |
| Secret | `WINDOWS_CODE_SIGNING_PFX_BASE64` | Base64-kodierte PFX-Datei mit genau einem privaten Code-Signing-Schlüssel |
| Secret | `WINDOWS_CODE_SIGNING_PFX_PASSWORD` | Passwort der PFX-Datei |
| Variable | `WINDOWS_CODE_SIGNING_CERT_SHA256` | Öffentlicher, 64-stelliger SHA-256-Fingerprint des Windows-Leaf-Zertifikats |
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

Die SHA-256-Pins sind absichtlich öffentliche Repository-Variablen. Der
Windows-Pin wird vor dem Build exakt in Updater und Plug-in-Launcher kompiliert.
Bei einem Zertifikatswechsel müssen Variable, Build und Release gemeinsam
umgestellt werden.

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
`SUBLAB808_WINDOWS_UPDATER_SIGNER_SHA256`. Die Production-VST3 enthält genau den
Updater unter `Contents\Helpers\SubLab808Updater.exe`. Der MSI-Packager erhält
den exakten Updaterpfad, nativen Hosttest, `SignTool`, Zeitstempel-URL,
Zertifikats-Store/-Thumbprint und `ExpectedSignerSha256`. Er signiert sämtliche
PE-Dateien im Payload, prüft die administrativ extrahierte MSI-Nutzlast mit dem
Hosttest und signiert zuletzt das MSI. Jede Windows-Evidence enthält zusätzlich
den exakten 40-stelligen Tag-Commit. Der Publish-Job akzeptiert x64 und ARM64EC
nur, wenn beide Commitwerte mit seinem erneut von `origin` geprüften Tag
übereinstimmen.

Der macOS-Job führt den bestehenden, fail-closed Packagingpfad
`scripts/package-release.sh Release <Version>` aus. Er baut eine Universal-VST3
mit exakt `arm64` und `x86_64`, signiert verschachtelten Helper und VST3 mit
Hardened Runtime und Zeitstempel, signiert das PKG, wartet auf eine akzeptierte
Notarisierung, stapelt Tickets und prüft PKG sowie ZIP-Roundtrip mit Gatekeeper
und Hosttest. Source-Manifest, Tag-Commit, `dirty=false`, Signer-Pins,
Notary-Submission-ID und Artefakthashes werden in eine kleine Evidence-Datei
gebunden.

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
Publish-Job werden die Windows-Evidence und sämtliche Hashes
erneut geprüft. Auf einem macOS-Runner werden zusätzlich die heruntergeladenen
Signaturen, tatsächlichen Zertifikat-Fingerprints, Hardened Runtime,
Zeitstempel, Universal-Slices, Staple-Tickets und Gatekeeper-Entscheidungen
erneut geprüft.

## Atomare Veröffentlichung

Der Publish-Job besitzt als einziger `contents: write`. Er verweigert vorhandene
Releases einschließlich Drafts, erstellt einen neuen Draft und merkt sich direkt
dessen exakte Release-ID. Erst nach Upload aller acht geprüften Dateien müssen
API-Assetnamen und serverseitige SHA-256-Digests vollständig passen. Unmittelbar
vor dem Publish werden Origin-Tag und die zu Beginn gemerkte Latest-Release-ID
erneut verglichen. Eine zwischenzeitliche manuelle oder fremde Veröffentlichung
verwirft den eigenen Draft.

Erst danach wird exakt dieser Draft mit `draft=false`, `prerelease=false` und
`make_latest=true` sichtbar. Der Tag muss semantisch neuer als das bisherige
Latest-Release sein. Ein einziger produktweiter Concurrency-Lock verhindert
zusätzlich, dass zwei Tags ihre Latest-Reihenfolge gegenseitig überschreiben.
Bei einem Fehler wird ausschließlich die im aktuellen Lauf neu erzeugte
Release-ID gelöscht; fremde oder bereits vorhandene Drafts werden nie gesucht
oder entfernt.

## Windows-Zertifikatswechsel

Der aktuell installierte Windows-Updater vertraut absichtlich nur dem exakt in
seine Version kompilierten Leaf-Zertifikat. PFX und
`WINDOWS_CODE_SIGNING_CERT_SHA256` dürfen deshalb nicht einfach gemeinsam auf
ein neues Zertifikat umgestellt und als transparentes Auto-Update bezeichnet
werden: Ein älterer Updater verwirft das neu signierte MSI.

Vor Ablauf oder Austausch des Zertifikats muss entweder ein gesondert
reviewter Bridge-Releasepfad mit getrennten Pins für den signierten Helper und
den nächsten Download implementiert werden, oder der erste Release mit dem
neuen Zertifikat wird ausdrücklich als manuelle Neuinstallation verteilt.
Clients, die einen zeitlich begrenzten Bridge-Release nicht installiert haben,
benötigen ebenfalls die manuelle Installation. Der derzeitige Single-Pin-Pfad
behauptet keine nahtlose Zertifikatsrotation.

## Normaler CI-Pfad

Pull Requests und der Default Branch bauen weiterhin ausschließlich klar
markierte, unsignierte Prüfartefakte. Datei und Actions-Container heißen für
beide Produkte einheitlich
`<Product>-Windows-{x64|arm64ec}-VST3-UNSIGNED-NOT-FOR-DISTRIBUTION`. Diese
Artefakte sind keine Release-Kandidaten und dürfen nicht veröffentlicht werden.
