#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Whykiki Audio
"""Reproducible, source-complete release archives from exact Git objects.

No worktree bytes, untracked files, Git internals, build output or credentials
enter an archive. A separate verification step checks every archived byte and
the JUCE pin before extraction/build. ZIP timestamps, ordering, modes and storage
are fixed so identical source commits produce identical archives on every OS.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tempfile
import unicodedata
import zipfile

from release_contract import ContractError, JUCE_COMMIT, load_product, strict_json, validate_product

MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 512 * 1024 * 1024
MAX_ENTRIES = 20000
MANIFEST = "SOURCE-MANIFEST.json"
SBOM = "SBOM.spdx.json"
OWN_DIRECTORIES = {"Source", "Tests", "Updater", "scripts", "cmake", "Installer", "Presets",
                   "release", "LICENSES", ".github", ".config", ".reuse", "docs"}
ROOT_FILES = {"CMakeLists.txt", "LICENSE", "COPYING", "NOTICE", "REUSE.toml", ".gitignore", ".gitmodules"}
SECRET_SUFFIXES = {".pfx", ".p12", ".pem", ".key", ".keychain", ".keychain-db"}
PRIVATE_KEY = re.compile(br"-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----")
IDENTITY = "Whykiki Audio"


class SourceError(ValueError):
    pass


def canonical_json(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(name):
    if not isinstance(name, str) or not name or len(name.encode("utf-8")) > 1024:
        raise SourceError("Invalid source path length")
    if name.startswith("/") or "\\" in name or any(ord(char) < 32 or ord(char) == 127 for char in name):
        raise SourceError(f"Unsafe source path: {name!r}")
    for part in name.split("/"):
        if part in {"", ".", ".."} or part.casefold() == ".git" or part.endswith((".", " ")) or ":" in part:
            raise SourceError(f"Unsafe source path: {name!r}")
        if re.fullmatch(r"(?i)(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?", part):
            raise SourceError(f"Windows-reserved source path: {name!r}")
    return name


def path_key(name):
    return unicodedata.normalize("NFC", name).casefold()


def validate_payload(name, data):
    safe_path(name)
    leaf = PurePosixPath(name).name.lower()
    if leaf == ".env" or leaf.startswith(".env.") or PurePosixPath(leaf).suffix in SECRET_SUFFIXES:
        raise SourceError(f"Credential-like file is not permitted in source archives: {name}")
    if len(data) > MAX_FILE_BYTES or PRIVATE_KEY.search(data):
        raise SourceError(f"Oversized source or private key material: {name}")


def git(repo, *arguments):
    return subprocess.check_output(["git", "-C", str(repo), *arguments], stderr=subprocess.PIPE)


def clean_commit(repo, expected=None):
    commit = git(repo, "rev-parse", "HEAD").decode("ascii").strip()
    if expected is not None and commit != expected:
        raise SourceError("Checkout does not match the required source commit")
    if git(repo, "status", "--porcelain=v1", "--untracked-files=normal").strip():
        raise SourceError("Source archives require a clean committed checkout, including submodules")
    return commit


def own_source_path(name):
    path = PurePosixPath(name)
    return (path.parts[0] in OWN_DIRECTORIES or name in ROOT_FILES
            or (len(path.parts) == 1 and path.suffix.lower() == ".md"))


def git_source_files(repo, commit, *, vendor=False):
    """Read immutable blobs (never mutable worktree content) in one batch."""
    entries = []
    excluded = []
    for record in git(repo, "ls-tree", "-r", "-z", commit).split(b"\0"):
        if not record:
            continue
        metadata, raw_name = record.split(b"\t", 1)
        mode, kind, object_id = metadata.decode("ascii").split(" ")
        try:
            name = safe_path(raw_name.decode("utf-8"))
        except UnicodeError as error:
            raise SourceError("Source names must use UTF-8") from error
        if not vendor and name == "external/JUCE" and mode == "160000":
            if object_id != JUCE_COMMIT:
                raise SourceError("JUCE gitlink differs from the reviewed commit")
            continue
        if kind != "blob" or mode not in {"100644", "100755"}:
            raise SourceError(f"Unsupported symlink/submodule/special source entry: {name}")
        if not vendor and not own_source_path(name):
            excluded.append(name)
            continue
        entries.append((name, object_id, 0o755 if mode == "100755" else 0o644))
    if len(entries) > MAX_ENTRIES:
        raise SourceError("Source has too many entries")
    request = "".join(oid + "\n" for _, oid, _ in entries).encode("ascii")
    sizes_raw = subprocess.check_output(["git", "-C", str(repo), "cat-file", "--batch-check"],
                                        input=request, stderr=subprocess.PIPE)
    sizes = []
    for row in sizes_raw.splitlines():
        fields = row.split()
        if len(fields) != 3 or fields[1] != b"blob" or not fields[2].isdigit():
            raise SourceError("Invalid Git object metadata")
        sizes.append(int(fields[2]))
    if len(sizes) != len(entries) or any(size > MAX_FILE_BYTES for size in sizes) or sum(sizes) > MAX_TOTAL_BYTES:
        raise SourceError("Source exceeds archive size limits")
    raw = subprocess.check_output(["git", "-C", str(repo), "cat-file", "--batch"],
                                   input=request, stderr=subprocess.PIPE)
    stream = io.BytesIO(raw)
    files = {}
    for (name, object_id, mode), size in zip(entries, sizes):
        if stream.readline() != f"{object_id} blob {size}\n".encode("ascii"):
            raise SourceError("Git object metadata changed during reading")
        data = stream.read(size)
        if len(data) != size or stream.read(1) != b"\n":
            raise SourceError("Truncated Git object")
        validate_payload(name, data)
        files[name] = (data, mode)
    if stream.read():
        raise SourceError("Unexpected data after Git objects")
    return files, excluded


def sbom(config, commit, files):
    """SPDX 2.3 inventory; vendor license conclusions are intentionally not invented."""
    records = []
    relationships = []
    package_hashes = {"SPDXRef-Product": [], "SPDXRef-JUCE": []}
    for index, (name, (data, _)) in enumerate(sorted(files.items())):
        vendor = name.startswith("external/JUCE/")
        file_id = f"SPDXRef-File-{index:05d}"
        digest = hashlib.sha1(data).hexdigest()
        package = "SPDXRef-JUCE" if vendor else "SPDXRef-Product"
        package_hashes[package].append(digest)
        records.append({"SPDXID": file_id, "fileName": "./" + name,
                        "checksums": [{"algorithm": "SHA256", "checksumValue": sha256(data)},
                                      {"algorithm": "SHA1", "checksumValue": digest}],
                        "licenseConcluded": "NOASSERTION",
                        "licenseInfoInFiles": ["NOASSERTION"], "copyrightText": "NOASSERTION"})
        relationships.append({"spdxElementId": package, "relationshipType": "CONTAINS",
                              "relatedSpdxElement": file_id})
    packages = []
    for package_id, name, version, download, declared in (
            ("SPDXRef-Product", config["productName"], config["version"],
             f"git+https://github.com/{config['repository']}.git@{commit}", "AGPL-3.0-only"),
            ("SPDXRef-JUCE", "JUCE", config["juce"]["version"],
             f"git+https://github.com/juce-framework/JUCE.git@{JUCE_COMMIT}", "NOASSERTION")):
        packages.append({"SPDXID": package_id, "name": name, "versionInfo": version,
                         "downloadLocation": download, "filesAnalyzed": True,
                         "licenseDeclared": declared, "licenseConcluded": "NOASSERTION",
                         "licenseInfoFromFiles": ["NOASSERTION"], "copyrightText": "NOASSERTION",
                         "packageVerificationCode": {"packageVerificationCodeValue": hashlib.sha1(
                             "".join(sorted(package_hashes[package_id])).encode("ascii")).hexdigest()}})
    packages[1]["licenseComments"] = ("JUCE modules use the upstream AGPLv3 option. The complete source tree also "
        "contains separately licensed dependencies and examples; preserve LICENSE.md and the notices in each "
        "subdirectory. Inclusion does not relicense those dependencies or enable non-VST3 SDK formats.")
    relationships.extend([
        {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-Product"},
        {"spdxElementId": "SPDXRef-Product", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": "SPDXRef-JUCE"}])
    return {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
            "name": config["productName"] + " corresponding source",
            "documentNamespace": f"https://github.com/{config['repository']}/source-sbom/{commit}/{JUCE_COMMIT}",
            "creationInfo": {"creators": ["Tool: whykiki-source-release-2"], "created": "2026-01-01T00:00:00Z",
                             "comment": "Fixed metadata date for reproducibility; source identity is the Git commit."},
            "packages": packages, "files": records, "relationships": relationships}


def make_manifest(config, commit, files, excluded):
    records = [{"path": name, "sha256": sha256(data), "size": len(data), "mode": mode}
               for name, (data, mode) in sorted(files.items())]
    return {"schemaVersion": 2, "release-contract-version": 2, "product": config["productName"],
            "version": config["version"], "commit": commit, "juceCommit": JUCE_COMMIT,
            "excludedNonBuildFiles": sorted(excluded), "files": records,
            "sourceSha256": sha256(canonical_json(records))}


def create_archive(repo, juce, output, commit=None):
    repo, juce, output = Path(repo).resolve(), Path(juce).resolve(), Path(output)
    if output.exists() or output.is_symlink():
        raise SourceError("Refusing to replace an existing source archive")
    root_commit = clean_commit(repo, commit)
    clean_commit(juce, JUCE_COMMIT)
    own, excluded = git_source_files(repo, root_commit)
    config = validate_product(strict_json(own["release/product.json"][0]))
    vendor, _ = git_source_files(juce, JUCE_COMMIT, vendor=True)
    files = {**own, **{"external/JUCE/" + name: value for name, value in vendor.items()}}
    required = {"LICENSE", "THIRD_PARTY_NOTICES.md", "CMakeLists.txt", "Presets/FactoryPresets.json",
                "external/JUCE/CMakeLists.txt", "external/JUCE/LICENSE.md", "scripts/source-release.py",
                "scripts/source_release.py", "scripts/release_contract.py"}
    if not required <= set(files):
        raise SourceError("Required build inputs/notices are missing: " + ", ".join(sorted(required - set(files))))
    if len({path_key(name) for name in files}) != len(files):
        raise SourceError("Source filenames collide on case-insensitive platforms")
    files[SBOM] = (canonical_json(sbom(config, root_commit, files)), 0o644)
    manifest = make_manifest(config, root_commit, files, excluded)
    files[MANIFEST] = (canonical_json(manifest), 0o644)
    prefix = f"{config['productName']}-{config['version']}-Source"
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".source-release-", dir=output.parent) as temp:
        temporary = Path(temp) / "source.zip"
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
            for name, (data, mode) in sorted(files.items()):
                member = zipfile.ZipInfo(prefix + "/" + safe_path(name), date_time=(1980, 1, 1, 0, 0, 0))
                member.create_system = 3
                member.external_attr = (stat.S_IFREG | mode) << 16
                member.compress_type = zipfile.ZIP_STORED
                archive.writestr(member, data)
        verify_archive(temporary, expected_commit=root_commit)
        # Exclusive-create publication works on Windows and POSIX and never overwrites.
        try:
            with temporary.open("rb") as source, output.open("xb") as target:
                while chunk := source.read(1024 * 1024):
                    target.write(chunk)
        except FileExistsError as error:
            raise SourceError("Source archive appeared during publication") from error
    return manifest


def validate_manifest(manifest):
    required = {"schemaVersion", "release-contract-version", "product", "version", "commit",
                "juceCommit", "excludedNonBuildFiles", "files", "sourceSha256"}
    if not isinstance(manifest, dict) or set(manifest) != required or type(manifest["schemaVersion"]) is not int \
            or manifest["schemaVersion"] != 2 or manifest["release-contract-version"] != 2:
        raise SourceError("Unsupported source manifest")
    if manifest["juceCommit"] != JUCE_COMMIT or not isinstance(manifest["commit"], str) \
            or not re.fullmatch(r"[0-9a-f]{40}", manifest["commit"]):
        raise SourceError("Invalid source/JUCE commit")
    records = manifest["files"]
    if not isinstance(records, list) or not records or len(records) > MAX_ENTRIES:
        raise SourceError("Invalid source file inventory")
    paths, folded, size = [], set(), 0
    for record in records:
        if not isinstance(record, dict) or set(record) != {"path", "sha256", "size", "mode"}:
            raise SourceError("Invalid source record")
        name = safe_path(record["path"])
        if name == MANIFEST or path_key(name) in folded or not isinstance(record["sha256"], str) \
                or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"]) \
                or type(record["size"]) is not int or not 0 <= record["size"] <= MAX_FILE_BYTES \
                or type(record["mode"]) is not int or record["mode"] not in {0o644, 0o755}:
            raise SourceError("Invalid/duplicate source file record")
        paths.append(name)
        folded.add(path_key(name))
        size += record["size"]
    if paths != sorted(paths) or size > MAX_TOTAL_BYTES or manifest["sourceSha256"] != sha256(canonical_json(records)):
        raise SourceError("Source inventory order, size or digest is invalid")
    if not isinstance(manifest["excludedNonBuildFiles"], list):
        raise SourceError("Invalid excluded-file inventory")
    for name in manifest["excludedNonBuildFiles"]:
        safe_path(name)
    return {record["path"]: record for record in records}


def validate_contents(manifest, files, expected_commit=None):
    records = validate_manifest(manifest)
    if expected_commit and manifest["commit"] != expected_commit:
        raise SourceError("Source archive is for a different commit")
    if set(files) != set(records):
        raise SourceError("Archive payload differs from the manifest inventory")
    for name, (data, mode) in files.items():
        validate_payload(name, data)
        record = records[name]
        if len(data) != record["size"] or sha256(data) != record["sha256"] or mode != record["mode"]:
            raise SourceError(f"Source bytes or mode differ from manifest: {name}")
    config = validate_product(strict_json(files["release/product.json"][0]))
    if config["productName"] != manifest["product"] or config["version"] != manifest["version"]:
        raise SourceError("Product identity differs from source manifest")
    original = {name: value for name, value in files.items() if name != SBOM}
    if files[SBOM][0] != canonical_json(sbom(config, manifest["commit"], original)):
        raise SourceError("SPDX inventory does not match the corresponding source")
    return config


def verify_archive(path, expected_commit=None):
    if Path(path).is_symlink() or Path(path).stat().st_size > MAX_TOTAL_BYTES + 20 * 1024 * 1024:
        raise SourceError("Unsafe or oversized source ZIP")
    files = {}
    prefix = None
    total = 0
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if not entries or len(entries) > MAX_ENTRIES or archive.comment:
            raise SourceError("Invalid source archive metadata")
        folded = set()
        for member in entries:
            safe_path(member.filename)
            parts = member.filename.split("/", 1)
            if len(parts) != 2 or path_key(member.filename) in folded:
                raise SourceError("Duplicate or non-rooted ZIP entry")
            folded.add(path_key(member.filename))
            if prefix is None:
                prefix = parts[0]
            if parts[0] != prefix:
                raise SourceError("Source archive must have exactly one root")
            mode = member.external_attr >> 16
            if member.create_system != 3 or not stat.S_ISREG(mode) or stat.S_IMODE(mode) not in {0o644, 0o755} \
                    or member.flag_bits & 1 or member.compress_type != zipfile.ZIP_STORED \
                    or member.file_size > MAX_FILE_BYTES or member.extra or member.comment \
                    or member.date_time != (1980, 1, 1, 0, 0, 0):
                raise SourceError("Unsafe or nondeterministic ZIP member metadata")
            total += member.file_size
            if total > MAX_TOTAL_BYTES:
                raise SourceError("Source ZIP exceeds total size limit")
            files[parts[1]] = (archive.read(member), stat.S_IMODE(mode))
    if MANIFEST not in files:
        raise SourceError("Source ZIP has no provenance manifest")
    manifest = strict_json(files.pop(MANIFEST)[0])
    config = validate_contents(manifest, files, expected_commit)
    if prefix != f"{config['productName']}-{config['version']}-Source":
        raise SourceError("Archive root does not match product/version")
    return manifest


def verify_tree(root, juce_only=False):
    """Verify extracted vendor sources before CMake trusts a gitless checkout.

    This establishes correspondence with the delivered manifest, not authenticity
    of an arbitrary manifest. Release checksums/signatures establish authenticity.
    Build directories outside the manifest are intentionally ignored.
    """
    root = Path(root).resolve()
    manifest_path = root / MANIFEST
    if manifest_path.is_symlink():
        raise SourceError("Symlinked source manifest")
    manifest = strict_json(manifest_path.read_bytes())
    records = validate_manifest(manifest)
    selected = {name: record for name, record in records.items()
                if not juce_only or name.startswith("external/JUCE/")}
    if not selected or (juce_only and "external/JUCE/CMakeLists.txt" not in selected):
        raise SourceError("Missing JUCE source inventory")
    files = {}
    for name, record in selected.items():
        candidate = root / name
        if any(parent.is_symlink() for parent in (candidate, *candidate.parents) if parent != root.parent):
            raise SourceError(f"Symlink in extracted source: {name}")
        if not candidate.is_file() or candidate.stat().st_size != record["size"]:
            raise SourceError(f"Missing/resized extracted source: {name}")
        data = candidate.read_bytes()
        validate_payload(name, data)
        if sha256(data) != record["sha256"]:
            raise SourceError(f"Modified extracted source: {name}")
        files[name] = (data, record["mode"])
    if juce_only:
        vendor = root / "external/JUCE"
        actual = set()
        for candidate in vendor.rglob("*"):
            if candidate.is_symlink():
                raise SourceError("Symlink in extracted JUCE tree")
            if candidate.is_file():
                actual.add(candidate.relative_to(root).as_posix())
        if actual != set(selected):
            raise SourceError("Extracted JUCE tree contains unmanifested files")
    else:
        validate_contents(manifest, files)
        # CMake/test output may live under a separate build directory, but extra
        # source files in the shipped source directories could affect globs or
        # imports and must not be silently accepted.
        for directory in OWN_DIRECTORIES | {"external"}:
            for candidate in (root / directory).rglob("*"):
                if candidate.is_symlink():
                    raise SourceError("Symlink in extracted source tree")
                if candidate.is_file():
                    relative = candidate.relative_to(root)
                    if "__pycache__" in relative.parts and candidate.suffix == ".pyc":
                        continue
                    if relative.as_posix() not in records:
                        raise SourceError("Extracted source contains unmanifested files")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create")
    create.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    create.add_argument("--juce", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--commit", help="Expected HEAD commit; historical/dirty checkouts are rejected")
    verify = commands.add_parser("verify")
    verify.add_argument("--archive", type=Path, required=True)
    verify.add_argument("--commit")
    tree = commands.add_parser("verify-tree")
    tree.add_argument("--root", type=Path, required=True)
    tree.add_argument("--juce-only", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "create":
            manifest = create_archive(args.repo, args.juce, args.output, args.commit)
        elif args.command == "verify":
            manifest = verify_archive(args.archive, args.commit)
        else:
            manifest = verify_tree(args.root, args.juce_only)
    except (SourceError, ContractError, OSError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Source release rejected: {error}\n")
    print(f"Verified {manifest['product']} {manifest['version']} source at {manifest['commit']}")


if __name__ == "__main__":
    main()
