#!/usr/bin/env python3
"""Compile and exercise the production SNS XML parser without a WeChat database.

The parser is file-local, so this runner copies its source section into a temporary
translation unit. Fixtures exercise the real implementation, not a Python port.
Run with Python 3 and a C++17 compiler; no WCDB dependency is needed.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


FIXTURES = r'''
#include <iostream>
#include <stdexcept>

void Check(bool passed, const char* label) {
    if (!passed) throw std::runtime_error(label);
}

int main() {
    try {
        std::vector<ExtractedMedia> media;
        ParseMediaList(R"XML(
            <media><url token="full-token" key="18446744073709551615"
                md5="full-md5" enc_idx="7">https://example.test/full</url>
                <thumb token="thumb-token" key="42" md5="thumb-md5"
                enc_idx="2">https://example.test/thumb</thumb>
                <key>legacy-key</key><md5>legacy-md5</md5></media>
        )XML", media);
        Check(media.size() == 1, "URL attribute fixture count");
        Check(media[0].key == "18446744073709551615", "large decimal key exact and URL precedence");
        Check(media[0].md5 == "full-md5", "URL md5 precedence");
        Check(media[0].token == "full-token" && media[0].thumbToken == "thumb-token", "separate tokens");
        Check(media[0].encIdx == "7", "URL encryption index precedence");
        Check(media[0].url == "https://example.test/full" &&
              media[0].thumb == "https://example.test/thumb", "media URLs preserved");

        media.clear();
        ParseMediaList(R"XML(
            <media><url>https://example.test/full</url>
                <thumb token='thumb-token' key='9007199254740993'
                md5='thumb-md5' encIdx='4'>https://example.test/thumb</thumb></media>
            <media><url>https://example.test/legacy</url>
                <key>legacy-key</key><md5>legacy-md5</md5></media>
            <media><url>https://example.test/plain</url></media>
        )XML", media);
        Check(media.size() == 3, "sibling media count");
        Check(media[0].key == "9007199254740993" && media[0].md5 == "thumb-md5", "thumbnail attribute fallback");
        Check(media[0].encIdx == "4", "thumbnail camel-case index fallback");
        Check(media[1].key == "legacy-key" && media[1].md5 == "legacy-md5", "legacy child tags preserved");
        Check(media[2].key.empty() && media[2].md5.empty(), "unencrypted metadata stays empty");

        media.clear();
        ParseMediaList(R"XML(
            <media><url key="parent-key">https://example.test/still</url>
                <LivePhoto><media><url token="live-token" key="18446744073709551614"
                md5="live-md5" encIdx="9">https://example.test/live</url>
                <thumb token="live-thumb-token" key="fallback-key">https://example.test/live-thumb</thumb>
                </media></LivePhoto></media>
            <media><url key="next-key">https://example.test/next</url></media>
        )XML", media);
        Check(media.size() == 2 && media[1].key == "next-key", "nested LivePhoto keeps following siblings");
        Check(media[0].key == "parent-key" && media[0].livePhoto.has_value(), "nested LivePhoto parsed");
        const auto& live = *media[0].livePhoto;
        Check(live.key == "18446744073709551614" && live.md5 == "live-md5", "LivePhoto URL attributes exact");
        Check(live.token == "live-token" && live.thumbToken == "live-thumb-token", "LivePhoto tokens preserved");
        Check(live.encIdx == "9" && live.thumb == "https://example.test/live-thumb", "LivePhoto index and thumbnail");

        media.clear();
        ParseMediaList(R"XML(
            <media><url>https://example.test/still</url><LivePhoto><media>
                <url>https://example.test/live</url><thumb key="thumb-key" md5="thumb-md5"
                enc_idx="3" token="thumb-token">https://example.test/live-thumb</thumb>
            </media></LivePhoto></media>
            <media><url>https://example.test/still2</url><LivePhoto><media>
                <url>https://example.test/live2</url><key>legacy-live-key</key><md5>legacy-live-md5</md5>
            </media></LivePhoto></media>
        )XML", media);
        Check(media.size() == 2 && media[0].livePhoto && media[1].livePhoto, "multiple LivePhotos");
        Check(media[0].livePhoto->key == "thumb-key" && media[0].livePhoto->md5 == "thumb-md5" &&
              media[0].livePhoto->encIdx == "3", "LivePhoto thumbnail fallback");
        Check(media[1].livePhoto->key == "legacy-live-key" &&
              media[1].livePhoto->md5 == "legacy-live-md5", "LivePhoto legacy fields preserved");

        media.clear();
        ParseMediaList("<mediaList></mediaList>", media);
        Check(media.empty(), "mediaList is not a media element");
        std::cout << "SNS parser regression fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[1] / "src/media_sns_ai.cpp")
    args = parser.parse_args()
    source = args.source.read_text()
    start = source.index("std::string ExtractXmlTag(")
    end = source.index("std::string SerializeSnsPost(", start)
    unit = "#include <string>\n#include <vector>\n#include <optional>\n" + source[start:end] + FIXTURES
    with tempfile.TemporaryDirectory(prefix="weflow-sns-parser-") as temp:
        cpp = Path(temp) / "sns_media_parser_test.cpp"
        binary = Path(temp) / "sns_media_parser_test"
        cpp.write_text(unit)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-pedantic",
                        str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
