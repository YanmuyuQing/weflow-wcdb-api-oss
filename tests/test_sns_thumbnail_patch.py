#!/usr/bin/env python3
"""Regression tests for staged thumbnail URL repair and ASAR integrity updates."""

import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
import struct


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/patch_weflow_sns_thumbnail_urls.py"
SPEC = importlib.util.spec_from_file_location("sns_thumbnail_patch", SCRIPT)
patch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(patch)
URL_SUFFIX = b'if(!t)return r;let[i,a]=r.split(`?`);if(a){let e=a.split(`&`).filter(e=>!e.startsWith(`token=`)&&!e.startsWith(`idx=`));r=e.length>0?`${i}?${e.join(`&`)}`:i}if(n){let e=r.split(`?`);return`${e[0]}?token=${t}&idx=1${e[1]?`&${e[1]}`:``}`}let o=r.includes(`?`)?`&`:`?`;return`${r}${o}token=${t}&idx=1`}'
MAIN = patch.URL_PREFIX + patch.SIZE_REWRITE + URL_SUFFIX


class ThumbnailPatchTest(unittest.TestCase):
    def test_signed_paths_preserved_with_token_handling(self):
        repaired = patch.patch_main(MAIN)
        function = repaired.decode().split("QA=", 1)[1]
        javascript = "const normalize = (" + function + ");\n" + r'''
            const assert = require('node:assert/strict');
            for (const size of ['150', '200', '480', '0']) {
                assert.equal(normalize(`http://example.test/image/${size}`, 'signed-token'),
                             `https://example.test/image/${size}?token=signed-token&idx=1`);
            }
            assert.equal(normalize('http://example.test/image/150?token=old&idx=2&tp=webp', 'new'),
                         'https://example.test/image/150?tp=webp&token=new&idx=1');
            assert.equal(normalize('http://example.test/image/150'), 'https://example.test/image/150');
            assert.equal(normalize('http://example.test/video?tp=mp4', 'token', true),
                         'https://example.test/video?token=token&idx=1&tp=mp4');
        '''
        subprocess.run(["node", "-e", javascript], check=True)
        self.assertEqual(len(repaired), len(MAIN))
        with self.assertRaisesRegex(ValueError, "already applied"):
            patch.patch_main(repaired)
        with self.assertRaisesRegex(ValueError, "exactly once"):
            patch.patch_main(MAIN + MAIN)

    def test_stage_preserves_archive_offsets_and_updates_integrity(self):
        with tempfile.TemporaryDirectory(prefix="weflow-thumbnail-patch-test-") as temp:
            root = Path(temp)
            source_asar = root / "original.asar"
            source_plist = root / "original.plist"
            other = b"unrelated payload bytes"
            digest = patch.sha256(MAIN)
            header = {"files": {
                "other.txt": {"size": len(other), "offset": "0"},
                "dist-electron": {"files": {"main.js": {
                    "size": len(MAIN), "offset": str(len(other)),
                    "integrity": {"algorithm": "SHA256", "hash": digest,
                                  "blockSize": 4194304, "blocks": [digest]}}}}}}
            raw_header = json.dumps(header, separators=(",", ":")).encode()
            padded_size = (len(raw_header) + 3) // 4 * 4
            prefix = struct.pack("<IIII", 4, 8 + padded_size, 4 + padded_size, len(raw_header))
            source_asar.write_bytes(prefix + raw_header + b"\0" * (padded_size - len(raw_header)) + other + MAIN)
            info = {"CFBundleShortVersionString": "6.3.1", "ElectronAsarIntegrity": {
                "Resources/app.asar": {"algorithm": "SHA256", "hash": patch.sha256(raw_header)}}}
            source_plist.write_bytes(plistlib.dumps(info))
            original_asar = source_asar.read_bytes()
            original_plist = source_plist.read_bytes()
            result = patch.stage_patch(source_asar, source_plist, root / "staged")
            repaired_asar = Path(result["app_asar"])
            repaired_header, _, repaired_offset, repaired_main = patch.read_main(repaired_asar)
            self.assertEqual(source_asar.read_bytes(), original_asar)
            self.assertEqual(source_plist.read_bytes(), original_plist)
            self.assertEqual(len(repaired_asar.read_bytes()), len(original_asar))
            self.assertEqual(repaired_main, patch.patch_main(MAIN))
            self.assertEqual(repaired_asar.read_bytes()[repaired_offset - len(other):repaired_offset], other)
            self.assertEqual(result["asar_header_sha256"], patch.sha256(repaired_header))
            self.assertEqual(plistlib.loads(Path(result["info_plist"]).read_bytes())["ElectronAsarIntegrity"]
                             ["Resources/app.asar"]["hash"], result["asar_header_sha256"])
            with self.assertRaisesRegex(ValueError, "already exists"):
                patch.stage_patch(source_asar, source_plist, root / "staged")


if __name__ == "__main__":
    unittest.main()
