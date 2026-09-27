# Third-party notices and source licensing

Copyright (c) 2026 Whykiki Audio. The original plugin, updater, installer and build
code and the 64 original factory presets are licensed under **AGPL-3.0-only**.
The project owner has confirmed the rights to license that original code and
those presets on these terms. This statement does not transfer rights in
third-party material or relicense any dependency.

The release archive contains the exact original JUCE 8.0.15 tree, commit
`91ad83ae34a81e0833b1a2b0866f54846370ae53`. The AGPLv3 option is used for JUCE
modules; no commercial JUCE entitlement is claimed. See
[`external/JUCE/LICENSE.md`](external/JUCE/LICENSE.md) in the complete source
archive and [the pinned upstream license](https://github.com/juce-framework/JUCE/blob/91ad83ae34a81e0833b1a2b0866f54846370ae53/LICENSE.md).
All upstream file headers and license texts are retained verbatim.

JUCE's tree also contains separately licensed dependencies. Its upstream inventory
covers AudioUnitSDK and Oboe (Apache-2.0); FLAC and Ogg Vorbis (BSD);
GLEW (BSD), Mesa and Khronos (MIT); Independent JPEG Group code; CHOC and LV2
(ISC); QuickJS and VST3 SDK (MIT); pslextensions (public domain); Box2D, pnglib and
zlib (zlib); HarfBuzz (upstream MIT-style terms); SheenBidi (Apache-2.0);
AAX and ASIO SDKs (their respective upstream proprietary/GPL terms). JUCE
examples, tools and build helpers have additional upstream notices, including
ISC, zlib, MIT and Apache-2.0. Consult the actual upstream notices for the precise
terms; this summary does not replace them.

Only VST3 plugins are built. Shipping the full dependency source does not enable
AAX, ASIO or Audio Unit product formats or establish permission to distribute
binaries in those formats. The SPDX 2.3 inventory records every source file and
its digest; vendor license conclusions are `NOASSERTION` because each upstream
license remains authoritative.

WiX 6 is a build tool, not a bundled runtime dependency. Its Open Source
Maintenance Fee terms require the appropriate FireGiant sponsorship for an
organisation with more than USD 10,000 annual revenue, subject to the current
terms and exemptions. The release owner must establish the applicable status
before distribution; source availability alone is not evidence of that status.
See [WiX OSMF](https://docs.firegiant.com/wix/osmf/).

`SOURCE-MANIFEST.json` and `SBOM.spdx.json` are generated into each complete
source release. The release's checksums bind that archive to the distributed
binaries. The manifest verifies correspondence, not independent authenticity;
obtain the archive from the same authenticated release as the binaries.
