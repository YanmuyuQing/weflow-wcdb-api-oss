#include "internal.hpp"

namespace weflow {

namespace {

std::vector<std::shared_ptr<WCDB::Database>> FindMediaDatabases(AccountContext& ctx) {
    std::lock_guard<std::recursive_mutex> lock(ctx.mutex);
    std::vector<std::shared_ptr<WCDB::Database>> result;
    fs::path base(ctx.db_storage_dir);
    for (const char* sub : {"media", "message"}) {
        fs::path dir = base / sub;
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            std::string fname = ToLower(it->path().filename().string());
            if (fname.rfind("media_", 0) == 0 && fname.size() > 9 && fname.substr(fname.size() - 3) == ".db") {
                if (auto db = OpenEncryptedDb(ctx, it->path().string(), false)) {
                    result.push_back(db);
                }
            }
        }
    }
    return result;
}

bool QueryVoiceRecord(
    AccountContext& ctx,
    int64_t serverId,
    int64_t localId,
    std::vector<uint8_t>& outBytes,
    int64_t& outDataSize,
    int64_t& outCreateTime) {
    auto mediaDbs = FindMediaDatabases(ctx);
    for (const auto& db : mediaDbs) {
        WCDB::Handle dbHandle = db->getHandle();
        std::vector<std::string> tables;
        WCDB::Statement tstmt = MakeRawStatement(
            "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'VoiceInfo%'");
        if (dbHandle.prepare(tstmt)) {
            while (dbHandle.step()) {
                WCDB::Value v = dbHandle.getValue(0);
                if (!v.isNull()) tables.push_back(static_cast<std::string>(v.textValue()));
            }
            dbHandle.finalize();
        }
        for (const auto& tbl : tables) {
            std::ostringstream sql;
            sql << "SELECT voice_data, data_size, create_time FROM " << tbl << " WHERE ";
            if (serverId != 0) {
                sql << "svr_id = " << serverId;
                if (localId > 0) sql << " OR local_id = " << localId;
            } else {
                sql << "local_id = " << localId;
            }
            sql << " LIMIT 1";
            WCDB::Statement stmt = MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    WCDB::Value bv = dbHandle.getValue(0);
                    if (bv.getType() == WCDB::Value::Type::BLOB) {
                        WCDB::Data d = bv.blobValue();
                        outBytes.assign(d.buffer(), d.buffer() + d.size());
                        outDataSize = dbHandle.getValue(1).isNull()
                            ? static_cast<int64_t>(outBytes.size())
                            : dbHandle.getValue(1).intValue();
                        outCreateTime = dbHandle.getValue(2).intValue();
                        dbHandle.finalize();
                        return true;
                    }
                }
                dbHandle.finalize();
            }
        }
    }
    return false;
}

std::string ToHex(const std::vector<uint8_t>& data) {
    static const char hexChars[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (uint8_t b : data) {
        s.push_back(hexChars[(b >> 4) & 0x0F]);
        s.push_back(hexChars[b & 0x0F]);
    }
    return s;
}

std::string ExtractXmlTag(const std::string& xml, const std::string& tag, size_t startPos = 0, size_t* outEndPos = nullptr) {
    std::string openTag = "<" + tag;
    size_t open = xml.find(openTag, startPos);
    auto isMediaTag = [&](size_t at) {
        size_t after = at + openTag.size();
        return after < xml.size() &&
            (xml[after] == '>' || xml[after] == '/' || xml[after] == ' ' ||
             xml[after] == '\t' || xml[after] == '\r' || xml[after] == '\n');
    };
    if (tag == "media") {
        while (open != std::string::npos && !isMediaTag(open)) {
            open = xml.find(openTag, open + openTag.size());
        }
    }
    if (open == std::string::npos) return "";
    size_t tagEnd = xml.find('>', open);
    if (tagEnd == std::string::npos) return "";
    if (tagEnd > open && xml[tagEnd - 1] == '/') {
        if (outEndPos) *outEndPos = tagEnd + 1;
        return "";
    }
    std::string closeTag = "</" + tag + ">";
    size_t close = xml.find(closeTag, tagEnd);
    // LivePhoto can contain a nested <media>; do not end the parent at its close.
    if (tag == "media") {
        size_t nested = xml.find(openTag, tagEnd + 1);
        while (close != std::string::npos && nested != std::string::npos && nested < close) {
            size_t nestedEnd = xml.find('>', nested);
            if (nestedEnd == std::string::npos) return "";
            if (isMediaTag(nested) && xml[nestedEnd - 1] != '/') {
                close = xml.find(closeTag, close + closeTag.size());
            }
            nested = xml.find(openTag, nestedEnd + 1);
        }
    }
    if (close == std::string::npos) return "";
    if (outEndPos) *outEndPos = close + closeTag.size();
    return xml.substr(tagEnd + 1, close - tagEnd - 1);
}

std::string ExtractXmlAttr(const std::string& xml, const std::string& attr, size_t startPos = 0) {
    std::string t1 = attr + "=\"";
    size_t p = xml.find(t1, startPos);
    char quote = '"';
    if (p == std::string::npos) {
        std::string t2 = attr + "='";
        p = xml.find(t2, startPos);
        quote = '\'';
        if (p == std::string::npos) return "";
        p += t2.size();
    } else {
        p += t1.size();
    }
    size_t end = xml.find(quote, p);
    if (end == std::string::npos) return "";
    return xml.substr(p, end - p);
}

struct ExtractedMedia {
    std::string url;
    std::string thumb;
    std::string md5;
    std::string token;
    std::string thumbToken;
    std::string key;
    std::string encIdx;
    struct LivePhotoData {
        std::string url;
        std::string thumb;
        std::string md5;
        std::string token;
        std::string thumbToken;
        std::string key;
        std::string encIdx;
    };
    std::optional<LivePhotoData> livePhoto;
};

void ParseMediaList(const std::string& xml, std::vector<ExtractedMedia>& outMedias) {
    size_t pos = 0;
    while (true) {
        size_t nextEnd = 0;
        std::string mediaXml = ExtractXmlTag(xml, "media", pos, &nextEnd);
        if (mediaXml.empty() && nextEnd == 0) break;
        pos = (nextEnd > pos) ? nextEnd : (pos + 7);

        ExtractedMedia m;
        size_t urlTagPos = mediaXml.find("<url");
        if (urlTagPos != std::string::npos) {
            size_t endUrlTag = mediaXml.find('>', urlTagPos);
            if (endUrlTag != std::string::npos) {
                std::string header = mediaXml.substr(urlTagPos, endUrlTag - urlTagPos);
                m.token = ExtractXmlAttr(header, "token");
                m.key = ExtractXmlAttr(header, "key");
                m.md5 = ExtractXmlAttr(header, "md5");
                m.encIdx = ExtractXmlAttr(header, "enc_idx");
                if (m.encIdx.empty()) m.encIdx = ExtractXmlAttr(header, "encIdx");
            }
        }
        m.url = ExtractXmlTag(mediaXml, "url");

        size_t thumbTagPos = mediaXml.find("<thumb");
        if (thumbTagPos != std::string::npos) {
            size_t endThumbTag = mediaXml.find('>', thumbTagPos);
            if (endThumbTag != std::string::npos) {
                std::string header = mediaXml.substr(thumbTagPos, endThumbTag - thumbTagPos);
                m.thumbToken = ExtractXmlAttr(header, "token");
                if (m.key.empty()) m.key = ExtractXmlAttr(header, "key");
                if (m.md5.empty()) m.md5 = ExtractXmlAttr(header, "md5");
                if (m.encIdx.empty()) m.encIdx = ExtractXmlAttr(header, "enc_idx");
                if (m.encIdx.empty()) m.encIdx = ExtractXmlAttr(header, "encIdx");
            }
        }
        m.thumb = ExtractXmlTag(mediaXml, "thumb");
        // Modern clients put these on <url>/<thumb>; older data uses child tags.
        // Keep keys as strings: ISAAC seeds can exceed JavaScript's safe integers.
        if (m.md5.empty()) m.md5 = ExtractXmlTag(mediaXml, "md5");
        if (m.key.empty()) m.key = ExtractXmlTag(mediaXml, "key");

        size_t lpPos = mediaXml.find("<LivePhoto");
        if (lpPos != std::string::npos) {
            std::string lpXml = ExtractXmlTag(mediaXml, "LivePhoto", lpPos);
            if (!lpXml.empty()) {
                std::string subMedia = ExtractXmlTag(lpXml, "media");
                ExtractedMedia::LivePhotoData lpd;
                size_t lUrlPos = subMedia.find("<url");
                if (lUrlPos != std::string::npos) {
                    size_t endLUrl = subMedia.find('>', lUrlPos);
                    if (endLUrl != std::string::npos) {
                        std::string h = subMedia.substr(lUrlPos, endLUrl - lUrlPos);
                        lpd.token = ExtractXmlAttr(h, "token");
                        lpd.key = ExtractXmlAttr(h, "key");
                        lpd.md5 = ExtractXmlAttr(h, "md5");
                        lpd.encIdx = ExtractXmlAttr(h, "enc_idx");
                        if (lpd.encIdx.empty()) lpd.encIdx = ExtractXmlAttr(h, "encIdx");
                    }
                }
                size_t lThumbPos = subMedia.find("<thumb");
                if (lThumbPos != std::string::npos) {
                    size_t endLThumb = subMedia.find('>', lThumbPos);
                    if (endLThumb != std::string::npos) {
                        std::string h = subMedia.substr(lThumbPos, endLThumb - lThumbPos);
                        lpd.thumbToken = ExtractXmlAttr(h, "token");
                        if (lpd.key.empty()) lpd.key = ExtractXmlAttr(h, "key");
                        if (lpd.md5.empty()) lpd.md5 = ExtractXmlAttr(h, "md5");
                        if (lpd.encIdx.empty()) lpd.encIdx = ExtractXmlAttr(h, "enc_idx");
                        if (lpd.encIdx.empty()) lpd.encIdx = ExtractXmlAttr(h, "encIdx");
                    }
                }
                lpd.url = ExtractXmlTag(subMedia, "url");
                lpd.thumb = ExtractXmlTag(subMedia, "thumb");
                if (lpd.md5.empty()) lpd.md5 = ExtractXmlTag(subMedia, "md5");
                if (lpd.key.empty()) lpd.key = ExtractXmlTag(subMedia, "key");
                m.livePhoto = lpd;
            }
        }

        outMedias.push_back(std::move(m));
    }
}

std::string SerializeSnsPost(
    const std::string& tid,
    const std::string& username,
    const std::string& rawXml,
    int64_t fallbackCreateTime) {
    std::ostringstream oss;
    oss << "{\"tid\":\"" << weflow::EscapeJson(tid) << "\","
        << "\"id\":\"" << weflow::EscapeJson(tid) << "\",";

    std::string postUser = ExtractXmlTag(rawXml, "username");
    if (postUser.empty()) postUser = username;
    oss << "\"username\":\"" << weflow::EscapeJson(postUser) << "\",";

    std::string nickname = ExtractXmlTag(rawXml, "nickname");
    oss << "\"nickname\":\"" << weflow::EscapeJson(nickname) << "\",";

    std::string cTimeStr = ExtractXmlTag(rawXml, "createTime");
    int64_t cTime = cTimeStr.empty() ? fallbackCreateTime : std::strtoll(cTimeStr.c_str(), nullptr, 10);
    oss << "\"createTime\":" << cTime << ",";

    std::string contentDesc = ExtractXmlTag(rawXml, "contentDesc");
    oss << "\"contentDesc\":\"" << weflow::EscapeJson(contentDesc) << "\",";

    std::string styleStr = ExtractXmlTag(rawXml, "contentStyle");
    int32_t type = styleStr.empty() ? 1 : static_cast<int32_t>(std::strtol(styleStr.c_str(), nullptr, 10));
    oss << "\"type\":" << type << ",";

    std::string mediaListXml = ExtractXmlTag(rawXml, "mediaList");
    std::vector<ExtractedMedia> medias;
    if (!mediaListXml.empty()) {
        ParseMediaList(mediaListXml, medias);
    }
    oss << "\"media\":[";
    for (size_t i = 0; i < medias.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& m = medias[i];
        oss << "{\"url\":\"" << weflow::EscapeJson(m.url) << "\","
            << "\"thumb\":\"" << weflow::EscapeJson(m.thumb) << "\","
            << "\"md5\":\"" << weflow::EscapeJson(m.md5) << "\","
            << "\"token\":\"" << weflow::EscapeJson(m.token) << "\","
            << "\"thumbToken\":\"" << weflow::EscapeJson(m.thumbToken) << "\","
            << "\"key\":\"" << weflow::EscapeJson(m.key) << "\","
            << "\"encIdx\":\"" << weflow::EscapeJson(m.encIdx) << "\"";
        if (m.livePhoto.has_value()) {
            const auto& lp = *m.livePhoto;
            oss << ",\"livePhoto\":{"
                << "\"url\":\"" << weflow::EscapeJson(lp.url) << "\","
                << "\"thumb\":\"" << weflow::EscapeJson(lp.thumb) << "\","
                << "\"md5\":\"" << weflow::EscapeJson(lp.md5) << "\","
                << "\"token\":\"" << weflow::EscapeJson(lp.token) << "\","
                << "\"thumbToken\":\"" << weflow::EscapeJson(lp.thumbToken) << "\","
                << "\"key\":\"" << weflow::EscapeJson(lp.key) << "\","
                << "\"encIdx\":\"" << weflow::EscapeJson(lp.encIdx) << "\"}";
        }
        oss << "}";
    }
    oss << "],";

    std::string likeXml = ExtractXmlTag(rawXml, "likeUserList");
    oss << "\"likes\":[";
    bool firstLike = true;
    size_t lpos = 0;
    while (!likeXml.empty()) {
        size_t nextEnd = 0;
        std::string userXml = ExtractXmlTag(likeXml, "User", lpos, &nextEnd);
        if (userXml.empty() && nextEnd == 0) break;
        lpos = (nextEnd > lpos) ? nextEnd : (lpos + 6);
        std::string lu = ExtractXmlTag(userXml, "username");
        std::string ln = ExtractXmlTag(userXml, "nickname");
        if (!lu.empty()) {
            if (!firstLike) oss << ",";
            firstLike = false;
            oss << "{\"username\":\"" << weflow::EscapeJson(lu) << "\","
                << "\"nickname\":\"" << weflow::EscapeJson(ln) << "\"}";
        }
    }
    oss << "],";

    std::string commentXml = ExtractXmlTag(rawXml, "commentUserList");
    oss << "\"comments\":[";
    bool firstComment = true;
    size_t cpos = 0;
    while (!commentXml.empty()) {
        size_t nextEnd = 0;
        std::string cObj = ExtractXmlTag(commentXml, "Comment", cpos, &nextEnd);
        if (cObj.empty() && nextEnd == 0) break;
        cpos = (nextEnd > cpos) ? nextEnd : (cpos + 9);
        std::string cid = ExtractXmlTag(cObj, "id");
        std::string cu = ExtractXmlTag(cObj, "username");
        std::string cn = ExtractXmlTag(cObj, "nickname");
        std::string cc = ExtractXmlTag(cObj, "content");
        int64_t ct = std::strtoll(ExtractXmlTag(cObj, "createTime").c_str(), nullptr, 10);
        std::string rcid = ExtractXmlTag(cObj, "refCommentId");
        std::string ru = ExtractXmlTag(cObj, "refUsername");
        std::string rn = ExtractXmlTag(cObj, "refNickname");
        if (!firstComment) oss << ",";
        firstComment = false;
        oss << "{\"id\":\"" << weflow::EscapeJson(cid) << "\","
            << "\"username\":\"" << weflow::EscapeJson(cu) << "\","
            << "\"nickname\":\"" << weflow::EscapeJson(cn) << "\","
            << "\"content\":\"" << weflow::EscapeJson(cc) << "\","
            << "\"createTime\":" << ct << ","
            << "\"refCommentId\":\"" << weflow::EscapeJson(rcid) << "\","
            << "\"refUsername\":\"" << weflow::EscapeJson(ru) << "\","
            << "\"refNickname\":\"" << weflow::EscapeJson(rn) << "\"}";
    }
    oss << "],";

    oss << "\"rawXml\":\"" << weflow::EscapeJson(rawXml) << "\"}";
    return oss.str();
}

} // namespace

} // namespace weflow

extern "C" {

// ============================================================================
// Hardlinks (Image & Video)
// ============================================================================

WCDB_API int32_t wcdb_resolve_image_hardlink(
    int64_t handle, const char* md5, const char* accountDir, char** outJson) {
    if (!md5 || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        auto hlDb = weflow::ResolveDbByKindOrPath(*ctx, "hardlink");
        if (!hlDb) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = hlDb->getHandle();
        std::string infoTable, dirTable;
        {
            WCDB::Statement s1 = weflow::MakeRawStatement(
                "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'image_hardlink_info%' ORDER BY name DESC LIMIT 1");
            if (dbHandle.prepare(s1)) {
                if (dbHandle.step() && !dbHandle.getValue(0).isNull()) {
                    infoTable = static_cast<std::string>(dbHandle.getValue(0).textValue());
                }
                dbHandle.finalize();
            }
            WCDB::Statement s2 = weflow::MakeRawStatement(
                "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'dir2id%' LIMIT 1");
            if (dbHandle.prepare(s2)) {
                if (dbHandle.step() && !dbHandle.getValue(0).isNull()) {
                    dirTable = static_cast<std::string>(dbHandle.getValue(0).textValue());
                }
                dbHandle.finalize();
            }
        }
        if (infoTable.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        std::string sql = "SELECT dir1, dir2, file_name FROM " + infoTable +
                          " WHERE lower(md5) = lower('" + weflow::EscapeSqlLiteral(md5) + "') LIMIT 1";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        int64_t dir1Id = 0, dir2Id = 0;
        std::string dir1Str, dir2Str, fileName;
        bool found = false;
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                WCDB::Value v0 = dbHandle.getValue(0);
                WCDB::Value v1 = dbHandle.getValue(1);
                WCDB::Value v2 = dbHandle.getValue(2);
                if (v0.getType() == WCDB::Value::Type::Integer) dir1Id = v0.intValue();
                else if (!v0.isNull()) dir1Str = static_cast<std::string>(v0.textValue());
                if (v1.getType() == WCDB::Value::Type::Integer) dir2Id = v1.intValue();
                else if (!v1.isNull()) dir2Str = static_cast<std::string>(v1.textValue());
                if (!v2.isNull()) fileName = static_cast<std::string>(v2.textValue());
                found = !fileName.empty();
            }
            dbHandle.finalize();
        }
        if (!found) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        auto resolveDirId = [&](int64_t rowid) -> std::string {
            if (rowid <= 0 || dirTable.empty()) return "";
            std::string dsql = "SELECT username FROM " + dirTable + " WHERE rowid = " + std::to_string(rowid) + " LIMIT 1";
            WCDB::Statement ds = weflow::MakeRawStatement(dsql);
            std::string res;
            if (dbHandle.prepare(ds)) {
                if (dbHandle.step() && !dbHandle.getValue(0).isNull()) {
                    res = static_cast<std::string>(dbHandle.getValue(0).textValue());
                }
                dbHandle.finalize();
            }
            return res;
        };
        if (dir1Str.empty() && dir1Id > 0) dir1Str = resolveDirId(dir1Id);
        if (dir2Str.empty() && dir2Id > 0) dir2Str = resolveDirId(dir2Id);

        std::string rootDir = (accountDir && accountDir[0] != '\0') ? std::string(accountDir) : ctx->account_root_dir;
        std::filesystem::path fullPath = std::filesystem::path(rootDir) / "msg" / "attach" /
                                         dir1Str / dir2Str / "Img" / fileName;
        std::string hlDbPath = (std::filesystem::path(ctx->db_storage_dir) / "hardlink" / "hardlink.db").string();
        std::ostringstream oss;
        oss << "{\"hardlink_db_path\":\"" << weflow::EscapeJson(hlDbPath) << "\","
            << "\"dir1\":\"" << weflow::EscapeJson(dir1Str) << "\","
            << "\"dir2\":\"" << weflow::EscapeJson(dir2Str) << "\","
            << "\"file_name\":\"" << weflow::EscapeJson(fileName) << "\","
            << "\"full_path\":\"" << weflow::EscapeJson(fullPath.string()) << "\"}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("resolve_image_hardlink", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_resolve_video_hardlink_md5(
    int64_t handle, const char* md5, const char* dbPath, char** outJson) {
    if (!md5 || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::lock_guard<std::recursive_mutex> lock(ctx->mutex);
        std::string target = (dbPath && dbPath[0] != '\0') ? std::string(dbPath) : "hardlink";
        auto hlDb = weflow::ResolveDbByKindOrPath(*ctx, target);
        if (!hlDb) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = hlDb->getHandle();
        std::string sql =
            "SELECT v.file_name, COALESCE(d1.username, CAST(v.dir1 AS TEXT)), COALESCE(d2.username, CAST(v.dir2 AS TEXT)) "
            "FROM video_hardlink_info_v4 v "
            "LEFT JOIN dir2id d1 ON v.dir1 = d1.rowid "
            "LEFT JOIN dir2id d2 ON v.dir2 = d2.rowid "
            "WHERE lower(v.md5) = lower('" + weflow::EscapeSqlLiteral(md5) + "') LIMIT 1";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::string fileName, dir1, dir2;
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                if (!dbHandle.getValue(0).isNull()) fileName = static_cast<std::string>(dbHandle.getValue(0).textValue());
                if (!dbHandle.getValue(1).isNull()) dir1 = static_cast<std::string>(dbHandle.getValue(1).textValue());
                if (!dbHandle.getValue(2).isNull()) dir2 = static_cast<std::string>(dbHandle.getValue(2).textValue());
            }
            dbHandle.finalize();
        }
        if (fileName.empty()) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        std::string relPath = "msg/video/" + (dir1.empty() ? "" : (dir1 + "/")) +
                              (dir2.empty() ? "" : (dir2 + "/")) + fileName;
        std::ostringstream oss;
        oss << "{\"md5\":\"" << weflow::EscapeJson(md5) << "\","
            << "\"file_name\":\"" << weflow::EscapeJson(fileName) << "\","
            << "\"dir1\":\"" << weflow::EscapeJson(dir1) << "\","
            << "\"dir2\":\"" << weflow::EscapeJson(dir2) << "\","
            << "\"relative_path\":\"" << weflow::EscapeJson(relPath) << "\"}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("resolve_video_hardlink_md5", -2, e.what());
        return -2;
    }
}

// ============================================================================
// Voice Media
// ============================================================================

WCDB_API int32_t wcdb_get_voice_data(
    int64_t handle,
    const char* /*sessionId*/,
    int32_t /*createTime*/,
    int32_t localId,
    int64_t svrId,
    const char* /*candidatesJson*/,
    char** outHex) {
    if (!outHex) return -1;
    *outHex = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::vector<uint8_t> bytes;
        int64_t dataSize = 0, ct = 0;
        if (!weflow::QueryVoiceRecord(*ctx, svrId, localId, bytes, dataSize, ct) || bytes.empty()) {
            return -2;
        }
        *outHex = weflow::AllocCString(weflow::BytesToHex(bytes.data(), bytes.size()));
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_voice_data", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_voice_raw_data(
    int64_t handle, const char* /*sessionId*/, int64_t serverId, int64_t localId, uint8_t** outBuf, int32_t* outLen) {
    if (!outBuf || !outLen) return -1;
    *outBuf = nullptr;
    *outLen = 0;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::vector<uint8_t> bytes;
        int64_t dataSize = 0, createTime = 0;
        if (!weflow::QueryVoiceRecord(*ctx, serverId, localId, bytes, dataSize, createTime) || bytes.empty()) {
            return -2;
        }
        uint8_t* buf = static_cast<uint8_t*>(std::malloc(bytes.size()));
        if (!buf) return -3;
        std::memcpy(buf, bytes.data(), bytes.size());
        *outBuf = buf;
        *outLen = static_cast<int32_t>(bytes.size());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_voice_raw_data", -2, e.what());
        return -2;
    }
}

// ============================================================================
// Emoticons
// ============================================================================

WCDB_API int32_t wcdb_get_emoticon_info(int64_t handle, const char* md5, char** outJson) {
    if (!md5 || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto emoDb = weflow::ResolveDbByKindOrPath(*ctx, "emoticon");
        if (!emoDb) {
            *outJson = weflow::AllocCString("{}");
            return 0;
        }
        WCDB::Handle dbHandle = emoDb->getHandle();
        for (const char* tbl : {"kNonStoreEmoticonTable", "CustomEmoticon"}) {
            std::string sql = std::string("SELECT md5, cdn_url, encrypt_url, aes_key, thumb_url, extern_url, extern_md5 "
                                          "FROM ") + tbl +
                              " WHERE lower(md5) = lower('" + weflow::EscapeSqlLiteral(md5) + "') LIMIT 1";
            WCDB::Statement stmt = weflow::MakeRawStatement(sql);
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    auto gs = [&dbHandle](int idx) -> std::string {
                        WCDB::Value v = dbHandle.getValue(idx);
                        return v.isNull() ? "" : static_cast<std::string>(v.textValue());
                    };
                    std::ostringstream oss;
                    oss << "{\"md5\":\"" << weflow::EscapeJson(gs(0)) << "\","
                        << "\"cdn_url\":\"" << weflow::EscapeJson(gs(1)) << "\","
                        << "\"encrypt_url\":\"" << weflow::EscapeJson(gs(2)) << "\","
                        << "\"aes_key\":\"" << weflow::EscapeJson(gs(3)) << "\","
                        << "\"thumb_url\":\"" << weflow::EscapeJson(gs(4)) << "\","
                        << "\"extern_url\":\"" << weflow::EscapeJson(gs(5)) << "\","
                        << "\"extern_md5\":\"" << weflow::EscapeJson(gs(6)) << "\"}";
                    dbHandle.finalize();
                    *outJson = weflow::AllocCString(oss.str());
                    return 0;
                }
                dbHandle.finalize();
            }
        }
        *outJson = weflow::AllocCString("{}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_emoticon_info", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_custom_emoticons(
    int64_t handle, int32_t typeFilter, int32_t limit, int32_t offset, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto emoDb = weflow::ResolveDbByKindOrPath(*ctx, "emoticon");
        if (!emoDb) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        int32_t lim = (limit > 0) ? limit : 200;
        int32_t off = (offset >= 0) ? offset : 0;
        WCDB::Handle dbHandle = emoDb->getHandle();
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        for (const char* tbl : {"kNonStoreEmoticonTable", "CustomEmoticon"}) {
            std::ostringstream sql;
            sql << "SELECT md5, type, cdn_url, encrypt_url, aes_key, thumb_url, extern_url, package_id FROM " << tbl;
            if (typeFilter >= 0) sql << " WHERE type = " << typeFilter;
            sql << " LIMIT " << lim << " OFFSET " << off;
            WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
            if (dbHandle.prepare(stmt)) {
                while (dbHandle.step()) {
                    auto gs = [&dbHandle](int idx) -> std::string {
                        WCDB::Value v = dbHandle.getValue(idx);
                        return v.isNull() ? "" : static_cast<std::string>(v.textValue());
                    };
                    if (!first) oss << ",";
                    first = false;
                    oss << "{\"md5\":\"" << weflow::EscapeJson(gs(0)) << "\","
                        << "\"type\":" << dbHandle.getValue(1).intValue() << ","
                        << "\"cdn_url\":\"" << weflow::EscapeJson(gs(2)) << "\","
                        << "\"encrypt_url\":\"" << weflow::EscapeJson(gs(3)) << "\","
                        << "\"aes_key\":\"" << weflow::EscapeJson(gs(4)) << "\","
                        << "\"thumb_url\":\"" << weflow::EscapeJson(gs(5)) << "\","
                        << "\"extern_url\":\"" << weflow::EscapeJson(gs(6)) << "\","
                        << "\"package_id\":\"" << weflow::EscapeJson(gs(7)) << "\"}";
                }
                dbHandle.finalize();
                break;
            }
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_custom_emoticons", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_emoticon_packages(int64_t handle, int32_t limit, int32_t offset, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto emoDb = weflow::ResolveDbByKindOrPath(*ctx, "emoticon");
        if (!emoDb) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        int32_t lim = (limit > 0) ? limit : 100;
        int32_t off = (offset >= 0) ? offset : 0;
        WCDB::Handle dbHandle = emoDb->getHandle();
        std::ostringstream sql;
        sql << "SELECT package_id, pack_name, icon_url FROM EmoticonPackage LIMIT " << lim << " OFFSET " << off;
        WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                auto gs = [&dbHandle](int idx) -> std::string {
                    WCDB::Value v = dbHandle.getValue(idx);
                    return v.isNull() ? "" : static_cast<std::string>(v.textValue());
                };
                if (!first) oss << ",";
                first = false;
                oss << "{\"package_id\":\"" << weflow::EscapeJson(gs(0)) << "\","
                    << "\"pack_name\":\"" << weflow::EscapeJson(gs(1)) << "\","
                    << "\"icon_url\":\"" << weflow::EscapeJson(gs(2)) << "\"}";
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_emoticon_packages", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_add_custom_emoticon(int64_t handle, const char* emoticonJson, char** outJson) {
    if (!emoticonJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto emoDb = weflow::ResolveDbByKindOrPath(*ctx, "emoticon");
        if (!emoDb) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"emoticon_db_not_found\"}");
            return 0;
        }
        std::string j(emoticonJson);
        std::string md5 = weflow::ParseJsonStringField(j, "md5");
        std::string cdnUrl = weflow::ParseJsonStringField(j, "cdn_url");
        std::string aesKey = weflow::ParseJsonStringField(j, "aes_key");
        int64_t type = weflow::ParseJsonIntField(j, "type", 1);
        if (md5.empty()) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"missing_md5\"}");
            return 0;
        }
        WCDB::Handle dbHandle = emoDb->getHandle();
        std::ostringstream sql;
        sql << "INSERT OR REPLACE INTO kNonStoreEmoticonTable (md5, type, cdn_url, aes_key) VALUES ('"
            << weflow::EscapeSqlLiteral(md5) << "', " << type << ", '"
            << weflow::EscapeSqlLiteral(cdnUrl) << "', '"
            << weflow::EscapeSqlLiteral(aesKey) << "')";
        bool ok = dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql.str()));
        *outJson = weflow::AllocCString(ok ? "{\"success\":true}" : "{\"success\":false,\"error\":\"insert_failed\"}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("add_custom_emoticon", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_delete_custom_emoticon(int64_t handle, const char* md5, char** outJson) {
    if (!md5 || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto emoDb = weflow::ResolveDbByKindOrPath(*ctx, "emoticon");
        if (!emoDb) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"emoticon_db_not_found\"}");
            return 0;
        }
        WCDB::Handle dbHandle = emoDb->getHandle();
        std::string sql = "DELETE FROM kNonStoreEmoticonTable WHERE lower(md5) = lower('" +
                          weflow::EscapeSqlLiteral(md5) + "')";
        bool ok = dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql));
        *outJson = weflow::AllocCString(ok ? "{\"success\":true}" : "{\"success\":false}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("delete_custom_emoticon", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_update_custom_emoticon(
    int64_t handle, const char* md5, const char* patchJson, char** outJson) {
    if (!md5 || !patchJson || !outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto emoDb = weflow::ResolveDbByKindOrPath(*ctx, "emoticon");
        if (!emoDb) {
            *outJson = weflow::AllocCString("{\"success\":false,\"error\":\"emoticon_db_not_found\"}");
            return 0;
        }
        std::string j(patchJson);
        std::string cdnUrl = weflow::ParseJsonStringField(j, "cdn_url");
        WCDB::Handle dbHandle = emoDb->getHandle();
        std::string sql = "UPDATE kNonStoreEmoticonTable SET cdn_url = '" +
                          weflow::EscapeSqlLiteral(cdnUrl) + "' WHERE lower(md5) = lower('" +
                          weflow::EscapeSqlLiteral(md5) + "')";
        bool ok = dbHandle.asHandleOperation()->execute(weflow::MakeRawStatement(sql));
        *outJson = weflow::AllocCString(ok ? "{\"success\":true}" : "{\"success\":false}");
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("update_custom_emoticon", -2, e.what());
        return -2;
    }
}

// ============================================================================
// Emoticon Captions & Voice Data Batch
// ============================================================================

WCDB_API int32_t wcdb_get_emoticon_caption(
    int64_t handle, const char* dbPath, const char* md5, char** outCaption) {
    if (!outCaption) return -1;
    *outCaption = nullptr;
    if (!md5 || md5[0] == '\0') {
        *outCaption = weflow::AllocCString("");
        return 0;
    }
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        std::string target = (dbPath && dbPath[0] != '\0') ? std::string(dbPath) : "emoticon";
        auto emoDb = weflow::ResolveDbByKindOrPath(*ctx, target);
        if (!emoDb) {
            *outCaption = weflow::AllocCString("");
            return 0;
        }
        WCDB::Handle dbHandle = emoDb->getHandle();
        std::string sql = "SELECT caption_ FROM kStoreEmoticonCaptionsTable WHERE lower(md5_) = lower('" +
                          weflow::EscapeSqlLiteral(md5) + "') LIMIT 1";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        std::string caption;
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                WCDB::Value v = dbHandle.getValue(0);
                if (!v.isNull()) caption = static_cast<std::string>(v.textValue());
            }
            dbHandle.finalize();
        }
        *outCaption = weflow::AllocCString(caption);
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_emoticon_caption", -2, e.what());
        *outCaption = weflow::AllocCString("");
        return 0;
    }
}

WCDB_API int32_t wcdb_get_emoticon_caption_strict(
    int64_t handle, const char* md5, char** outCaption) {
    return wcdb_get_emoticon_caption(handle, nullptr, md5, outCaption);
}

WCDB_API int32_t wcdb_get_voice_data_batch(int64_t handle, const char* requestsJson, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    if (!requestsJson) {
        *outJson = weflow::AllocCString("[]");
        return 0;
    }
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto reqs = weflow::ParseJsonObjectArray(requestsJson);
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < reqs.size(); ++i) {
            if (i > 0) oss << ",";
            const auto& obj = reqs[i];
            int64_t localId = weflow::ParseJsonIntField(obj, "local_id", 0);
            std::string svrIdStr = weflow::ParseJsonStringField(obj, "svr_id");
            int64_t svrId = svrIdStr.empty() ? weflow::ParseJsonIntField(obj, "svr_id", 0) : std::strtoll(svrIdStr.c_str(), nullptr, 10);

            std::vector<uint8_t> bytes;
            int64_t dataSize = 0, createTime = 0;
            bool found = weflow::QueryVoiceRecord(*ctx, svrId, localId, bytes, dataSize, createTime);
            oss << "{\"index\":" << i;
            if (found && !bytes.empty()) {
                oss << ",\"hex\":\"" << weflow::ToHex(bytes) << "\"";
            }
            oss << "}";
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_voice_data_batch", -2, e.what());
        return -2;
    }
}

// ============================================================================
// Moments / SNS (sns.db -> SnsTimeLine)
// ============================================================================

WCDB_API int32_t wcdb_get_sns_timeline(
    int64_t handle,
    int32_t limit,
    int32_t offset,
    const char* username,
    const char* keyword,
    int32_t startTime,
    int32_t endTime,
    char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        if (!snsDb) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }

        std::vector<std::string> usernames;
        if (username && username[0] != '\0') {
            if (username[0] == '[') {
                usernames = weflow::ParseJsonStringArray(username);
            } else {
                usernames.push_back(username);
            }
        }

        WCDB::Handle dbHandle = snsDb->getHandle();
        std::string tidCol = "tid";
        std::string userCol = "user_name";
        std::string contentCol = "content";
        std::string timeCol = "";

        {
            WCDB::Statement s = weflow::MakeRawStatement("PRAGMA table_info(SnsTimeLine)");
            if (dbHandle.prepare(s)) {
                while (dbHandle.step()) {
                    WCDB::Value v = dbHandle.getValue(1);
                    if (!v.isNull()) {
                        std::string cname = static_cast<std::string>(v.textValue());
                        std::string lc = weflow::ToLower(cname);
                        if (lc == "username") userCol = cname;
                        else if (lc == "user_name") userCol = cname;
                        else if (lc == "createtime" || lc == "create_time") timeCol = cname;
                    }
                }
                dbHandle.finalize();
            }
        }

        std::ostringstream sql;
        sql << "SELECT " << tidCol << ", " << userCol << ", " << contentCol;
        if (!timeCol.empty()) sql << ", " << timeCol;
        sql << " FROM SnsTimeLine WHERE 1=1";

        if (!usernames.empty()) {
            sql << " AND " << userCol << " IN (";
            for (size_t i = 0; i < usernames.size(); ++i) {
                if (i > 0) sql << ",";
                sql << "'" << weflow::EscapeSqlLiteral(usernames[i]) << "'";
            }
            sql << ")";
        }
        if (!timeCol.empty()) {
            if (startTime > 0) sql << " AND " << timeCol << " >= " << startTime;
            if (endTime > 0) sql << " AND " << timeCol << " <= " << endTime;
        }
        sql << " ORDER BY " << tidCol << " DESC";

        int32_t lim = (limit > 0) ? limit : 20;
        int32_t off = (offset >= 0) ? offset : 0;
        bool filterMemory = (keyword && keyword[0] != '\0') || (timeCol.empty() && (startTime > 0 || endTime > 0));
        if (!filterMemory) {
            sql << " LIMIT " << lim << " OFFSET " << off;
        }

        WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
        std::vector<std::string> postJsonList;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                std::string tidStr;
                WCDB::Value v0 = dbHandle.getValue(0);
                if (v0.getType() == WCDB::Value::Type::Integer) tidStr = std::to_string(v0.intValue());
                else if (!v0.isNull()) tidStr = static_cast<std::string>(v0.textValue());

                std::string uname = dbHandle.getValue(1).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(1).textValue());

                std::string rawXml;
                WCDB::Value cv = dbHandle.getValue(2);
                if (cv.getType() == WCDB::Value::Type::BLOB) {
                    WCDB::Data d = cv.blobValue();
                    std::string decomp;
                    if (weflow::TryDecompressZstd(d.buffer(), d.size(), decomp)) rawXml = std::move(decomp);
                    else rawXml.assign(reinterpret_cast<const char*>(d.buffer()), d.size());
                } else if (!cv.isNull()) {
                    std::string t = static_cast<std::string>(cv.textValue());
                    std::string decomp;
                    if (weflow::TryDecompressZstd(t.data(), t.size(), decomp)) rawXml = std::move(decomp);
                    else rawXml = std::move(t);
                }

                int64_t fallbackTime = 0;
                if (!timeCol.empty()) {
                    fallbackTime = dbHandle.getValue(3).intValue();
                }

                if (startTime > 0 || endTime > 0) {
                    std::string ctStr = weflow::ExtractXmlTag(rawXml, "createTime");
                    int64_t ct = ctStr.empty() ? fallbackTime : std::strtoll(ctStr.c_str(), nullptr, 10);
                    if (startTime > 0 && ct < startTime) continue;
                    if (endTime > 0 && ct > endTime) continue;
                }

                if (keyword && keyword[0] != '\0') {
                    std::string kw(keyword);
                    std::string desc = weflow::ExtractXmlTag(rawXml, "contentDesc");
                    if (desc.find(kw) == std::string::npos && rawXml.find(kw) == std::string::npos) {
                        continue;
                    }
                }

                postJsonList.push_back(weflow::SerializeSnsPost(tidStr, uname, rawXml, fallbackTime));
            }
            dbHandle.finalize();
        }

        std::ostringstream oss;
        oss << "[";
        size_t startIdx = filterMemory ? static_cast<size_t>(off) : 0;
        size_t count = 0;
        for (size_t i = startIdx; i < postJsonList.size() && count < static_cast<size_t>(lim); ++i, ++count) {
            if (count > 0) oss << ",";
            oss << postJsonList[i];
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_sns_timeline", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_sns_feed(
    int64_t handle,
    int32_t limit,
    int32_t offset,
    const char* usernamesJson,
    const char* keyword,
    int64_t startTime,
    int64_t endTime,
    char** outJson) {
    return wcdb_get_sns_timeline(handle, limit, offset, usernamesJson, keyword, static_cast<int32_t>(startTime), static_cast<int32_t>(endTime), outJson);
}

WCDB_API int32_t wcdb_get_sns_count(
    int64_t handle,
    const char* usernamesJson,
    const char* keyword,
    int64_t /*startTime*/,
    int64_t /*endTime*/,
    int32_t* outCount) {
    if (!outCount) return -1;
    *outCount = 0;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        if (!snsDb) return 0;
        auto usernames = weflow::ParseJsonStringArray(usernamesJson);
        std::ostringstream sql;
        sql << "SELECT COUNT(*) FROM SnsTimeLine WHERE 1=1";
        if (!usernames.empty()) {
            sql << " AND user_name IN (";
            for (size_t i = 0; i < usernames.size(); ++i) {
                if (i > 0) sql << ",";
                sql << "'" << weflow::EscapeSqlLiteral(usernames[i]) << "'";
            }
            sql << ")";
        }
        if (keyword && keyword[0] != '\0') {
            sql << " AND content LIKE '%" << weflow::EscapeSqlLiteral(keyword) << "%'";
        }
        WCDB::Handle dbHandle = snsDb->getHandle();
        WCDB::Statement stmt = weflow::MakeRawStatement(sql.str());
        if (dbHandle.prepare(stmt)) {
            if (dbHandle.step()) {
                *outCount = static_cast<int32_t>(dbHandle.getValue(0).intValue());
            }
            dbHandle.finalize();
        }
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_sns_count", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_search_sns(
    int64_t handle, const char* keyword, int32_t limit, int32_t offset, char** outJson) {
    return wcdb_get_sns_feed(handle, limit, offset, "[]", keyword, 0, 0, outJson);
}

WCDB_API int32_t wcdb_get_sns_Bounds(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        int64_t minTid = 0, maxTid = 0, cnt = 0;
        if (snsDb) {
            WCDB::Handle dbHandle = snsDb->getHandle();
            WCDB::Statement stmt = weflow::MakeRawStatement("SELECT MIN(tid), MAX(tid), COUNT(*) FROM SnsTimeLine");
            if (dbHandle.prepare(stmt)) {
                if (dbHandle.step()) {
                    minTid = dbHandle.getValue(0).intValue();
                    maxTid = dbHandle.getValue(1).intValue();
                    cnt = dbHandle.getValue(2).intValue();
                }
                dbHandle.finalize();
            }
        }
        std::ostringstream oss;
        oss << "{\"min_tid\":" << minTid << ",\"max_tid\":" << maxTid << ",\"count\":" << cnt << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_sns_Bounds", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_sns_usernames(int64_t handle, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        if (!snsDb) {
            *outJson = weflow::AllocCString("[]");
            return 0;
        }
        WCDB::Handle dbHandle = snsDb->getHandle();
        std::string uCol = "user_name";
        {
            WCDB::Statement s = weflow::MakeRawStatement("PRAGMA table_info(SnsTimeLine)");
            if (dbHandle.prepare(s)) {
                while (dbHandle.step()) {
                    WCDB::Value v = dbHandle.getValue(1);
                    if (!v.isNull()) {
                        std::string c = weflow::ToLower(static_cast<std::string>(v.textValue()));
                        if (c == "username") uCol = "userName";
                    }
                }
                dbHandle.finalize();
            }
        }
        WCDB::Statement stmt = weflow::MakeRawStatement(
            "SELECT DISTINCT " + uCol + " FROM SnsTimeLine WHERE " + uCol + " IS NOT NULL AND " + uCol + " != ''");
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                WCDB::Value v = dbHandle.getValue(0);
                if (!v.isNull()) {
                    if (!first) oss << ",";
                    first = false;
                    oss << "\"" << weflow::EscapeJson(static_cast<std::string>(v.textValue())) << "\"";
                }
            }
            dbHandle.finalize();
        }
        oss << "]";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_sns_usernames", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_sns_export_stats(int64_t handle, const char* myWxid, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        if (!snsDb) {
            *outJson = weflow::AllocCString("{\"total_posts\":0,\"total_friends\":0,\"my_posts\":null}");
            return 0;
        }
        WCDB::Handle dbHandle = snsDb->getHandle();
        std::string uCol = "user_name";
        {
            WCDB::Statement s = weflow::MakeRawStatement("PRAGMA table_info(SnsTimeLine)");
            if (dbHandle.prepare(s)) {
                while (dbHandle.step()) {
                    WCDB::Value v = dbHandle.getValue(1);
                    if (!v.isNull()) {
                        std::string c = weflow::ToLower(static_cast<std::string>(v.textValue()));
                        if (c == "username") uCol = "userName";
                    }
                }
                dbHandle.finalize();
            }
        }
        int64_t totalPosts = 0;
        int64_t totalFriends = 0;
        WCDB::Statement s1 = weflow::MakeRawStatement("SELECT COUNT(*), COUNT(DISTINCT " + uCol + ") FROM SnsTimeLine WHERE " + uCol + " IS NOT NULL AND " + uCol + " != ''");
        if (dbHandle.prepare(s1)) {
            if (dbHandle.step()) {
                totalPosts = dbHandle.getValue(0).intValue();
                totalFriends = dbHandle.getValue(1).intValue();
            }
            dbHandle.finalize();
        }

        std::string targetWxid = (myWxid && myWxid[0] != '\0') ? std::string(myWxid) : ctx->my_wxid;
        std::ostringstream oss;
        oss << "{\"total_posts\":" << totalPosts
            << ",\"total_friends\":" << totalFriends;
        if (!targetWxid.empty()) {
            int64_t myPosts = 0;
            WCDB::Statement s2 = weflow::MakeRawStatement(
                "SELECT COUNT(*) FROM SnsTimeLine WHERE " + uCol + " = '" + weflow::EscapeSqlLiteral(targetWxid) + "'");
            if (dbHandle.prepare(s2)) {
                if (dbHandle.step()) myPosts = dbHandle.getValue(0).intValue();
                dbHandle.finalize();
            }
            oss << ",\"my_posts\":" << myPosts;
        } else {
            oss << ",\"my_posts\":null";
        }
        oss << "}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_sns_export_stats", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_sns_annual_stats(
    int64_t handle, int32_t begin, int32_t end, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;

    try {
        auto snsDb = weflow::ResolveDbByKindOrPath(*ctx, "sns");
        if (!snsDb) {
            *outJson = weflow::AllocCString("{\"totalPosts\":0,\"typeCounts\":{},\"topLikers\":[],\"topLiked\":[]}");
            return 0;
        }
        WCDB::Handle dbHandle = snsDb->getHandle();
        std::string uCol = "user_name";
        {
            WCDB::Statement s = weflow::MakeRawStatement("PRAGMA table_info(SnsTimeLine)");
            if (dbHandle.prepare(s)) {
                while (dbHandle.step()) {
                    WCDB::Value v = dbHandle.getValue(1);
                    if (!v.isNull()) {
                        std::string c = weflow::ToLower(static_cast<std::string>(v.textValue()));
                        if (c == "username") uCol = "userName";
                    }
                }
                dbHandle.finalize();
            }
        }

        std::string myWxid = ctx->my_wxid;
        int64_t totalPosts = 0;
        std::map<int32_t, int64_t> typeCounts;
        std::map<std::string, int64_t> likerCounts;
        std::map<std::string, int64_t> likedCounts;

        std::string sql = "SELECT " + uCol + ", content FROM SnsTimeLine";
        WCDB::Statement stmt = weflow::MakeRawStatement(sql);
        if (dbHandle.prepare(stmt)) {
            while (dbHandle.step()) {
                std::string author = dbHandle.getValue(0).isNull() ? "" : static_cast<std::string>(dbHandle.getValue(0).textValue());
                std::string rawXml;
                WCDB::Value cv = dbHandle.getValue(1);
                if (cv.getType() == WCDB::Value::Type::BLOB) {
                    WCDB::Data d = cv.blobValue();
                    std::string decomp;
                    if (weflow::TryDecompressZstd(d.buffer(), d.size(), decomp)) rawXml = std::move(decomp);
                    else rawXml.assign(reinterpret_cast<const char*>(d.buffer()), d.size());
                } else if (!cv.isNull()) {
                    std::string t = static_cast<std::string>(cv.textValue());
                    std::string decomp;
                    if (weflow::TryDecompressZstd(t.data(), t.size(), decomp)) rawXml = std::move(decomp);
                    else rawXml = std::move(t);
                }

                std::string cTimeStr = weflow::ExtractXmlTag(rawXml, "createTime");
                int64_t cTime = cTimeStr.empty() ? 0 : std::strtoll(cTimeStr.c_str(), nullptr, 10);
                if (begin > 0 && cTime < begin) continue;
                if (end > 0 && cTime > end) continue;

                std::string styleStr = weflow::ExtractXmlTag(rawXml, "contentStyle");
                int32_t postType = styleStr.empty() ? 1 : static_cast<int32_t>(std::strtol(styleStr.c_str(), nullptr, 10));

                bool isMyPost = (!myWxid.empty() && author == myWxid);
                if (isMyPost || myWxid.empty()) {
                    totalPosts++;
                    typeCounts[postType]++;
                }

                // 统计点赞
                std::string likeXml = weflow::ExtractXmlTag(rawXml, "likeUserList");
                if (!likeXml.empty()) {
                    size_t lpos = 0;
                    while (true) {
                        size_t nextEnd = 0;
                        std::string userXml = weflow::ExtractXmlTag(likeXml, "User", lpos, &nextEnd);
                        if (userXml.empty() && nextEnd == 0) break;
                        lpos = (nextEnd > lpos) ? nextEnd : (lpos + 6);
                        std::string lu = weflow::ExtractXmlTag(userXml, "username");
                        if (!lu.empty()) {
                            if (isMyPost && lu != myWxid) {
                                likerCounts[lu]++;
                            }
                            if (!isMyPost && lu == myWxid && !author.empty()) {
                                likedCounts[author]++;
                            }
                        }
                    }
                }
            }
            dbHandle.finalize();
        }

        auto getTop10 = [](const std::map<std::string, int64_t>& m) {
            std::vector<std::pair<std::string, int64_t>> vec(m.begin(), m.end());
            std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
            if (vec.size() > 10) vec.resize(10);
            return vec;
        };
        auto topLikers = getTop10(likerCounts);
        auto topLiked = getTop10(likedCounts);

        std::ostringstream oss;
        oss << "{\"totalPosts\":" << totalPosts << ",\"typeCounts\":{";
        bool firstType = true;
        for (const auto& [t, cnt] : typeCounts) {
            if (!firstType) oss << ",";
            firstType = false;
            oss << "\"" << t << "\":" << cnt;
        }
        oss << "},\"topLikers\":[";
        for (size_t i = 0; i < topLikers.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "{\"username\":\"" << weflow::EscapeJson(topLikers[i].first) << "\","
                << "\"count\":" << topLikers[i].second << "}";
        }
        oss << "],\"topLiked\":[";
        for (size_t i = 0; i < topLiked.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "{\"username\":\"" << weflow::EscapeJson(topLiked[i].first) << "\","
                << "\"count\":" << topLiked[i].second << "}";
        }
        oss << "]}";
        *outJson = weflow::AllocCString(oss.str());
        return 0;
    } catch (const std::exception& e) {
        weflow::SetLastError("get_sns_annual_stats", -2, e.what());
        return -2;
    }
}

WCDB_API int32_t wcdb_get_sns_Notification(int64_t handle, int32_t /*limit*/, int32_t /*offset*/, char** outJson) {
    if (!outJson) return -1;
    *outJson = nullptr;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;
    *outJson = weflow::AllocCString("[]");
    return 0;
}

// ============================================================================
// AI Query Helpers & Stubs (wcdb_ai_*)
// ============================================================================

WCDB_API int32_t wcdb_ai_query_timeline(int64_t handle, const char* queryJson, char** outJson) {
    if (!queryJson || !outJson) return -1;
    std::string sid = weflow::ParseJsonStringField(queryJson, "session_id");
    return wcdb_get_session_timeline(handle, sid.c_str(), outJson);
}

WCDB_API int32_t wcdb_ai_query_topic_stats(int64_t handle, const char* queryJson, char** outJson) {
    if (!queryJson || !outJson) return -1;
    std::string sid = weflow::ParseJsonStringField(queryJson, "session_id");
    int64_t st = weflow::ParseJsonIntField(queryJson, "start_time", 0);
    int64_t et = weflow::ParseJsonIntField(queryJson, "end_time", 0);
    return wcdb_get_aggregate_stats(handle, sid.c_str(), st, et, outJson);
}

WCDB_API int32_t wcdb_ai_query_source_refs(int64_t handle, const char* /*queryJson*/, char** outJson) {
    if (!outJson) return -1;
    auto ctx = weflow::GetAccount(handle);
    if (!ctx) return -1;
    *outJson = weflow::AllocCString("[]");
    return 0;
}

WCDB_API int32_t wcdb_ai_get_recent_messages(int64_t handle, const char* sessionId, int32_t limit, char** outJson) {
    return wcdb_get_messages(handle, sessionId, limit, 0, outJson);
}

WCDB_API int32_t wcdb_ai_get_messages_before(
    int64_t handle, const char* sessionId, int64_t beforeSeqOrTime, int32_t limit, char** outJson) {
    return wcdb_get_messages_around_anchor(handle, sessionId, 0, beforeSeqOrTime, INT64_MAX, limit, 0, 0, outJson);
}

WCDB_API int32_t wcdb_ai_get_messages_after(
    int64_t handle, const char* sessionId, int64_t afterSeqOrTime, int32_t limit, char** outJson) {
    return wcdb_get_messages_around_anchor(handle, sessionId, 0, afterSeqOrTime, 0, 0, limit, 0, outJson);
}

WCDB_API int32_t wcdb_ai_get_message_context(
    int64_t handle, const char* sessionId, int64_t localId, int32_t radius, char** outJson) {
    return wcdb_get_messages_around_anchor(handle, sessionId, 0, 0, localId, radius, radius, 1, outJson);
}

// Observed in disassembly @ 0xa99e0..0xa99f0: 4-byte stubs returning -1 (`mov w0, #-1; ret`)
WCDB_API int32_t wcdb_ai_get_search_message_context(void) { return -1; }
WCDB_API int32_t wcdb_ai_get_conversation_between(void) { return -1; }
WCDB_API int32_t wcdb_ai_search_sessions(void) { return -1; }
WCDB_API int32_t wcdb_ai_get_session_messages(void) { return -1; }
WCDB_API int32_t wcdb_ai_get_session_summaries(void) { return -1; }

} // extern "C"
