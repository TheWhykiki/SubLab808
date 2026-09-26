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
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_contract import ContractError, load_product, verify_product_projection

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


def write_process_log(path, result):
    """Persist tool output without recording command arguments or environment values."""
    chunks = []
    for value in (getattr(result, "stdout", None), getattr(result, "stderr", None)):
        if value:
            chunks.append(value.decode() if isinstance(value, bytes) else value)
    path.write_text("".join(chunks) or "(no output)\n")


def run_and_log(command, destination, runner):
    try:
        result = runner(command, capture=True)
    except subprocess.CalledProcessError as error:
        write_process_log(destination, error)
        raise
    write_process_log(destination, result)
    return result


def read_json_object(raw, description):
    try:
        value = json.loads(raw)
    except (json.JSONDecodeError, TypeError) as error:
        raise ReleaseError(f"{description} was not valid JSON") from error
    if not isinstance(value, dict):
        raise ReleaseError(f"{description} was not a JSON object")
    return value


def read_uuid(value, description):
    if not isinstance(value, str):
        raise ReleaseError(f"{description} did not contain a UUID")
    try:
        return uuid.UUID(value)
    except ValueError as error:
        raise ReleaseError(f"{description} did not contain a valid UUID") from error


def validate_notary_log(raw, submission_id):
    log = read_json_object(raw, "Notarization log")
    if read_uuid(log.get("jobId"), "Notarization log") != submission_id:
        raise ReleaseError("Notarization log did not match the submitted job")
    if log.get("status") != "Accepted" or type(log.get("statusCode")) is not int or log["statusCode"] != 0:
        raise ReleaseError("Notarization log did not report an accepted job")
    if "issues" not in log:
        raise ReleaseError("Notarization log did not contain an issues field")
    issues = log["issues"]
    if issues is not None and not isinstance(issues, list):
        raise ReleaseError("Notarization log issues must be null or a list")
    if issues is not None:
        if any(not isinstance(issue, dict) for issue in issues):
            raise ReleaseError("Notarization log contained a malformed issue")
        if any(str(issue.get("severity", "")).casefold() == "error" for issue in issues):
            raise ReleaseError("Notarization log contained an error issue")
    return log


def git(root, *arguments):
    return subprocess.check_output(["git", "-C", str(root), *arguments]).decode()


def source_inputs(root):
    """Exact tracked bytes, plus new files in source/test/build-script dirs.

    Generated build/dist trees are not source inputs. Submodules are traversed
    from their actual checked-out commits, not merely the parent gitlink.
    """
    files, repositories = {}, []
    if not (root / ".git").exists():
        # Corresponding-source ZIPs deliberately contain no Git internals. Their
        # complete manifest replaces local Git metadata for unsigned rebuilds.
        from source_release import MANIFEST, SourceError, strict_json, verify_tree
        try:
            archive_manifest = verify_tree(root)
            for entry in archive_manifest["files"]:
                data = (root / entry["path"]).read_bytes()
                if len(data) != entry["size"] or digest(data) != entry["sha256"]:
                    raise ReleaseError("Source archive changed while preparing the snapshot")
                files[entry["path"]] = (data, entry["mode"])
            manifest_bytes = (root / MANIFEST).read_bytes()
            if strict_json(manifest_bytes) != archive_manifest:
                raise ReleaseError("Source manifest changed while preparing the snapshot")
            files[MANIFEST] = (manifest_bytes, 0o644)
        except (SourceError, ContractError, OSError, KeyError) as error:
            raise ReleaseError(f"A gitless build requires an intact corresponding-source archive: {error}") from error
        repositories = [{"path": ".", "commit": archive_manifest["commit"], "dirty": False},
                        {"path": "external/JUCE", "commit": archive_manifest["juceCommit"], "dirty": False}]
        records = [{"path": name, "sha256": digest(data), "mode": oct(mode)}
                   for name, (data, mode) in sorted(files.items())]
        return files, {"schema": 1, "repositories": repositories, "files": records,
                       "source_sha256": digest(json.dumps(records, sort_keys=True).encode()),
                       "origin": "verified-source-archive", "archive_source_sha256": archive_manifest["sourceSha256"]}

    def collect(repo, prefix):
        repositories.append({"path": prefix or ".", "commit": git(repo, "rev-parse", "HEAD").strip(),
                             "dirty": bool(git(repo, "status", "--porcelain", "--untracked-files=normal").strip())})
        tracked = git(repo, "ls-files", "--cached", "-z").split("\0")
        additional = git(repo, "ls-files", "--others", "--exclude-standard", "-z", "--",
                         "Source", "Tests", "scripts", "cmake", ".github", "CMakeLists.txt",
                         "Presets", "Resources", "Assets", "Updater", "release", "Installer").split("\0")
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
    try:
        product = verify_product_projection(root)
    except (ContractError, OSError) as error:
        raise ReleaseError(str(error)) from error
    version = product["version"]
    prefix = product["productName"].upper()
    if version_override and version_override != version:
        raise ReleaseError("Requested version must match CMakeLists.txt; change the project version first")
    app = environment.get(prefix + "_APPLICATION_IDENTITY", "").strip()
    installer = environment.get(prefix + "_INSTALLER_IDENTITY", "").strip()
    notary = environment.get(prefix + "_NOTARY_PROFILE", "").strip()
    notary_keychain_value = environment.get(prefix + "_NOTARY_KEYCHAIN", "").strip()
    if bool(app) != bool(installer) or (notary and not (app and installer)):
        raise ReleaseError("Signed/notarized releases require both application and installer identities")
    if bool(notary) != bool(notary_keychain_value):
        raise ReleaseError("Notarization requires both a profile and its explicit keychain path")
    if app == "-" or installer == "-":
        raise ReleaseError("For ad-hoc builds leave both identities unset; '-' is not a distribution identity")
    signer_configuration(environment, required=bool(app or installer))
    notary_keychain = None
    if notary_keychain_value:
        candidate_keychain = Path(notary_keychain_value)
        if (not candidate_keychain.is_absolute() or candidate_keychain.is_symlink()
                or not candidate_keychain.is_file()):
            raise ReleaseError(prefix + "_NOTARY_KEYCHAIN must be an existing absolute regular file")
        notary_keychain = str(candidate_keychain.resolve(strict=True))
    jobs = environment.get(prefix + "_BUILD_JOBS", "2")
    if not jobs.isdigit() or not 1 <= int(jobs) <= 64:
        raise ReleaseError(prefix + "_BUILD_JOBS must be an integer between 1 and 64")
    if (root / "dist").is_symlink():
        raise ReleaseError("Refusing a symlinked dist directory")
    return version, app, installer, notary, notary_keychain, jobs


def signer_configuration(environment, *, required=False):
    variables = ("MACOS_DEVELOPER_ID_APPLICATION_CERT_SHA256", "MACOS_DEVELOPER_ID_INSTALLER_CERT_SHA256",
                 "MACOS_NEXT_DEVELOPER_ID_APPLICATION_CERT_SHA256", "MACOS_NEXT_DEVELOPER_ID_INSTALLER_CERT_SHA256")
    pins = [environment.get(variable, "").lower() for variable in variables]
    if any(value and not re.fullmatch(r"[0-9a-f]{64}", value) for value in pins):
        raise ReleaseError("macOS certificate pins must be exactly 64 hexadecimal characters")
    app, installer, next_app, next_installer = pins
    if bool(app) != bool(installer) or bool(next_app) != bool(next_installer) or (next_app and not app):
        raise ReleaseError("macOS application/installer pins must be complete current and optional next pairs")
    if next_app and (app, installer) == (next_app, next_installer):
        raise ReleaseError("Next macOS signer pair must differ from the current pair")
    if required and not app:
        raise ReleaseError("Signed distribution requires embedded macOS application and installer certificate pins")
    return dict(zip(("currentApplication", "currentInstaller", "nextApplication", "nextInstaller"), pins))


def validate_bundle(bundle, version, runner, expected_pins=None):
    product = bundle.stem
    with (bundle / "Contents/Info.plist").open("rb") as source:
        metadata = plistlib.load(source)
    if metadata.get("CFBundleShortVersionString") != version:
        raise ReleaseError("Built bundle version does not match the source snapshot")
    binary = bundle / f"Contents/MacOS/{product}"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ReleaseError("Missing or non-executable VST3 binary")
    architectures = runner(["lipo", "-archs", binary], capture=True).stdout.split()
    if len(architectures) != 2 or set(architectures) != {"arm64", "x86_64"}:
        raise ReleaseError("Release VST3 must contain exactly arm64 and x86_64")
    for architecture in architectures:
        output = runner(["xcrun", "vtool", "-arch", architecture, "-show-build", binary], capture=True).stdout
        if re.findall(r"\bminos\s+(\S+)", output) != ["11.0"]:
            raise ReleaseError(f"Unexpected deployment target for {architecture}")
    runner(["codesign", "--verify", "--deep", "--strict", bundle])
    helper = bundle / f"Contents/Helpers/{product}Updater.app"
    with (helper / "Contents/Info.plist").open("rb") as source:
        updater_info = plistlib.load(source)
    if (updater_info.get("CFBundleShortVersionString") != version
            or updater_info.get("WKProduct") != product):
        raise ReleaseError("Embedded updater version/product does not match the plugin")
    if expected_pins is not None and updater_info.get("WKSignerPins") != expected_pins:
        raise ReleaseError("Embedded updater signer pins differ from the release configuration")
    helper_binary = helper / f"Contents/MacOS/{product}Updater"
    if not os.access(helper_binary, os.X_OK):
        raise ReleaseError("Missing executable updater")
    helper_architectures = runner(["lipo", "-archs", helper_binary], capture=True).stdout.split()
    if len(helper_architectures) != 2 or set(helper_architectures) != {"arm64", "x86_64"}:
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
               "CMakeError.log": stage / "build/CMakeFiles/CMakeError.log",
               "notary-submit.json": stage / "notary-submit.json",
               "notary-log.json": stage / "notary-log.json",
               "gatekeeper-package.log": stage / "gatekeeper-package.log",
               "gatekeeper-vst3.log": stage / "gatekeeper-vst3.log"}
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
    version, app, installer, notary, notary_keychain, jobs = validate_options(
        root, configuration, version_override, environment
    )
    config = load_product(root)
    product = config["productName"]
    pins = signer_configuration(environment, required=bool(app))
    if (app or installer or notary) and not (root / ".git").exists():
        raise ReleaseError("Signed releases require a Git checkout; source archives support unsigned rebuilds only")
    if platform.system() != "Darwin":
        raise ReleaseError("macOS distribution tools are required")
    required_tools = ["cmake", "ctest", "codesign", "ditto", "pkgbuild", "pkgutil", "lipo", "xcrun"]
    if notary:
        required_tools.append("spctl")
    for tool in required_tools:
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
        pin_arguments = [f"-DWK_MACOS_{name}_CERT_SHA256={pins[key]}" for name, key in (
            ("APPLICATION", "currentApplication"), ("INSTALLER", "currentInstaller"),
            ("NEXT_APPLICATION", "nextApplication"), ("NEXT_INSTALLER", "nextInstaller"))]
        runner(["cmake", "-S", source, "-B", build, "-G", "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Release", *pin_arguments])
        # Build the default target so every registered CTest executable is built,
        # including future preset/parameter tests added without editing this script.
        diagnostics["step"] = "build"
        runner(["cmake", "--build", build, "-j", jobs])
        test_report = stage / "ctest-results.xml"
        diagnostics["step"] = "ctest"
        runner(["ctest", "--test-dir", build, "--output-on-failure", "--no-tests=error",
                "--output-junit", test_report])
        bundle = build / f"{product}_artefacts/Release/VST3/{product}.vst3"
        diagnostics["step"] = "validate_bundle"
        built_hash = validate_bundle(bundle, version, runner, pins)
        payload = stage / "payload"
        staged_bundle = payload / f"Library/Audio/Plug-Ins/VST3/{product}.vst3"
        staged_bundle.parent.mkdir(parents=True)
        diagnostics["step"] = "stage_bundle"
        runner(["ditto", bundle, staged_bundle])
        diagnostics["step"] = "sign_bundle"
        signing_identity = app or "-"
        signing_options = ["codesign", "--force", "--sign", signing_identity]
        if app:
            signing_options += ["--options", "runtime", "--timestamp"]
        else:
            print("Warning: ad-hoc VST3 and unsigned installer; no notarization claim.")
        # Sign nested code inside-out. Recursive --deep signing can hide missing
        # rules and is intentionally reserved for verification below.
        runner(signing_options + [staged_bundle / f"Contents/Helpers/{product}Updater.app"])
        runner(signing_options + [staged_bundle])
        packaged_hash = validate_bundle(staged_bundle, version, runner, pins)
        artifacts = stage / "artifacts"
        artifacts.mkdir()
        # Preserve auditable results before the temporary source/build is removed.
        # Missing reports are an error, never an unsubstantiated test-success claim.
        shutil.copy2(test_report, artifacts / "ctest-results.xml")
        shutil.copy2(build / "Testing/Temporary/LastTest.log", artifacts / "CTest-LastTest.log")
        package = artifacts / f"{product}-{version}-macOS-universal.pkg"
        components = stage / "components.plist"
        components.write_bytes(plistlib.dumps([{
            "RootRelativeBundlePath": f"Library/Audio/Plug-Ins/VST3/{product}.vst3",
            "BundleIsRelocatable": False, "BundleIsVersionChecked": True,
            "BundleHasStrictIdentifier": True, "BundleOverwriteAction": "upgrade"}]))
        command = ["pkgbuild", "--root", payload, "--identifier", config["bundleId"] + ".pkg",
                   "--version", version, "--install-location", "/", "--component-plist", components]
        if installer:
            command += ["--sign", installer]
        diagnostics["step"] = "pkgbuild"
        runner(command + [package])
        if app:
            diagnostics["step"] = "verify_signer_pairs"
            verifier_arguments = [sys.executable, "-B", source / "scripts/verify-macos-signers.py",
                                  "--package", package, "--bundle", staged_bundle,
                                  "--application-cert-sha256", pins["currentApplication"],
                                  "--installer-cert-sha256", pins["currentInstaller"],
                                  "--next-application-cert-sha256", pins["nextApplication"],
                                  "--next-installer-cert-sha256", pins["nextInstaller"]]
            run_and_log(verifier_arguments, artifacts / "signer-verification.json", runner)
        notary_submission_id = None
        if notary:
            diagnostics["step"] = "notary_submit"
            response = runner(["xcrun", "notarytool", "submit", package, "--keychain-profile", notary,
                               "--keychain", notary_keychain,
                               "--wait", "--output-format", "json"], capture=True)
            submit_raw = response.stdout if isinstance(response.stdout, str) else ""
            (stage / "notary-submit.json").write_text(submit_raw)
            submission = read_json_object(submit_raw, "Notarization submission")
            notary_submission_id = read_uuid(submission.get("id"), "Notarization submission")

            # Fetch the full log whenever Apple returned a usable job ID, including
            # for an Invalid job, so a failed candidate retains actionable evidence.
            diagnostics["step"] = "notary_log"
            log_response = runner(["xcrun", "notarytool", "log", str(notary_submission_id),
                                   "--keychain-profile", notary,
                                   "--keychain", notary_keychain], capture=True)
            log_raw = log_response.stdout if isinstance(log_response.stdout, str) else ""
            (stage / "notary-log.json").write_text(log_raw)
            validate_notary_log(log_raw, notary_submission_id)
            if submission.get("status") != "Accepted":
                raise ReleaseError("Notarization was not accepted; existing releases are unchanged")
            diagnostics["step"] = "staple"
            for target in (package, staged_bundle):
                runner(["xcrun", "stapler", "staple", target])
                runner(["xcrun", "stapler", "validate", target])
        archive = artifacts / f"{product}-{version}-macOS-universal-VST3.zip"
        diagnostics["step"] = "zip_roundtrip"
        runner(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", staged_bundle, archive])
        roundtrip = stage / "zip-roundtrip"
        runner(["ditto", "-x", "-k", archive, roundtrip])
        restored = roundtrip / f"{product}.vst3"
        if validate_bundle(restored, version, runner, pins) != packaged_hash:
            raise ReleaseError("ZIP-restored binary differs from the signed candidate")
        host = build / f"{product}HostTests_artefacts/Release/{product}HostTests"
        diagnostics["step"] = "zip_host"
        runner([host, restored])
        expanded = stage / "package-roundtrip"
        diagnostics["step"] = "installer_roundtrip"
        runner(["pkgutil", "--expand-full", package, expanded])
        installed = expanded / f"Payload/Library/Audio/Plug-Ins/VST3/{product}.vst3"
        if validate_bundle(installed, version, runner, pins) != packaged_hash:
            raise ReleaseError("Installer payload differs from the signed candidate")
        diagnostics["step"] = "installer_host"
        runner([host, installed])
        if notary:
            diagnostics["step"] = "gatekeeper_package"
            run_and_log(["spctl", "--assess", "--type", "install", "--verbose=4", package],
                        stage / "gatekeeper-package.log", runner)
            diagnostics["step"] = "gatekeeper_vst3"
            run_and_log(["spctl", "--assess", "--type", "execute", "--verbose=4", restored],
                        stage / "gatekeeper-vst3.log", runner)
        diagnostics["step"] = "write_manifests"
        if notary:
            for name in ("notary-submit.json", "notary-log.json",
                         "gatekeeper-package.log", "gatekeeper-vst3.log"):
                shutil.copy2(stage / name, artifacts / name)
        write_json(artifacts / "source-manifest.json", provenance)
        verification = ["generated_factory_bank_matches_snapshot", "fresh_snapshot_build", "ctest",
                        "zip_host_state_audio_roundtrip", "installer_payload_host_state_audio_roundtrip",
                        "both_macos11_slices"]
        if notary:
            verification += ["notary_log_accepted", "gatekeeper_package", "gatekeeper_zip_vst3"]
        if app:
            verification += ["pinned_installer_and_both_application_slices"]
        manifest = {"schema": 1, "version": version, "configuration": configuration,
                    "commit": provenance["repositories"][0]["commit"],
                    "source_sha256": provenance["source_sha256"],
                    "dirty": any(repo["dirty"] for repo in provenance["repositories"]),
                    "built_binary_sha256": built_hash, "packaged_binary_sha256": packaged_hash,
                    "application_signed": bool(app), "installer_signed": bool(installer), "notarized": bool(notary),
                    "macos_signer_pins": pins,
                    "verification": verification,
                    "artifacts": {path.name: file_hash(path) for path in sorted(artifacts.iterdir())}}
        if notary_submission_id is not None:
            manifest["notary_submission_id"] = str(notary_submission_id)
        write_json(artifacts / "release-manifest.json", manifest)
        checksums = "".join(f"{file_hash(path)}  {path.name}\n" for path in sorted(artifacts.iterdir()))
        (artifacts / "SHA256SUMS.txt").write_text(checksums)
        name = (f"{product}-{version}-{manifest['commit'][:12]}-"
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
