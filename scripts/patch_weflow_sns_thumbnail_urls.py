#!/usr/bin/env python3
"""Stage a reversible WeFlow 6.3.1 thumbnail URL repair without editing the app.

WeFlow rewrites signed thumbnail paths such as /150 to /0 while retaining the
thumbnail token. Remove only that rewrite, preserving ASAR offsets and file sizes,
and update the payload and header integrity hashes. The output contains app.asar
and Info.plist for review and installation after backing up their originals.
"""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import struct


URL_PREFIX = b'QA=(e,t,n=!1)=>{if(!e)return e;let r=e.replace(`http://`,`https://`);'
SIZE_REWRITE = br'if(!n){let[e,t]=r.split(`?`),n=e.replace(/\/(150|200|480)($|\?)/,`/0$2`);r=t?`${n}?${t}`:n}'
MAIN_ENTRY = "dist-electron/main.js"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_packed_entry(asar_path, entry):
    with asar_path.open("rb") as archive:
        prefix = archive.read(16)
        if len(prefix) != 16:
            raise ValueError("Truncated ASAR header")
        marker, header_size, pickle_size, json_size = struct.unpack("<IIII", prefix)
        if marker != 4 or pickle_size + 4 != header_size or json_size > pickle_size - 4:
            raise ValueError("Unsupported ASAR header layout")
        raw_header = archive.read(json_size)
        header = json.loads(raw_header)
        meta = header
        for part in entry.split("/"):
            meta = meta["files"][part]
        if meta.get("unpacked") or meta.get("link"):
            raise ValueError(f"{entry} must be packed in app.asar")
        offset = 8 + header_size + int(meta["offset"])
        archive.seek(offset)
        payload = archive.read(meta["size"])
        integrity = meta["integrity"]
        digest = sha256(payload)
        if len(payload) != meta["size"] or integrity["algorithm"] != "SHA256":
            raise ValueError(f"Invalid {entry} payload metadata")
        if integrity["hash"] != digest:
            raise ValueError(f"Existing {entry} integrity hash does not match its contents")
        block_size = integrity["blockSize"]
        blocks = [sha256(payload[i:i + block_size]) for i in range(0, len(payload), block_size)]
        if integrity["blocks"] != blocks:
            raise ValueError(f"Existing {entry} block integrity does not match its contents")
        return raw_header, meta, offset, payload


def read_main(asar_path):
    return read_packed_entry(asar_path, MAIN_ENTRY)


def patch_main(main):
    old = URL_PREFIX + SIZE_REWRITE
    new = URL_PREFIX + b" " * len(SIZE_REWRITE)
    if main.count(old) != 1:
        if new in main:
            raise ValueError("Thumbnail URL repair is already applied")
        raise ValueError("Expected WeFlow 6.3.1 URL normalization code was not found exactly once")
    return main.replace(old, new, 1)


def stage_entry_patch(asar_path, plist_path, output_dir, entry, patch_entry, check_only=False):
    raw_header, meta, offset, payload = read_packed_entry(asar_path, entry)
    plist_bytes = plist_path.read_bytes()
    info = plistlib.loads(plist_bytes)
    if info.get("CFBundleShortVersionString") != "6.3.1":
        raise ValueError("This narrowly scoped patch supports WeFlow 6.3.1 only")
    old_header_hash = sha256(raw_header)
    plist_integrity = info["ElectronAsarIntegrity"]["Resources/app.asar"]
    if plist_integrity.get("algorithm") != "SHA256" or plist_integrity.get("hash") != old_header_hash:
        raise ValueError("Existing Info.plist ASAR header hash does not match")
    patched_payload = patch_entry(payload)
    if len(patched_payload) != len(payload):
        raise ValueError(f"Expected {entry} patch to preserve the payload size")
    if len(meta["integrity"]["blocks"]) != 1:
        raise ValueError(f"Expected {entry} to fit in one integrity block")
    old_payload_hash = meta["integrity"]["hash"].encode("ascii")
    new_payload_hash = sha256(patched_payload).encode("ascii")
    # In this version the payload hash and its sole block hash occur twice.
    # Fail closed if another header entry happens to share that digest.
    if raw_header.count(old_payload_hash) != 2:
        raise ValueError(f"Unexpected {entry} integrity hash occurrence count")
    patched_header = raw_header.replace(old_payload_hash, new_payload_hash)
    new_header_hash = sha256(patched_header)
    if plist_bytes.count(old_header_hash.encode("ascii")) != 1:
        raise ValueError("Expected one text ASAR hash in Info.plist")
    patched_plist = plist_bytes.replace(old_header_hash.encode("ascii"), new_header_hash.encode("ascii"), 1)
    summary = {"payload_sha256": new_payload_hash.decode("ascii"), "asar_header_sha256": new_header_hash,
               "archive_size_unchanged": True}
    if check_only:
        return {"check_only": True, **summary}
    output_dir.mkdir(parents=True, exist_ok=True)
    staged_asar = output_dir / "app.asar"
    staged_plist = output_dir / "Info.plist"
    if staged_asar.exists() or staged_plist.exists():
        raise ValueError("Output app.asar or Info.plist already exists; choose a fresh output directory")
    shutil.copy2(asar_path, staged_asar)
    with staged_asar.open("r+b") as archive:
        archive.seek(16)
        archive.write(patched_header)
        archive.seek(offset)
        archive.write(patched_payload)
    staged_plist.write_bytes(patched_plist)
    verified_header, verified_meta, verified_offset, verified_payload = read_packed_entry(staged_asar, entry)
    verified_info = plistlib.loads(staged_plist.read_bytes())
    if (verified_header != patched_header or verified_payload != patched_payload or verified_offset != offset or
            verified_meta["size"] != meta["size"] or staged_asar.stat().st_size != asar_path.stat().st_size or
            verified_info["ElectronAsarIntegrity"]["Resources/app.asar"]["hash"] != sha256(verified_header)):
        raise ValueError("Staged repair verification failed")
    return {"app_asar": str(staged_asar), "info_plist": str(staged_plist), **summary}


def stage_patch(asar_path, plist_path, output_dir, check_only=False):
    result = stage_entry_patch(asar_path, plist_path, output_dir, MAIN_ENTRY, patch_main, check_only)
    result["main_sha256"] = result.pop("payload_sha256")
    result["removed_rewrite_bytes"] = len(SIZE_REWRITE)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=Path("/Applications/WeFlow.app"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--check", action="store_true", help="Validate repair and hashes without writing files")
    args = parser.parse_args()
    if not args.check and args.output_dir is None:
        parser.error("--output-dir is required unless --check is used")
    contents = args.app / "Contents"
    result = stage_patch(contents / "Resources/app.asar", contents / "Info.plist",
                         args.output_dir, check_only=args.check)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
