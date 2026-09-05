# Windows-MSI-Paketierung

Diese Schicht baut aus einem bereits geprüften VST3-Bundle ein eigenständiges,
per-machine installiertes MSI. Der Produktname und die dauerhaften
Upgrade-Identitäten kommen aus `Installer/Windows/package-config.json`; dieselbe
Paketierungslogik wird deshalb ohne Produkt-Sonderfälle in beiden Plug-in-Repositories
verwendet.

## Paket- und Architekturvertrag

| Aufruf | VST3-Payload | MSI-Plattform | Ziel |
| --- | --- | --- | --- |
| `-Architecture x64` | `Contents\x86_64-win` | `x64` | `CommonFiles6432Folder\VST3\<Product>.vst3` |
| `-Architecture arm64ec` | `Contents\arm64ec-win` | `arm64` | `CommonFiles6432Folder\VST3\<Product>.vst3` |

ARM64EC ist die Windows-on-Arm-Ausprägung des VST3-Plug-ins. Windows Installer
kennt dafür keine eigene ARM64EC-Plattform; das MSI ist daher ein Arm64-Paket,
während das darin geprüfte Plug-in ein ARM64EC-PE-Image ist. Die x64- und
ARM64EC-Pakete besitzen unterschiedliche, stabile UpgradeCodes. Da beide dasselbe
VST3-Ziel besitzen, blockiert jedes MSI eine parallele Installation der anderen
Architektur. Ein Architekturwechsel erfolgt durch Deinstallation des alten Pakets
und anschließende Installation des neuen.

Das MSI installiert ausschließlich das vollständige Bundle. Die rekursive Ernte
ist an einen benannten WiX-Bindepfad gebunden. Reparse Points, Symlinks, Junctions,
Alternate Data Streams und unter Windows kollidierende Pfadschreibweisen werden
vorher abgelehnt. Leere Quellverzeichnisse sind nur zulässig, wenn sie die
administrative Extraktion identisch wiederherstellt. Es gibt keine Custom Actions,
keine eingebetteten Skripte und keine Netzwerk- oder Update-Logik im MSI.

Vor der Ernte klassifiziert das Skript **jede** Datei nach ihrem Inhalt. Ein
gültiger `MZ`-/`PE\0\0`-Header wird unabhängig vom Dateinamen erkannt; alle so
gefundenen PE-Images müssen zur gewählten Architektur passen und werden im
Produktionsmodus exakt einmal signiert. `.exe`, `.dll` und `.vst3` ohne gültiges
MZ/PE-Image sowie beschädigte MZ-Dateien werden abgelehnt. Skript-, Installer-,
Shell- und weitere ausführbare Risiko-Endungen (unter anderem PowerShell, Batch,
WSH, Python, MSI/MSP, SCR, CPL, OCX und SYS) sind im Bundle vollständig verboten.

## Voraussetzungen

- Windows mit PowerShell 7.2 oder neuer
- ein .NET SDK, das das lokale WiX-Tool ausführen kann
- Visual Studio C++ Build Tools und Windows SDK (`dumpbin.exe`, `signtool.exe`)
- ein bereits gebautes `<Product>.vst3` mit genau einem passenden Windows-Payload
- für Produktionsartefakte ein zugängliches Code-Signing-Zertifikat und ein eigener
  HTTPS-RFC-3161-Zeitstempel-Endpunkt

Der Skriptaufruf verändert das übergebene Bundle nicht. Er erstellt eine geprüfte
Arbeitskopie, signiert dort alle inhaltsbasiert erkannten PE-Dateien, baut daraus
das MSI und signiert erst danach das MSI.
Alle mit MSVC erzeugten Produkt-, Helper- und Testtargets verwenden die statische
Runtime (`/MT`, in Debug `/MTd`). Das ausgelieferte VST3 und der Updater setzen
daher auf einem sauberen Windows keine separat vorinstallierte Visual-C++-
Redistributable-Version voraus.

## Signierter Produktionsbuild

Die Standardeinstellung ist fail-closed: Ohne genau eine Zertifikatsauswahl und
ohne RFC 3161-Zeitstempel wird kein Kandidat erzeugt. Die folgenden Werte sind
absichtlich Platzhalter und keine mitgelieferten Zertifikate oder Dienste:

```powershell
$bundle = (Resolve-Path '<path-to-product.vst3>').Path
$hostTest = (Resolve-Path '<path-to-native-host-test.exe>').Path
$updater = (Resolve-Path "$bundle\Contents\Helpers\<Product>Updater.exe").Path

./scripts/build-windows-installer.ps1 `
    -Architecture x64 `
    -BundlePath $bundle `
    -Version '1.2.3' `
    -SourceCommit '<40-lowercase-hex-tag-commit>' `
    -UpdaterPath $updater `
    -ExpectedSignerSha256 '<64-hex-certificate-sha256-from-cmake>' `
    -CertificateThumbprint '<40-hex-certificate-thumbprint>' `
    -TimestampUrl 'https://<your-rfc3161-provider>' `
    -HostTestPath $hostTest
```

Statt `-CertificateThumbprint` kann genau einmal `-CertificateSubject` angegeben
werden. `-CertificateStoreName` ist standardmäßig `My`; Zertifikate aus dem
Computerkonto benötigen zusätzlich `-UseMachineCertificateStore`. Ein
Produktionspaket verlangt exakt `Contents\Helpers\<Product>Updater.exe` über
`-UpdaterPath` und bindet die Evidence über `-SourceCommit` exakt an den
40-stelligen, kleingeschriebenen Git-Commit des Release-Tags. Dessen in CMake
eingebrannter 64-stelliger Zertifikat-SHA-256
muss zusätzlich unverändert als `-ExpectedSignerSha256` übergeben werden. Das
Skript bricht ab, wenn der tatsächliche Fingerprint des ausgewählten Zertifikats
abweicht. Der Updater muss dieselbe x64- beziehungsweise ARM64EC-PE-Architektur
wie das Plug-in besitzen. Vor jeder Signatur führt das Skript den gestagten Helper
mit dessen rein lesendem `--validate-build-contract`-Modus aus. Dazu erzeugt es
für jeden Aufruf eine kryptografisch zufällige 32-Byte-Challenge und eine zufällige,
auf den aktuellen Benutzer beschränkte Named Pipe mit genau einer Instanz.
Packager und WIN32-Helper binden dabei beide Gegenstellen aneinander: Der Helper
prüft die PID des Pipe-Servers; der Packager liest die vom Betriebssystem gemeldete
Client-PID aus der verbundenen Pipe und verlangt exakt die PID des gerade von ihm
gestarteten Helpers. Erst danach liest er unabhängig von einer Konsole genau einen
kanonischen ASCII-/UTF-8-JSON-Datensatz.

Der Datensatz enthält Schema und Schemaversion, Challenge, Server-PID,
Produktions-/Compile-only-Modus sowie die vollständige kompilierte Identität:
Produkt, Version, Hersteller, GitHub-Owner und -Repository, Architektur, beide
UpgradeCodes und Signer-Pin. Verbindung, begrenztes Lesen und Prozessende teilen
sich ein 30-Sekunden-Limit; das Skript liest höchstens die erwartete Länge plus
ein Byte. Nur bytegenaue Übereinstimmung ohne BOM, Zusatz-Whitespace oder
Folgedaten **und** Exitcode 0 besteht. Ein bloßer Exitcode 0, Console-Ausgabe,
ein umbenannter Fremd-Helper, Testmodus oder der nur kompilierte CI-Shape reichen
damit nicht aus und werden vor der Paketierung abgelehnt.

Der Payload-Snapshot selbst ist die Autorität: Neben diesem exakten Helper darf
kein zweites updater-, upgrade-, setup-, installer-, bootstrap- oder
patch-artiges PE vorhanden sein. `-UpdaterPath` dient nur der zusätzlichen
expliziten Bindung und kann einen versteckten oder umbenannten Helper nicht aus
der Klassifikation herausnehmen.

Eine Subject-Auswahl muss exakt und eindeutig auf ein Zertifikat im gewählten
Store passen. Das Skript signiert anschließend immer über dessen aufgelösten
Thumbprint, verifiziert nach jeder Signatur denselben Signer und schreibt dessen
SHA-256-Zertifikatsfingerprint in die Evidence. Dadurch kann eine mehrdeutige
Subject-Suche nicht still ein anderes Zertifikat auswählen.

Für Windows on Arm wird derselbe Befehl nativ auf einem Arm64-System mit
`-Architecture arm64ec` ausgeführt. Im Produktionsmodus ist `-HostTestPath`
zwingend. Der Hosttest erhält den Pfad des
administrativ extrahierten Bundles als einziges Argument und muss mit Exitcode 0
enden. Dadurch prüft er das tatsächliche MSI-Payload, nicht den Build-Ordner.
`msiexec /a` und der Hosttest haben jeweils einen expliziten Timeout von 300
Sekunden. Für begründet langsamere Maschinen lassen sie sich mit
`-AdministrativeExtractionTimeoutSeconds` und `-HostTestTimeoutSeconds` auf
höchstens 3600 Sekunden erhöhen; ein Timeout beendet den gestarteten Prozessbaum
und verwirft den Kandidaten.

## Absichtlich unsignierter CI-/Testbuild

Nur für interne Tests darf die bewusste Ausnahme verwendet werden:

```powershell
./scripts/build-windows-installer.ps1 `
    -Architecture arm64ec `
    -BundlePath (Resolve-Path '<path-to-product.vst3>').Path `
    -Version '1.2.3' `
    -AllowUnsigned `
    -HostTestPath (Resolve-Path '<path-to-native-host-test.exe>').Path
```

`-AllowUnsigned` darf mit keiner Signing-Option kombiniert werden. Verzeichnis,
MSI-Dateiname und Evidence tragen zwingend
`UNSIGNED-NOT-FOR-DISTRIBUTION`. Solche Dateien dürfen nicht veröffentlicht oder
an Endnutzer verteilt werden. Eine bereits vorhandene Signatur einzelner
Eingabedateien macht den so markierten Pipeline-Lauf nicht zu einem
Distributionsartefakt.
Zusätzlich muss der tatsächliche Payload-Snapshot beweisen, dass das primäre
VST3-Binary das **einzige** PE im Bundle ist. Damit sind weder ein eingebetteter
Updater noch ein umbenannter oder verschobener PE-Helper möglich; diese Prüfung
hängt nicht von `-UpdaterPath` ab.

## Prüfungen und Evidence

Ein erfolgreicher Lauf führt vor der Ausgabe unter anderem diese Prüfungen aus:

1. Das lokale WiX-Manifest enthält ausschließlich WiX `6.0.2`; Restore und
   ausgeführte Version werden geprüft.
2. Bundle-Form und `moduleinfo.json` werden strukturell geprüft. Der Parser
   akzeptiert die von JUCE erzeugten abschließenden Kommata, lehnt aber Kommentare,
   ungültiges UTF-8, doppelte JSON-Schlüssel, falsche Typen und übertiefe Daten ab.
   Name, Version, Hersteller sowie die **exakte** Menge aus Prozessor- und
   Controller-Klasse einschließlich CID und Kategorie müssen mit Version und
   `package-config.json` übereinstimmen.
   Da diese JSON-Datei editierbar ist, wird zusätzlich die Windows-Versionresource
   des primären VST3-PE vor jeder Signatur geprüft: `FileVersion` und
   `ProductVersion` müssen exakt der Paketversion entsprechen, `CompanyName` dem
   Hersteller sowie `ProductName` und `FileDescription` dem Produkt. Drei
   Negativmutationen prüfen diese unabhängige Binary-Bindung bei jedem Lauf.
   Der eingebettete Updater besitzt eine eigene Windows-Versionresource. Dieselben
   Versions-, Hersteller- und Produktfelder sowie `InternalName`,
   `OriginalFilename` und die Beschreibung `<Product> Updater` werden vor und nach
   der Signatur geprüft; drei weitere Negativmutationen binden diese Identität.
3. Jede Datei wird inhaltsbasiert als PE oder Daten klassifiziert. Gefährliche
   Endungen und inkonsistente MZ/PE-Header werden abgelehnt; jedes PE wird auf x64
   beziehungsweise ARM64EC geprüft. Zwölf eingebettete Mutationstests verändern
   Identitätsfelder, CIDs/Kategorien, Klassenanzahl, Dateiendungen und versteckte
   Helper und müssen vor jedem Paketbau fail-closed anschlagen.
4. Das vollständige, sortierte Payload wird vor und nach Staging sowie während des
   WiX-Builds über Pfade, Größen und SHA-256-Hashes kontrolliert.
5. `wix msi validate` führt die Windows-Installer-ICE-Prüfung aus.
6. MSI-Tabellen, Summary-Architektur, eingebettetes Cabinet und
   Upgrade-/Downgrade-Regeln werden direkt kontrolliert. Der vollständige
   `Directory`-Graph muss genau einen `TARGETDIR`-Wurzelpfad besitzen und frei von
   Zyklen und verwaisten Eltern sein. Jede 64-Bit-Komponente muss unter
   `INSTALLFOLDER` liegen und jeder `File`-Eintrag auf eine bekannte Komponente
   zeigen. Side-Effect-Tabellen werden vollständig abgelehnt; dazu gehören unter
   anderem `CustomAction`, `Binary`, Service-, Registry-, `RemoveFile`-,
   `MoveFiles`-, `DuplicateFile`-, `Shortcut`- und Permission-Tabellen. In allen
   vorhandenen Install-, Admin- und Advertise-Sequenzen sind `ForceReboot`,
   `ScheduleReboot` und `DisableRollback` verboten.
   `ProductName`, `Manufacturer`, `ProductLanguage`,
   `MSIDEPLOYMENTCOMPLIANT=1`, die normalisierte Menge der beiden LaunchConditions
   und `SecureCustomProperties` als exakt die drei Upgrade-Erkennungsvariablen
   werden dabei genauso streng geprüft wie im nativen Updater.
7. `msiexec /a` extrahiert das MSI administrativ. Zulässig sind ausschließlich
   die nötigen Vorfahren des exakt benannten Produkt-Bundles, dessen kompletter
   Baum und optional genau eine gleichnamige administrative MSI-Datei direkt im
   Extraktionswurzelverzeichnis. Jede fremde Datei oder jedes fremde Verzeichnis
   bricht den Build ab; anschließend muss der Bundle-Baum bytegenau mit der
   signierten Paketquelle übereinstimmen.
8. Falls angegeben – im Produktionsmodus zwingend –, lädt und prüft
   `-HostTestPath` dieses extrahierte Bundle.

Das Resultat ist ein neues, atomar veröffentlichtes Kandidatenverzeichnis unter
`dist\windows` (oder `-OutputDirectory`). Es enthält genau das MSI und eine
`*.evidence.json` mit MSI-Hash, vollständiger Payload-Hashliste, Produkt- und
UpgradeCodes sowie expliziten Ergebnissen für Graph-, Referenz-, Side-Effect-,
Sequenz- und Extraktionslayout-Prüfung. Schema 2 protokolliert zusätzlich den
`moduleinfo.json`-Hash und seine gebundene Identität, die fünf geprüften Felder der
Plugin- und Updater-PE-Versionresources samt Mutationstestzahlen, sämtliche erkannten PE-Pfade,
Updater-/Helper-Klassifikation, tatsächlich signierte PE-Pfade, den exakten
MSI-Produkt-/Hersteller-/Sprach-/UAC-Vertrag, sichere Upgrade-Properties,
den architekturgenauen Anzeigenamen `<Product> VST3 - Windows x64` oder
`<Product> VST3 - Windows on Arm (ARM64EC)`, die beiden LaunchConditions und die Zahl
der bestandenen Policy-Mutationstests. Ein bestehender Kandidat wird nie
überschrieben. Der Build führt keine Veröffentlichung durch.

Signierte MSI-Dateien heißen
`<Product>-<Version>-Windows-x64.msi` beziehungsweise
`<Product>-<Version>-Windows-arm64ec.msi`. Testkandidaten fügen unmittelbar vor
`.msi` den Marker `-UNSIGNED-NOT-FOR-DISTRIBUTION` ein; das umgebende
Verzeichnis verwendet denselben Basenamen.

## Upgrades und Rollback

Die UpgradeCodes in `package-config.json` sind veröffentlichte Produktidentitäten
und dürfen nach dem ersten Release nicht geändert werden. Der ProductCode wird
stabil aus Produkt, Architektur und der kanonischen dreiteiligen MSI-Version
abgeleitet; jede neue Version erhält einen anderen ProductCode. Dieselbe Version
darf deshalb nicht mit verändertem Inhalt erneut veröffentlicht werden.

Downgrades und gleichversionige Major Upgrades sind gesperrt. `MajorUpgrade` plant
`RemoveExistingProducts` mit `afterInstallInitialize`: Die alte Version wird früh
entfernt, befindet sich aber bereits in der Windows-Installer-Transaktion und wird
bei einem Fehler wiederhergestellt. Diese frühe, rollback-sichere Planung erlaubt
auch, dass sich der automatisch geerntete Dateibaum zwischen Versionen ändert.

Die resultierende WiX-6.0.2-`Upgrade`-Tabelle wird als exakter Vertrag geprüft:
`WIX_UPGRADE_DETECTED` hat kein Minimum, die aktuelle Version als exklusives
Maximum, Sprache 1033 und Attribute `0x1`; `WIX_DOWNGRADE_DETECTED` hat die
aktuelle Version als exklusives Minimum, kein Maximum, Sprache 1033 und Attribute
`0x2`; `OTHERARCHITECTUREDETECTED` hat Minimum 0.0.0 inklusiv, kein Maximum,
keine Sprachbindung und Attribute `0x102`. Weitere oder doppelte Upgrade-Zeilen
werden abgelehnt.

## Werkzeug-Lock und WiX-Lizenz

`.config/dotnet-tools.json` pinnt die lokale CLI exakt auf WiX `6.0.2`. Das Skript
verwendet ausschließlich dieses Manifest und `Installer/Windows/NuGet.Config`,
deaktiviert Parallelität und Cache-Nutzung, verlangt eine signierte NuGet-Package
von FireGiant und verifiziert anschließend die ausgeführte Version. `dotnet tool
restore` besitzt keinen `--locked-mode` für Projekt-Lockdateien; bei lokalen Tools
ist das exakte Toolmanifest der Lock-Vertrag. Eine Versionsspanne oder ein globales
`wix` wird nicht akzeptiert.

WiX 6 unterliegt zusätzlich zur Open-Source-Lizenz dem Programm zur
**Open Source Maintenance Fee (OSMF)**. Nach den veröffentlichten Bedingungen
benötigen Organisationen oberhalb der dort genannten Umsatzschwelle eine passende
FireGiant-Sponsorschaft. WiX 6 erzwingt die EULA noch nicht technisch, die
vertragliche Pflicht kann trotzdem bestehen. Vor einem kommerziellen Build sind
die jeweils aktuellen [WiX-OSMF-Bedingungen](https://docs.firegiant.com/wix/osmf/)
und die [WiX-Lizenzinformationen](https://docs.firegiant.com/wix/) durch die
verantwortliche Organisation zu prüfen. Dieses Repository erteilt keine Lizenz.

## CMake-/CI-Integration

Die CMake-Konfiguration und der Workflow integrieren diese Schicht wie folgt:

1. In jedem Windows-Job zuerst VST3, `moduleinfo.json` und den nativen Hosttest
   vollständig bauen und testen.
2. Auf x64 `-Architecture x64`, auf dem nativen Windows-Arm-Runner
   `-Architecture arm64ec` ausführen. Build-/Tool-Caches und Artefaktnamen müssen
   Betriebssystem und Architektur enthalten.
3. Normale CI-Checks verwenden ausschließlich `-AllowUnsigned`. Ein
   Release-Job muss Signing-Identität und RFC-3161-URL aus geschützten Secrets
   übergeben und darf die Unsigned-Ausnahme nicht setzen.
4. Das gesamte Kandidatenverzeichnis einschließlich Evidence als separates x64-
   beziehungsweise ARM64EC-Artefakt hochladen. Niemals nur die MSI-Datei aus der
   Evidence herauslösen.
5. Den statischen Vertrag auf allen Plattformen mit
   `python3 -m unittest scripts/tests/test_windows_installer_contract.py` prüfen.

Das oben genannte Kandidatenverzeichnis ist das CI-Artefakt. Ein späterer,
separat freigegebener Release-Schritt hängt daraus die **signierte** MSI unter
ihrem unveränderten kanonischen Dateinamen und die zugehörige Evidence als zwei
Release-Assets an; nur so findet der native Updater die MSI. Ein unsignierter
Kandidat darf diesen Schritt nie erreichen.

Der konkrete, taggebundene GitHub-Actions-Vertrag einschließlich kurzlebigem
PFX-Import, Architektur-Jobs, Evidence-Revalidierung und atomarem Draft-Publish
steht in [WINDOWS_RELEASE.md](WINDOWS_RELEASE.md).

Die ICE-Validierung benötigt einen Windows-Installer-fähigen Benutzerkontext.
Wenn ein nichtinteraktiver Runner das verhindert, ist ein administrativer
self-hosted Release-Runner zu verwenden; die Produktionsprüfung darf nicht still
unterdrückt werden.
