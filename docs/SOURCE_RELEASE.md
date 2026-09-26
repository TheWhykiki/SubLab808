# Complete corresponding-source releases

The product configuration in `release/product.json` is the source of identity,
version, JUCE pin and VST3/MSI compatibility identifiers. Run
`python3 scripts/release_contract.py --other ../<other-plugin>` to verify that
the shared security and distribution files have not drifted across repositories.
`release/shared-files.json` explicitly lists byte-identical files. The complete
signed-release workflow additionally permits only product name, uppercase product
prefix and the two configured UpgradeCodes to differ. The native transition
helper permits only the exact product name to differ. All other workflow/helper
bytes, including approval guards and commands, must match; nothing is stripped.

Generate a source archive only from a clean, committed checkout and clean JUCE
checkout at the configured revision:

```sh
python3 scripts/source-release.py create --repo . --juce /path/to/JUCE \
    --output /path/out/Product-Version-Source.zip
python3 scripts/source-release.py verify --archive /path/out/Product-Version-Source.zip
```

Use the actual product name and version in the output filename. The archive has
one `Product-Version-Source/` root. It contains original tracked build inputs,
presets, tests, release scripts, notices and the complete pinned JUCE tree at
`external/JUCE/`. It excludes Git metadata, untracked files, build outputs and
non-build design-reference images; excluded tracked paths are recorded.
Credentials, private keys, symlinks, nested submodules, unsafe filenames and
case-insensitive collisions are rejected. No existing output is overwritten.

The stored ZIP is deterministic across platforms: sorted entries, fixed epoch,
stable executable modes, no compression-library variation. Its
`SOURCE-MANIFEST.json` binds all source bytes and `SBOM.spdx.json` to the exact
product/JUCE commits. The SPDX 2.3 SBOM lists file checksums and preserved vendor
license boundaries. It does not infer licensing permission from a checksum.

Extract using a normal ZIP tool, then verify before building:

```sh
python3 scripts/source-release.py verify-tree --root .
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

These commands do not require Git or a network fetch of JUCE. On Windows add the
Visual Studio generator and `-A x64` or `-A ARM64EC`; ARM64EC must build/run on
native Windows on Arm. Signing credentials are intentionally absent, so this
rebuild produces local unsigned/ad-hoc candidates, not signed release replicas.
Interactive dialog tests need a desktop session. Platform SDKs, CMake and the
native compiler must already be installed.

On macOS the verified Git-free source directory also supports
`sh scripts/package-release.sh Release`: it snapshots the manifest-bound sources,
rebuilds/tests and emits an ad-hoc VST3 ZIP plus an unsigned local PKG candidate.
It preserves the original product/JUCE source commits in the candidate evidence.
Modified or unlisted source files are rejected. Signed/notarized distribution
packaging requires the clean Git release checkout, identities and protected
release workflow; it is not enabled by the source archive alone.

Archive verification is structural and content-based, not a replacement for
checking the authenticated release checksum. A source manifest can be modified
by anyone who can replace the entire archive.
