#!/usr/bin/env python3
"""Fresh-build, test and atomically publish a source-bound macOS candidate.

Existing build/dist artifacts are never trusted or overwritten. Compilation
uses a source snapshot including initialized submodules and dirty source files.
Developer-ID signing and notarization require real caller-provided identities.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile


class ReleaseError(RuntimeError):
    pass


def run(command, *, cwd=None, capture=False):
    return subprocess.run([str(part) for part in command], cwd=cwd, check=True,
                          text=True, capture_output=capture)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    return digest(path.read_bytes())


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")


def git(root, *arguments):
    return subprocess.check_output(["git", "-C", str(root), *arguments]).decode()


def source_inputs(root):
    """Exact tracked bytes, plus new files in source/test/build-script dirs.

    Generated build/dist trees are not source inputs. Submodules are traversed
    from their actual checked-out commits, not merely the parent gitlink.
    """
    files, repositories = {}, []

    def collect(repo, prefix):
        repositories.append({"path": prefix or ".", "commit": git(repo, "rev-parse", "HEAD").strip(),
                             "dirty": bool(git(repo, "status", "--porcelain", "--untracked-files=normal").strip())})
        tracked = git(repo, "ls-files", "--cached", "-z").split("\0")
        additional = git(repo, "ls-files", "--others", "--exclude-standard", "-z", "--",
                         "Source", "Tests", "scripts", "cmake", ".github", "CMakeLists.txt",
                         "Presets", "Resources", "Assets", "Updater").split("\0")
        for name in sorted(set(tracked + additional) - {""}):
            relative = Path(name)
            if relative.is_absolute() or ".." in relative.parts:
                raise ReleaseError("Unsafe source path")
            path = repo / relative
            key = (Path(prefix) / relative).as_posix()
            if path.is_symlink():
                raise ReleaseError(f"Source symlinks are not supported: {key}")
            if not path.exists():
                continue  # Tracked deletion is part of the dirty snapshot.
            if path.is_dir():
                if not (path / ".git").exists():
                    raise ReleaseError(f"Initialize the submodule before packaging: {key}")
                collect(path, key)
            else:
                files[key] = (path.read_bytes(), 0o755 if path.stat().st_mode & 0o111 else 0o644)

    collect(root, "")
    records = [{"path": name, "sha256": digest(data), "mode": oct(mode)}
               for name, (data, mode) in sorted(files.items())]
    manifest = {"schema": 1, "repositories": repositories, "files": records,
                "source_sha256": digest(json.dumps(records, sort_keys=True).encode())}
    return files, manifest


def snapshot_source(root, destination):
    files, manifest = source_inputs(root)
    _, confirmation = source_inputs(root)
    if manifest != confirmation:
        raise ReleaseError("Source changed while creating the snapshot; retry when edits finish")
    destination.mkdir()
    for name, (data, mode) in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
    return manifest


def validate_options(root, configuration, version_override, environment):
    if configuration != "Release":
        raise ReleaseError("Distribution packaging accepts only the Release configuration")
    match = re.search(r"^project\(SubLab808 VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b",
                      (root / "CMakeLists.txt").read_text(), re.MULTILINE)
    if not match:
        raise ReleaseError("Cannot determine the CMake project version")
    version = match.group(1)
    if version_override and version_override != version:
        raise ReleaseError("Requested version must match CMakeLists.txt; change the project version first")
    app = environment.get("SUBLAB808_APPLICATION_IDENTITY", "").strip()
    installer = environment.get("SUBLAB808_INSTALLER_IDENTITY", "").strip()
    notary = environment.get("SUBLAB808_NOTARY_PROFILE", "").strip()
    if bool(app) != bool(installer) or (notary and not (app and installer)):
        raise ReleaseError("Signed/notarized releases require both application and installer identities")
    if app == "-" or installer == "-":
        raise ReleaseError("For ad-hoc builds leave both identities unset; '-' is not a distribution identity")
    jobs = environment.get("SUBLAB808_BUILD_JOBS", "2")
    if not jobs.isdigit() or not 1 <= int(jobs) <= 64:
        raise ReleaseError("SUBLAB808_BUILD_JOBS must be an integer between 1 and 64")
    if (root / "dist").is_symlink():
        raise ReleaseError("Refusing a symlinked dist directory")
    return version, app, installer, notary, jobs


def validate_bundle(bundle, version, runner):
    with (bundle / "Contents/Info.plist").open("rb") as source:
        metadata = plistlib.load(source)
    if metadata.get("CFBundleShortVersionString") != version:
        raise ReleaseError("Built bundle version does not match the source snapshot")
    binary = bundle / "Contents/MacOS/SubLab808"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ReleaseError("Missing or non-executable VST3 binary")
    architectures = runner(["lipo", "-archs", binary], capture=True).stdout.split()
    if set(architectures) != {"arm64"}:
        raise ReleaseError("Release VST3 must contain exactly arm64")
    for architecture in architectures:
        output = runner(["xcrun", "vtool", "-arch", architecture, "-show-build", binary], capture=True).stdout
        if re.findall(r"\bminos\s+(\S+)", output) != ["11.0"]:
            raise ReleaseError(f"Unexpected deployment target for {architecture}")
    runner(["codesign", "--verify", "--deep", "--strict", bundle])
    helper = bundle / "Contents/Helpers/SubLab808Updater.app"
    with (helper / "Contents/Info.plist").open("rb") as source:
        updater_info = plistlib.load(source)
    if (updater_info.get("CFBundleShortVersionString") != version
            or updater_info.get("WKProduct") != "SubLab808"):
        raise ReleaseError("Embedded updater version/product does not match the plugin")
    helper_binary = helper / "Contents/MacOS/SubLab808Updater"
    if not os.access(helper_binary, os.X_OK):
        raise ReleaseError("Missing executable updater")
    if set(runner(["lipo", "-archs", helper_binary], capture=True).stdout.split()) != {"arm64"}:
        raise ReleaseError("Embedded updater has incorrect architectures")
    return file_hash(binary)


def publish_candidate(artifacts, dist, name):
    """A completed directory is moved into dist on the same filesystem."""
    if dist.is_symlink():
        raise ReleaseError("Refusing a dist symlink created during the build")
    dist.mkdir(exist_ok=True)
    lock = dist / ".release-publish.lock"
    try:
        handle = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError as error:
        raise ReleaseError("Another publication is active (or its lock needs inspection)") from error
    try:
        os.close(handle)
        destination = dist / name
        if destination.exists() or destination.is_symlink():
            raise ReleaseError(f"Candidate already exists; nothing overwritten: {destination}")
        artifacts.rename(destination)
        return destination
    finally:
        lock.unlink()


def save_failure_evidence(stage, context, error):
    """Keep diagnostics, never a failed package, after temporary build cleanup."""
    if stage.parent.is_symlink():
        raise ReleaseError("Refusing a symlinked diagnostics destination")
    destination = Path(tempfile.mkdtemp(prefix="failed-run-", dir=stage.parent))
    provenance = context.get("provenance")
    external = isinstance(error, subprocess.CalledProcessError)
    command = error.cmd if external else None
    tool = Path(str(command[0])).name if isinstance(command, (list, tuple)) and command else None
    details = {"schema": 1, "status": "failed", "step": context["step"],
               "error_type": type(error).__name__, "tool": tool,
               "exit_code": error.returncode if external else None,
               "commit": provenance["repositories"][0]["commit"] if provenance else None,
               "source_sha256": provenance["source_sha256"] if provenance else None,
               "artifacts": {}, "unavailable_reports": []}
    # Mark the folder failed immediately, even if a later diagnostic copy fails.
    # Do not persist environment variables, signing arguments, or notary credentials.
    write_json(destination / "failure.json", details)
    if provenance:
        write_json(destination / "source-manifest.json", provenance)
    reports = {"ctest-results.xml": stage / "ctest-results.xml",
               "CTest-LastTest.log": stage / "build/Testing/Temporary/LastTest.log",
               "CTest-LastTestsFailed.log": stage / "build/Testing/Temporary/LastTestsFailed.log",
               "CMakeConfigureLog.yaml": stage / "build/CMakeFiles/CMakeConfigureLog.yaml",
               "CMakeError.log": stage / "build/CMakeFiles/CMakeError.log"}
    for name, source in reports.items():
        if not source.is_file():
            continue  # An earlier failure may never have reached CMake/CTest.
        try:
            shutil.copy2(source, destination / name)
        except OSError:
            details["unavailable_reports"].append(name)
    # Captured errors (e.g. the isolated recipe checker) have no CTest log yet.
    if external:
        for name, content in (("command-stdout.log", error.stdout), ("command-stderr.log", error.stderr)):
            if content:
                (destination / name).write_bytes(content.encode() if isinstance(content, str) else content)
    details["artifacts"] = {path.name: file_hash(path) for path in sorted(destination.iterdir())
                            if path.is_file() and path.name != "failure.json"}
    write_json(destination / "failure.json", details)
    print(f"Failed-run diagnostics (not a release): {destination}", file=sys.stderr)
    return destination


@contextmanager
def retain_failure_evidence(stage):
    context = {"step": "source_snapshot"}
    try:
        yield context
    except (Exception, KeyboardInterrupt) as error:
        try:
            save_failure_evidence(stage, context, error)
        except Exception as diagnostic_error:
            # A full disk or unreadable diagnostic must not hide the original failure.
            print(f"Could not retain failed-run diagnostics: {type(diagnostic_error).__name__}", file=sys.stderr)
        raise


def package_release(root, configuration="Release", version_override=None, environment=None, runner=run):
    root = Path(root).resolve()
    environment = os.environ if environment is None else environment
    version, app, installer, notary, jobs = validate_options(root, configuration, version_override, environment)
    if platform.system() != "Darwin":
        raise ReleaseError("macOS distribution tools are required")
    for tool in ("cmake", "ctest", "codesign", "ditto", "pkgbuild", "pkgutil", "lipo", "xcrun"):
        if not shutil.which(tool):
            raise ReleaseError(f"Missing required release tool: {tool}")
    dist = root / "dist"
    dist.mkdir(exist_ok=True)
    # dist is git-ignored, so a concurrent status/add cannot accidentally pick up
    # the multi-gigabyte temporary source/build tree. Publication stays same-volume.
    with tempfile.TemporaryDirectory(prefix=".release-candidate-", dir=dist) as temporary, \
            retain_failure_evidence(Path(temporary)) as diagnostics:
        stage = Path(temporary)
        source = stage / "source"
        provenance = snapshot_source(root, source)
        diagnostics["provenance"] = provenance
        if validate_options(source, configuration, version_override, environment)[0] != version:
            raise ReleaseError("Project version changed while taking the source snapshot")
        # Compiled preset tests cannot detect JSON edits absent from FactoryBank.h.
        # Check the immutable snapshot, not the live checkout, before any build.
        # Ignore PYTHONOPTIMIZE so the generator's assertions cannot be disabled.
        diagnostics["step"] = "verify_factory_bank"
        runner([sys.executable, "-I", source / "scripts/generate-presets.py", "--check"])
        build = stage / "build"
        diagnostics["step"] = "configure"
        runner(["cmake", "-S", source, "-B", build, "-G", "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Release"])
        # Build the default target so every registered CTest executable is built,
        # including future preset/parameter tests added without editing this script.
        diagnostics["step"] = "build"
        runner(["cmake", "--build", build, "-j", jobs])
        test_report = stage / "ctest-results.xml"
        diagnostics["step"] = "ctest"
        runner(["ctest", "--test-dir", build, "--output-on-failure", "--no-tests=error",
                "--output-junit", test_report])
        bundle = build / "SubLab808_artefacts/Release/VST3/SubLab808.vst3"
        diagnostics["step"] = "validate_bundle"
        built_hash = validate_bundle(bundle, version, runner)
        payload = stage / "payload"
        staged_bundle = payload / "Library/Audio/Plug-Ins/VST3/SubLab808.vst3"
        staged_bundle.parent.mkdir(parents=True)
        diagnostics["step"] = "stage_bundle"
        runner(["ditto", bundle, staged_bundle])
        diagnostics["step"] = "sign_bundle"
        if app:
            runner(["codesign", "--force", "--options", "runtime", "--timestamp", "--sign", app,
                    staged_bundle / "Contents/Helpers/SubLab808Updater.app"])
            runner(["codesign", "--force", "--deep", "--options", "runtime", "--timestamp", "--sign", app, staged_bundle])
        else:
            print("Warning: ad-hoc VST3 and unsigned installer; no notarization claim.")
            runner(["codesign", "--force", "--deep", "--sign", "-", staged_bundle])
        packaged_hash = validate_bundle(staged_bundle, version, runner)
        artifacts = stage / "artifacts"
        artifacts.mkdir()
        # Preserve auditable results before the temporary source/build is removed.
        # Missing reports are an error, never an unsubstantiated test-success claim.
        shutil.copy2(test_report, artifacts / "ctest-results.xml")
        shutil.copy2(build / "Testing/Temporary/LastTest.log", artifacts / "CTest-LastTest.log")
        package = artifacts / f"SubLab808-{version}-macOS-arm64.pkg"
        components = stage / "components.plist"
        components.write_bytes(plistlib.dumps([{
            "RootRelativeBundlePath": "Library/Audio/Plug-Ins/VST3/SubLab808.vst3",
            "BundleIsRelocatable": False, "BundleIsVersionChecked": True,
            "BundleHasStrictIdentifier": True, "BundleOverwriteAction": "upgrade"}]))
        command = ["pkgbuild", "--root", payload, "--identifier", "audio.whykiki.sublab808.pkg",
                   "--version", version, "--install-location", "/", "--component-plist", components]
        if installer:
            command += ["--sign", installer]
        diagnostics["step"] = "pkgbuild"
        runner(command + [package])
        if notary:
            diagnostics["step"] = "notarize"
            response = runner(["xcrun", "notarytool", "submit", package, "--keychain-profile", notary,
                               "--wait", "--output-format", "json"], capture=True)
            if json.loads(response.stdout).get("status") != "Accepted":
                raise ReleaseError("Notarization was not accepted; existing releases are unchanged")
            for target in (package, staged_bundle):
                runner(["xcrun", "stapler", "staple", target])
                runner(["xcrun", "stapler", "validate", target])
        archive = artifacts / f"SubLab808-{version}-macOS-arm64-VST3.zip"
        diagnostics["step"] = "zip_roundtrip"
        runner(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", staged_bundle, archive])
        roundtrip = stage / "zip-roundtrip"
        runner(["ditto", "-x", "-k", archive, roundtrip])
        restored = roundtrip / "SubLab808.vst3"
        if validate_bundle(restored, version, runner) != packaged_hash:
            raise ReleaseError("ZIP-restored binary differs from the signed candidate")
        host = build / "SubLab808HostTests_artefacts/Release/SubLab808HostTests"
        diagnostics["step"] = "zip_host"
        runner([host, restored])
        expanded = stage / "package-roundtrip"
        diagnostics["step"] = "installer_roundtrip"
        runner(["pkgutil", "--expand-full", package, expanded])
        installed = expanded / "Payload/Library/Audio/Plug-Ins/VST3/SubLab808.vst3"
        if validate_bundle(installed, version, runner) != packaged_hash:
            raise ReleaseError("Installer payload differs from the signed candidate")
        diagnostics["step"] = "installer_host"
        runner([host, installed])
        diagnostics["step"] = "write_manifests"
        write_json(artifacts / "source-manifest.json", provenance)
        manifest = {"schema": 1, "version": version, "configuration": configuration,
                    "commit": provenance["repositories"][0]["commit"],
                    "source_sha256": provenance["source_sha256"],
                    "dirty": any(repo["dirty"] for repo in provenance["repositories"]),
                    "built_binary_sha256": built_hash, "packaged_binary_sha256": packaged_hash,
                    "application_signed": bool(app), "installer_signed": bool(installer), "notarized": bool(notary),
                    "verification": ["generated_factory_bank_matches_snapshot", "fresh_snapshot_build", "ctest", "zip_host_state_audio_roundtrip",
                                     "installer_payload_host_state_audio_roundtrip", "arm64_macos11_slice"],
                    "artifacts": {path.name: file_hash(path) for path in sorted(artifacts.iterdir())}}
        write_json(artifacts / "release-manifest.json", manifest)
        checksums = "".join(f"{file_hash(path)}  {path.name}\n" for path in sorted(artifacts.iterdir()))
        (artifacts / "SHA256SUMS.txt").write_text(checksums)
        name = (f"SubLab808-{version}-{manifest['commit'][:12]}-"
                f"{provenance['source_sha256'][:16]}-{packaged_hash[:12]}")
        diagnostics["step"] = "publish"
        destination = publish_candidate(artifacts, dist, name)
    print(f"Validated release candidate: {destination}")
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("configuration", nargs="?", default="Release")
    parser.add_argument("version", nargs="?")
    options = parser.parse_args()
    try:
        package_release(Path(__file__).resolve().parent.parent, options.configuration, options.version)
    except (ReleaseError, subprocess.CalledProcessError, OSError, ValueError) as error:
        parser.exit(1, f"Release failed without replacing existing artifacts: {error}\n")


if __name__ == "__main__":
    main()
