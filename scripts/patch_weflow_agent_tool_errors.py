#!/usr/bin/env python3
"""Stage the WeFlow 6.3.1 AI Agent tool error stream repair.

Unavailable tools still need a tool-input-error before tool-output-error. Keep
the existing retry bookkeeping and let the normal stream publish both events.
"""

import argparse
import json
from pathlib import Path

from patch_weflow_sns_thumbnail_urls import sha256, stage_resized_entries


EXPECTED_HASHES = {
    "dist-electron/main.js": "94dbaf0b2ba8fa7cf36a182def6607749e789004b127762e70023dd928cddb85",
    "dist-electron/agentRunWorker.js": "6c47a5eb6d4058e7ac110195ff23a4459e8d67fdb4ae952cb05c9efe40836bec",
}
ERROR_PREFIX = (
    'if(e.type===`tool-input-error`&&(O.has(Q(e.toolCallId))||'
    '/AI_NoSuchToolError/.test(Q(e.errorText)))){let t=Q(e.toolName);'
)
RETRY_SETS = {"dist-electron/main.js": "ei", "dist-electron/agentRunWorker.js": "ri"}


def patch_bundle(payload, entry):
    if sha256(payload) != EXPECTED_HASHES[entry]:
        raise ValueError(f"Unrecognized WeFlow 6.3.1 bundle for {entry}")
    retry_set = RETRY_SETS[entry]
    old = (ERROR_PREFIX + f"t&&{retry_set}.add(t);continue}}").encode("utf-8")
    new = (ERROR_PREFIX + f"t&&{retry_set}.add(t);}}").encode("utf-8")
    if payload.count(old) != 1:
        raise ValueError(f"Expected exactly one Agent error filter in {entry}")
    return payload.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=Path("/Applications/WeFlow.app"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check and args.output_dir is None:
        parser.error("--output-dir is required unless --check is used")
    contents = args.app / "Contents"
    patches = {entry: lambda payload, entry=entry: patch_bundle(payload, entry) for entry in EXPECTED_HASHES}
    result = stage_resized_entries(contents / "Resources/app.asar", contents / "Info.plist",
                                   args.output_dir, patches, args.check)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
