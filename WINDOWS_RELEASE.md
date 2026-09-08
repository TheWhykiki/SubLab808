# Signierter Cross-Platform-Releasepfad

`.github/workflows/windows-release.yml` ist der gemeinsame Produktionspfad für
Windows und macOS. Das Einchecken dieser Datei veröffentlicht jetzt nichts: Der
Workflow besitzt ausschließlich `workflow_dispatch`, verlangt
`confirm_release=true` und akzeptiert nur einen bereits auf `origin` vorhandenen
Tag im Format `vMAJOR.MINOR.PATCH`. Der Tag muss exakt zur CMake-Projektversion
passen und ein Vorfahr des aktuellen Default-Branch-Standes sein. Der Dispatch
muss außerdem aus dem Default Branch des kanonischen Repositories
`TheWhykiki/SubLab808` stammen. `scripts/check-windows-release-state.py` löst
aus der vollständig paginierten öffentlichen Historie entweder den einmaligen
Windows-Bootstrap oder die exakt neueste stabile Windows-Baseline N auf. Nur im
Bootstrap-Modus muss zusätzlich
`confirm_first_windows_release_bootstrap=true` gesetzt sein; ein Follow-up
verlangt stattdessen den nativen installierten N→N+1-Gate auf x64 und ARM64EC.

Ein Release darf niemals nur Windows enthalten, weil auch der macOS-Updater
`/releases/latest` auswertet. Der Stage-Job benötigt deshalb sowohl beide
Windows-Jobs als auch den signierten und notarisierten macOS-Job und die
nachgelagerte native Intel-Abnahme genau dieses macOS-Kandidaten.

## Geschützte Konfiguration

Vor dem ersten echten Lauf müssen im GitHub-Repository diese Werte eingerichtet
werden:

Zusätzlich muss vor dem Bootstrap unter **Settings → General → Releases**
**Enable release immutability** aktiviert werden. Diese Einstellung schützt nur
künftig veröffentlichte Releases und heilt keine bereits vorhandenen veränderlichen
Releases nachträglich; der Workflow prüft sie deshalb vor Authorization, Stage und
Finalize jeweils fail-closed über das separate Administration-Read-Token.

Unter **Settings → Environments** muss außerdem vor dem ersten Lauf das
Environment `physical-daw-release` angelegt werden. Es braucht mindestens einen
expliziten Benutzer als **Required reviewer** (keine Teams), aktiviertes **Prevent self-review** und
deaktiviertes **Allow administrators to bypass configured protection rules**.
Seine Deployment-Branches werden auf **Protected branches only** begrenzt; der
Default Branch selbst muss geschützt sein.
Das Environment enthält keine Secrets. Wird es vergessen und von GitHub beim
ersten Bezug ungeschützt angelegt, bricht der nachgelagerte Validator ab, weil
er genau eine nichtleere `required_reviewers`-Regel mit
`prevent_self_review=true`, `deployment_branch_policy.protected_branches=true`
und `custom_branch_policies=false` verlangt. Der geprüfte Workflow-Run muss
außerdem auf dem gemeldeten Default Branch liegen. Der Validator liest diesen
Branch zusätzlich direkt über die GitHub-API und verlangt `protected=true`; die
Environment-Auswahl **Protected branches only** genügt für sich allein
ausdrücklich nicht. Candidate-Commit und Workflow-Commit müssen serverseitig
nachweisbar Vorfahren des beobachteten Branch-Tips sein. Unmittelbar vor der
Promotion liest der Finalizer Default-Branch, Schutzstatus und beide
Abstammungsbeziehungen erneut; ein Default-Branch-Wechsel, Force-Push oder
entfernter Commit lässt den Candidate in Quarantäne. Auch ein fehlender oder
nicht exakt passender Review-Kommentar verhindert die Promotion.

| Typ | Name | Inhalt |
| --- | --- | --- |
| Secret | `WINDOWS_CODE_SIGNING_PFX_BASE64` | Base64-kodierte PFX-Datei mit genau einem privaten Code-Signing-Schlüssel |
| Secret | `WINDOWS_CODE_SIGNING_PFX_PASSWORD` | Passwort der PFX-Datei |
| Secret | `WINDOWS_RELEASE_GATE_PRIVATE_KEY_PKCS8_BASE64` | Kanonisches Base64 einer separaten ECDSA-P-256-PKCS#8-Private-Key-Datei; signiert ausschließlich kurzlebige N→N+1-Abnahmefreigaben |
| Secret | `IMMUTABLE_RELEASES_ADMIN_READ_TOKEN` | Eng begrenztes Fine-grained-PAT oder GitHub-App-Token mit ausschließlich Repository-Administration-Lesezugriff zur fail-closed Prüfung von `immutable-releases.enabled=true`; nie an Build- oder Acceptance-Jobs übergeben |
| Variable | `WINDOWS_CODE_SIGNING_CERT_SHA256` | Öffentlicher, 64-stelliger SHA-256-Fingerprint des aktuellen Windows-Leaf-Zertifikats |
| Variable | `WINDOWS_NEXT_CODE_SIGNING_CERT_SHA256` | Optionaler, vom aktuellen Pin verschiedener 64-stelliger SHA-256-Fingerprint des nächsten Windows-Leaf-Zertifikats |
| Variable | `WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY` | Zum Private Key gehöriger P-256-Public-Key als exakt 128 Großhex-Zeichen `X||Y` |
| Variable | `WINDOWS_RELEASE_GATE_NEXT_PUBLIC_KEY_XY` | Optionaler, verschiedener nächster P-256-Public-Key im selben Format für eine vorbereitete Gate-Key-Rotation |
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

Die Release-Gate-Autorisierung verwendet absichtlich ein davon unabhängiges
ECDSA-P-256-Schlüsselpaar. Der Workflow importiert den privaten PKCS#8-Key nur
in den eng begrenzten Verifikations- und Acceptance-Schritten, leitet daraus
den Public Key ab und verlangt bytegenaue Übereinstimmung mit
`WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY`. Der Private Key wird weder an Build-,
Packaging-, Stage- noch Finalize-Prozesse vererbt. Der aktuelle und optional
nächste Public Key werden dagegen in den Updater kompiliert und in der Evidence
gebunden, damit eine Rotation über genau einen Bridge-Release möglich bleibt.

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
`SUBLAB808_WINDOWS_UPDATER_SIGNER_SHA256`, optional den davon verschiedenen
`SUBLAB808_WINDOWS_UPDATER_NEXT_SIGNER_SHA256`, den exakt 128-stelligen
`SUBLAB808_WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY` und optional den davon
verschiedenen `SUBLAB808_WINDOWS_RELEASE_GATE_NEXT_PUBLIC_KEY_XY`. Die
Production-VST3 enthält genau den Updater unter
`Contents\Helpers\SubLab808Updater.exe`. Der MSI-Packager
erhält den exakten Updaterpfad, nativen Hosttest, `SignTool`, Zeitstempel-URL,
Zertifikats-Store/-Thumbprint, `ExpectedSignerSha256` und optional
`ExpectedNextSignerSha256`. Die PFX und sämtliche tatsächlich erzeugten
Signaturen müssen immer dem aktuellen Pin entsprechen; der nächste Pin erteilt
keine Berechtigung zum Signieren dieses Builds. Entsprechend muss der aktive
Release-Gate-Private-Key exakt zum aktuellen Gate-Public-Key gehören; der
nächste Gate-Key darf noch keine Autorisierung ausstellen. Der Packager signiert sämtliche
PE-Dateien im Payload, prüft die administrativ extrahierte MSI-Nutzlast mit dem
Hosttest und signiert zuletzt das MSI. Die Windows-Evidence in Schema 4 bindet
aktuellen Pin, optionalen nächsten Pin und die daraus geordnete Payload-Allowlist
`[current]` beziehungsweise `[current, next]`. Evidence-Schema 4 bindet außerdem
`releaseGatePublicKeyXY`, den optionalen `releaseGateNextPublicKeyXY` und die
geordneten `releaseGatePublicKeyAllowlistXY` nach demselben Current/Next-Prinzip.
Sie enthält zusätzlich den exakten
40-stelligen Tag-Commit. Der Stage-Job akzeptiert x64 und ARM64EC nur, wenn
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

Der Autorisierungsjob liest die vollständige öffentliche Release-Liste über
`gh api --paginate --slurp` mit API-Version `2026-03-10`. Der Resolver akzeptiert
nur exakte, immutable stabile Releases mit dem vollständigen Set aus acht
Cross-Platform-Assets oder vollständige immutable Prereleases als dauerhaft
sichtbare Quarantänen.
Er bindet Produkt, Candidate-Tag, Baseline-ID/-Tag/-Commit und einen kanonischen
Digest der gesamten übrigen Historie in eine einzige JSON-Zeile. Der Candidate
muss strikt neuer als jede stabile oder quarantänisierte Windows-Version sein.
Dieses JSON und sein SHA-256 werden während Stage und Finalize bytegenau erneut
verlangt. Beim Ausblenden der eigenen Candidate-ID wird die flache Historie
wieder exakt in 100er REST-Seiten aufgeteilt; dadurch bleiben auch Historien mit
mehr als 100 Releases fail-closed prüfbar.

Ohne Windows-Baseline ist nur der in
`Installer/Windows/bootstrap-policy.json` dauerhaft gebundene Tag `v1.4.0`
zulässig. Dieser Bootstrap führt auf beiden Architekturen eine saubere
Installation, vollständigen Evidence-/Payloadvergleich, nativen Hostload und
produktcodegenaue Deinstallation aus. Der Policy-Tag darf bei einem späteren
Versions-Bump nie verschoben werden. Eine bereits vorhandene Quarantäne macht
die Historie nicht zu einer vertrauenswürdigen Baseline und erlaubt auch keine
Wiederverwendung ihres Tags.

Für jeden Follow-up-Release wird dagegen genau das MSI der aufgelösten stabilen
Baseline N aus deren öffentlicher immutable Release-ID geladen, gegen
serverseitigen Digest, Evidence, Signatur, MSI-Identität und Commit attestiert
und installiert. Noch vor dem Staging muss die Evidence dieser Baseline den
aktiven Candidate-Gate-Key in ihrer Current/Next-Allowlist autorisieren. Nur der
darin ausgelieferte und erneut hash-/signaturgeprüfte
`Contents\Helpers\SubLab808Updater.exe` darf den öffentlichen Candidate N+1
abrufen. Der Harness startet ihn mit dem eng gebundenen CLI
`--release-gate --challenge … --response-pipe … --parent-process-id …
--release-id … --tag … --source-commit …
--parent-process-created-at-filetime … --expires-at-unix-seconds …
--authorization-signature-p1363 …`. Die Signatur ist exakt 64 Byte
IEEE-P1363-`r||s`, kanonisch als 128 Großhex-Zeichen und mit Low-S normalisiert.
Sie bindet Domain, Schema, Repository, Produkt, Architektur, installierte
Version, Release-ID, Tag, Commit, Challenge, Pipe, Parent-PID und dessen
Erstellungszeit sowie eine höchstens fünf Minuten entfernte Ablaufzeit in eine
kanonische UTF-8-Nachricht. Der kopierte `--resume`-Child muss
über eine nur für den aktuellen Benutzer zugängliche Named Pipe genau eine
kanonische `phase=verified`-Receipt liefern; Client-PID, Parent-Beziehung,
Pfad, Hash und Signer werden serverseitig geprüft. Challenge, Pipe, PID,
Erstellungszeit, Ablaufzeit, Signatur und sonstige Autorisierungsdaten dürfen
nicht im Journal persistieren. Sowohl der installierte Parent als auch der
kopierte Resume-Child prüfen die Autorisierung vor Mutex-, Datei-, Netzwerk-
oder UAC-Arbeit erneut. `--resume` ist im Gate-Modus ausschließlich der interne
Handoff auf den gerade frisch erzeugten `created`-Vorgang: Der installierte
Helper lehnt eine übergebene Resume-ID ab, und der Child verweigert jeden bereits
fortgeschrittenen oder `verified` Gate-Vorgang. Ein Gate-Abbruch wird daher nie
aus persistentem Fortschritt als neue Abnahme fortgesetzt.

Nach der Receipt verlangt der Gate Candidate-ProductState, den vollständigen
installierten Evidence-Baum, nativen Hostload, Ablehnung des exakten älteren
N-MSI, unveränderte N+1-Nutzlast, produktcodegenaue Deinstallation und die
Bereinigung des test-eigenen Updater-Vorgangs. Ein neu gebauter Candidate-Helper
ist kein Ersatz für den installierten N-Helper. x64↔ARM64EC-Wechsel bleiben
eine separate physische Abnahme, weil die beiden UpgradeCodes eine parallele
Installation absichtlich verhindern.

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

## Physische Cubase-/Reaper-Freigabe

Es gibt bewusst keinen angeblichen automatischen Cubase- oder Reaper-Test:
GitHub-hosted Runner enthalten diese DAWs nicht, und das Repository setzt keine
nicht vorhandene Self-hosted-DAW-Farm voraus. Nach den automatischen Gates wartet
der Job `physical-daw-acceptance` deshalb ohne belegten Runner im geschützten
Environment. Die prüfende Person lädt die Dateien aus der im Job verlinkten,
öffentlichen immutable Prerelease-ID, vergleicht deren SHA-256-Werte mit den
serverseitigen `digest`-Feldern und führt auf physischer Hardware diese acht
Abnahmen aus:

- Windows x64 MSI in Cubase und Reaper;
- Windows ARM64EC MSI in Cubase und Reaper;
- macOS universal in Cubase und Reaper, jeweils für den PKG-installierten und
  den aus dem ZIP bereitgestellten VST3-Pfad.

Jede Abnahme umfasst Installation beziehungsweise Kopie, Plug-in-Rescan,
Instanziierung, Audioverarbeitung, State-Save/-Reload, Downgrade-Ablehnung auf
Windows und saubere Entfernung.
Auf Windows bleibt UAC aktiviert; auf macOS bleiben Gatekeeper und die
gestapelten Notarisierungstickets aktiv. `machine` ist nur ein nicht geheimes
Inventar-Alias, niemals Seriennummer, Benutzername oder sonstiges Geheimnis.

Erst danach darf ein anderer Required Reviewer den wartenden Environment-Job
freigeben. Sein Kommentar muss ausschließlich ein JSON-Objekt nach Schema 1
enthalten; Markdown-Fences oder Begleittext sind nicht zulässig. Die exakten
Werte für `runId`, `runAttempt`, `releaseId`, `tag`, `commit`,
`assetManifestSha256` und die vier Digests stehen in der Zusammenfassung von
`stage-release`. Vor dem Einfügen werden alle Platzhalter ersetzt:

```json
{
  "schemaVersion": 1,
  "repository": "TheWhykiki/SubLab808",
  "product": "SubLab808",
  "runId": 123456789,
  "runAttempt": 1,
  "releaseId": 987654321,
  "tag": "v1.4.1",
  "commit": "0000000000000000000000000000000000000000",
  "assetManifestSha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "artifacts": {
    "SubLab808-1.4.1-Windows-x64.msi": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
    "SubLab808-1.4.1-Windows-arm64ec.msi": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
    "SubLab808-1.4.1-macOS-universal.pkg": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
    "SubLab808-1.4.1-macOS-universal-VST3.zip": "sha256:0000000000000000000000000000000000000000000000000000000000000000"
  },
  "checks": [
    {"platform":"windows-x64-msi","host":"Cubase","hostVersion":"13.0.50","osVersion":"Windows 11 24H2","machine":"qa-win-x64-01","tester":"qa-operator","testedAt":"2026-09-08T12:00:00Z","result":"pass"},
    {"platform":"windows-x64-msi","host":"Reaper","hostVersion":"7.50","osVersion":"Windows 11 24H2","machine":"qa-win-x64-01","tester":"qa-operator","testedAt":"2026-09-08T12:10:00Z","result":"pass"},
    {"platform":"windows-arm64ec-msi","host":"Cubase","hostVersion":"13.0.50","osVersion":"Windows 11 24H2 ARM64","machine":"qa-win-arm-01","tester":"qa-operator","testedAt":"2026-09-08T12:20:00Z","result":"pass"},
    {"platform":"windows-arm64ec-msi","host":"Reaper","hostVersion":"7.50","osVersion":"Windows 11 24H2 ARM64","machine":"qa-win-arm-01","tester":"qa-operator","testedAt":"2026-09-08T12:30:00Z","result":"pass"},
    {"platform":"macos-universal-pkg","host":"Cubase","hostVersion":"13.0.50","osVersion":"macOS 15.6","machine":"qa-mac-01","tester":"qa-operator","testedAt":"2026-09-08T12:40:00Z","result":"pass"},
    {"platform":"macos-universal-pkg","host":"Reaper","hostVersion":"7.50","osVersion":"macOS 15.6","machine":"qa-mac-01","tester":"qa-operator","testedAt":"2026-09-08T12:50:00Z","result":"pass"},
    {"platform":"macos-universal-zip","host":"Cubase","hostVersion":"13.0.50","osVersion":"macOS 15.6","machine":"qa-mac-01","tester":"qa-operator","testedAt":"2026-09-08T13:00:00Z","result":"pass"},
    {"platform":"macos-universal-zip","host":"Reaper","hostVersion":"7.50","osVersion":"macOS 15.6","machine":"qa-mac-01","tester":"qa-operator","testedAt":"2026-09-08T13:10:00Z","result":"pass"}
  ]
}
```

Nach der Freigabe liest der Job über den dokumentierten REST-Endpunkt
`GET /repos/{owner}/{repo}/actions/runs/{run_id}/approvals` die Review-Historie.
Er akzeptiert genau einen Eintrag: `state` muss exakt `approved` sein,
`environments` muss ausschließlich ID und Name des aktuellen
`physical-daw-release` enthalten, `user.id` und `user.login` müssen exakt einen
direkt im Environment konfigurierten Benutzer benennen, dieser darf weder per
stabiler User-ID noch per Login ursprünglicher Workflow-Aktor oder Re-run-Aktor
sein und `comment` muss das obige Receipt
erfüllen. Zusätzliche, abgelehnte, unklare oder zu einem anderen Versuch
gehörende Review-Einträge sind fail-closed. Zusätzlich werden das Environment
über `GET /repos/{owner}/{repo}/environments/physical-daw-release`, der Run über
`GET /repos/{owner}/{repo}/actions/runs/{run_id}`, der aktuelle Default Branch
über `GET /repos/{owner}/{repo}/branches/{branch}` und der Candidate über seine
exakte Release-ID erneut gelesen. Nur eine Branch-Antwort mit passendem Namen,
`protected=true` und gültigem beobachtetem Commit wird in den kanonischen
Receipt-Umschlag übernommen. Zwei Compare-API-Antworten müssen zusätzlich
`status=ahead|identical` liefern und Candidate- sowie Workflow-Commit jeweils
als exakten Merge-Base des beobachteten Tips belegen.

Der Validator verlangt die vollständige Cubase-/Reaper-Matrix mit acht
`result=pass`, nachvollziehbaren DAW-/OS-Versionen, Maschinen-Alias, Tester und
UTC-Zeitpunkt nach Veröffentlichung des Candidates. Das Receipt bindet zudem
Run-ID und -Attempt, Release-ID, Tag-Commit, den kanonischen Digest aller acht
Asset-Metadaten und die serverseitigen Digests der vier installierbaren
Artefakte. Der validierte kanonische Receipt-Umschlag wird 90 Tage als
Actions-Artefakt aufbewahrt; SHA-256 und Base64-Inhalt werden zusätzlich in
die bei der Promotion gesetzten Release Notes geschrieben. Reviewer-Login und
stabile GitHub-User-ID sowie Environment-ID bleiben darin dauerhaft gebunden,
auch wenn das Actions-Artefakt abläuft oder ein Login später umbenannt wird. Das ist eine
nachvollziehbare, authentifizierte menschliche Attestation, aber kein
kryptografischer Beweis dafür, dass die DAWs tatsächlich ausgeführt wurden.

Die drei Build-Kandidaten sind Actions-Artefakte mit nur einem Tag
Aufbewahrung. Die nativen Windows-Hostharnesses bleiben sieben Tage, die beiden
kleinen Transition-Receipts 30 Tage als Actions-Artefakte erhalten. Receipts
können nicht nachträglich an den bereits immutable Candidate angehängt werden
und sind deshalb bewusst keine Release-Assets. Alle Container-Namen enthalten
Run-ID und Run-Attempt, damit ein erneuter Lauf niemals Kandidaten eines
früheren Versuchs übernimmt. Im
Intel-Gate wird zusätzlich ein deterministischer SHA-256-Wert über alle drei
macOS-Dateien ausgegeben. Der Stage-Job hängt zwingend von diesem Gate ab,
berechnet denselben Wert aus seinem erneut heruntergeladenen Kandidaten und
verweigert bei jeder Abweichung die Veröffentlichung. Außerdem werden dort die Windows-Evidence und sämtliche Hashes
erneut geprüft. Auf einem macOS-Runner werden zusätzlich die heruntergeladenen
Signaturen, tatsächlichen Zertifikat-Fingerprints, Hardened Runtime,
Zeitstempel, Universal-Slices, Staple-Tickets und Gatekeeper-Entscheidungen
erneut geprüft.

## Gestufte Veröffentlichung und Fehlergrenzen

`authorize_windows_release`, `stage-release` und `finalize-release` prüfen über
ein separates Administration-Read-Token jeweils fail-closed, dass GitHubs
Repository-Policy `immutable-releases.enabled=true` meldet. Alle REST-Abfragen,
die das neue `immutable`-Feld auswerten oder den Resolver speisen, pinnen
API-Version `2026-03-10`.

`stage-release` besitzt `contents: write`, verweigert vorhandene Releases zum
Candidate-Tag, erstellt einen Draft und merkt sich sofort dessen exakte ID.
Alle acht Dateien werden über den ID-spezifischen Upload-Endpunkt angehängt.
Vor Sichtbarkeit müssen der Draft per ID, Tag, explizit beim POST gespeicherten
40-hex `target_commitish`, Run-Marker, vollständigem Namenssatz, Größen,
Asset-IDs und serverseitigen SHA-256-Digests übereinstimmen. Der Workflow prüft
außerdem den gepielten Remote-Tag gegen denselben Commit. Erst dann wird genau
diese ID mit `draft=false`, `prerelease=true` und `make_latest=false`
veröffentlicht. GitHub muss danach `immutable=true` zurückgeben; der vorherige
Latest-Zustand und der autorisierte Historien-Snapshot müssen unverändert sein.

Der öffentliche Prerelease ist absichtlich bereits für die unveränderten
Updater in den nativen x64- und ARM64EC-Gates erreichbar, aber noch nicht
`latest`. Beide automatischen Acceptance-Jobs erhalten weder Code-Signing-/Notary-Secrets noch
das Administration-Token. Sie erhalten ausschließlich den separaten
Release-Gate-Private-Key; ohne eine dazu passende kurzlebige Signatur kann kein
lokaler Prozess einen quarantänisierten Prerelease in den UAC-Pfad treiben. Der
anschließende Environment-Job erhält keine Signing- oder Notary-Secrets. Nur
wenn beide nativen Gates und das oben beschriebene physische Receipt erfolgreich
sind und Release-ID, acht
Asset-Metadaten, Immutability, Origin-Tag, Baseline, gesamte übrige Historie und
vorheriges Latest nochmals unverändert sind, setzt `finalize-release` dieselbe
ID auf `prerelease=false` und `make_latest=true`. Titel und Notes werden dabei
von „transition candidate“ auf den stabilen Release-Text umgestellt. Der
anschließende Resolver-Aufruf erlaubt ausschließlich diese soeben promotete
ID, schließt sie wieder aus Baseline/History-Digest aus und muss bytegenau das
initiale JSON liefern.

Der produktweite Concurrency-Lock serialisiert Workflow-Läufe. Manuelle Writer
kann GitHub nicht atomar sperren; die wiederholten exakten API-Prüfungen
begrenzen dieses Rennen. Ein Fehler vor jedem Veröffentlichungsversuch darf nur
den eigenen, weiterhin eindeutig identifizierten Draft löschen. Ab dem ersten
Publish-Versuch wird nie automatisch gelöscht oder der Tag wiederverwendet.
Scheitert ein Architecture-Gate, bleibt die geprüfte ID als öffentlicher
immutable Prerelease quarantänisiert und wird bei späteren Versionsentscheidungen
mitgezählt. Bei einem fehlgeschlagenen oder zeitlich unklaren Promotion-PATCH
liest der Finalizer die exakte ID begrenzt erneut: vollständig Stable+Latest
gilt als Erfolg, unverändert immutable Prerelease als Quarantäne; jeder andere
Zustand wird ehrlich als `UNKNOWN` gemeldet und verlangt manuelle Prüfung ohne
Delete, Demotion oder Neuveröffentlichung.

GitHub-hosted Windows-Runner schalten UAC aus. Der automatische Follow-up-Gate
durchläuft deshalb den echten `ShellExecuteEx(..., runas)`-/`msiexec`-Pfad,
beweist aber keinen sichtbaren Consent-Dialog und keine DAW-Ausführung. Genau
deshalb bleiben die physischen x64-, Windows-on-Arm- und macOS-Abnahmen vor der
Environment-Freigabe Pflicht. GitHub Actions validiert nur Identität, Umfang und
Bindung der menschlichen Attestation; es behauptet nicht, diese Abnahmen selbst
ausgeführt zu haben.

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

## Windows-Release-Gate-Keywechsel

Der Gate-Key rotiert unabhängig vom Authenticode-Zertifikat und verwendet einen
eigenen Bridge-Release:

1. Release N enthält `current=K0`, `next=K1` und damit die Evidence-Allowlist
   `[K0, K1]`. Der aktive Private Key bleibt K0; nur er signiert die
   kurzlebigen Transition-Autorisierungen für N.
2. Erst nachdem N stabil veröffentlicht und auf x64 sowie ARM64EC installiert
   wurde, wechseln Private Key und `WINDOWS_RELEASE_GATE_PUBLIC_KEY_XY`
   atomar auf K1. Der Pre-Publish-Gate für N+1 beweist vor jeder Sichtbarkeit,
   dass K1 bereits in der immutable Baseline-Evidence von N autorisiert war.
3. N+1 enthält `current=K1` und optional `next=K2`. K0 kann erst entfernt
   werden, wenn keine weitere Abnahme gegen einen K0-only-Updater erforderlich
   ist. Ein ungeplanter Sprung auf einen nicht in der Baseline-Allowlist
   enthaltenen Key blockiert vor dem Staging.

Public Keys sind exakt 128 Großhex-Zeichen `X||Y` auf der NIST-P-256-Kurve.
Private Keys sind kanonisches Base64 einer PKCS#8-Struktur. Signaturen sind
SHA-256/ECDSA im festen 64-Byte-IEEE-P1363-Format und werden vor Übergabe auf
Low-S normalisiert. Current und Next müssen verschieden sein; der aktive
Private Key muss immer exakt Current entsprechen.

## Normaler CI-Pfad

Pull Requests und der Default Branch bauen weiterhin ausschließlich klar
markierte, unsignierte Prüfartefakte. Datei und Actions-Container heißen für
beide Produkte einheitlich
`<Product>-Windows-{x64|arm64ec}-VST3-UNSIGNED-NOT-FOR-DISTRIBUTION`. Diese
Artefakte sind keine Release-Kandidaten und dürfen nicht veröffentlicht werden.
