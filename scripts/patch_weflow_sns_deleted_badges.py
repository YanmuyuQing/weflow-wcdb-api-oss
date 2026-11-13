#!/usr/bin/env python3
"""Stage the WeFlow 6.3.1 Moments renderer repair for false deletion badges.

A failed media request only describes that URL. Use the existing retry tile
for image and video failures, and keep the post badge tied to manual deletion.
The output contains app.asar and Info.plist for review and installation.
"""

import argparse
import json
from pathlib import Path

from patch_weflow_sns_thumbnail_urls import stage_entry_patch


RENDERER_ENTRY = "dist/assets/ContactSnsTimelineDialog-B9h7U6sl.js"
MEDIA_ERROR = b'R=e=>{if(e===404||e===410){p();return}m.current<2?(m.current++,g(e=>e+1)):a(!0)}'
VIDEO_ERROR = MEDIA_ERROR.replace(b'R=', b'L=').replace(b':a(!0)', b':p()')
RETRY_MEDIA = b'R=e=>{e!==404&&e!==410&&m.current<2?(m.current++,g(e=>e+1)):a(!0)}'
POST_CALLBACK = b',onMediaDeleted:[1,54].includes(e.type??0)?()=>c(!0):void 0'


def patch_renderer(payload):
    deleted_tile_start = b'o?(0,w.jsx)(`div`,{className:`sns-media-item deleted-media`'
    retry_tile_start = b'i?(0,w.jsx)(`div`,{className:`sns-media-item failed-media`'
    try:
        tile_start = payload.index(deleted_tile_start)
        tile_end = payload.index(retry_tile_start, tile_start)
        icon_start = payload.index(b'b=n(`image-off`,')
        icon_end = payload.index(b'x=n(`map-pin`,', icon_start)
    except ValueError as error:
        raise ValueError("Expected WeFlow 6.3.1 media placeholders were not found") from error

    replacements = (
        (payload[icon_start:icon_end], b""),
        (VIDEO_ERROR + b"," + MEDIA_ERROR, RETRY_MEDIA),
        (b"[o,l]=(0,C.useState)(!1),", b""),
        (b"p=()=>{l(!0),r?.()},", b""),
        (b"else L(n.status);", b"else R(n.status);"),
        (b"N?L():R(),d(!1),M(!1)", b"R(),d(!1),M(!1)"),
        (payload[tile_start:tile_end], b""),
        (b"[s,c]=(0,C.useState)(!1),[d,f]", b"[d,f]"),
        (b"s||d?`post-deleted`", b"d?`post-deleted`"),
        (b"(s||d)&&(0,w.jsxs)(`span`,{className:`post-deleted-badge`",
         b"d&&(0,w.jsxs)(`span`,{className:`post-deleted-badge`"),
        (POST_CALLBACK, b""),
        (b",onMediaDeleted:r", b""),
    )
    for old, new in replacements:
        expected = 3 if old == b",onMediaDeleted:r" else 1
        if payload.count(old) != expected or len(new) > len(old):
            raise ValueError(f"Unexpected WeFlow 6.3.1 renderer code near {old[:75]!r}")
        # Keep ASAR offsets and renderer line positions stable.
        payload = payload.replace(old, new.ljust(len(old)))
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=Path("/Applications/WeFlow.app"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--check", action="store_true", help="Validate repair and hashes without writing files")
    args = parser.parse_args()
    if not args.check and args.output_dir is None:
        parser.error("--output-dir is required unless --check is used")
    contents = args.app / "Contents"
    result = stage_entry_patch(contents / "Resources/app.asar", contents / "Info.plist",
                               args.output_dir, RENDERER_ENTRY, patch_renderer, args.check)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
