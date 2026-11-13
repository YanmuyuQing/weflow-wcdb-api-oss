#!/usr/bin/env python3
"""Stage the WeFlow 6.3.1 Moments media recovery patch."""

import argparse
import json
from pathlib import Path

from patch_weflow_sns_deleted_badges import RENDERER_ENTRY
from patch_weflow_sns_thumbnail_urls import MAIN_ENTRY, sha256, stage_resized_entries


MAIN_HASH = "90b3811ca27bf89b0f54937377b031ba6d5d5edfb88686677f50734960b87935"
RENDERER_HASH = "5b3f4fa314cb860a1bb6d97616cf4366b57da71e482cdfcc9fcf26ec03d9df9f"

# The old client can leave a video or cover on disk XORed with the wrong seed.
# A cache hit needs the same format checks as a fresh download.
VIDEO_HELPERS = r'''
function snsVideoContentType(data) {
  const type = $A(data, "");
  if (type.startsWith("image/")) return type;
  if (data.length >= 16 && data.toString("ascii", 4, 8) === "ftyp" &&
      data.readUInt32BE(0) >= 16 && data.readUInt32BE(0) <= data.length) {
    return "video/mp4";
  }
  return "";
}
async function snsXorVideo(data, key) {
  if (!/^\d+$/.test(String(key ?? ""))) return data;
  let stream;
  try {
    stream = await XA.getInstance().getKeystream(String(key), 131072);
  } catch {
    stream = new ZA(key).generateKeystreamBE(131072);
  }
  const result = Buffer.from(data);
  for (let index = 0; index < Math.min(result.length, stream.length); index++) {
    result[index] ^= stream[index];
  }
  return result;
}
async function snsRecoverCachedVideo(data, key, previousKey) {
  if (snsVideoContentType(data)) return data;
  if (previousKey && previousKey !== key) {
    const raw = await snsXorVideo(data, previousKey);
    if (snsVideoContentType(raw)) return raw;
    const recovered = await snsXorVideo(raw, key);
    if (snsVideoContentType(recovered)) return recovered;
  }
  return snsXorVideo(data, key);
}
async function snsSaveVideo(cachePath, data) {
  const pending = cachePath + "." + o.default.randomUUID() + ".tmp";
  try {
    await (0,v.writeFile)(pending, data);
    await (0,v.rename)(pending, cachePath);
  } finally {
    await (0,v.unlink)(pending).catch(() => {});
  }
}
'''

MEDIA_REQUEST = r'''
async function snsRequestMedia(url, alternate, key, previousKey, cancelled = () => false) {
  const request = async address => {
    if (!address) return {success: false};
    try {
      return await window.electronAPI.sns.proxyImage({url: address, key, previousKey});
    } catch {
      return {success: false};
    }
  };
  const primary = await request(url);
  if (primary.success && (primary.dataUrl || primary.videoPath)) return primary;
  if (cancelled() || !alternate || alternate === url) return primary;
  const fallback = await request(alternate);
  if (fallback.success && (fallback.dataUrl || fallback.videoPath)) return fallback;
  // Preserve a transient error so the existing retry path still runs.
  return [404, 410].includes(fallback.status) && ![404, 410].includes(primary.status)
    ? primary
    : fallback;
}
'''


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Unexpected WeFlow 6.3.1 code near {old[:85]!r}")
    return source.replace(old, new, 1)


def patch_main(payload):
    if sha256(payload) != MAIN_HASH:
        raise ValueError("Expected the WeFlow 6.3.1 main bundle with signed thumbnail paths preserved")
    source = payload.decode("utf-8")
    source = replace_once(source, ";function nj(e)", ";" + VIDEO_HELPERS + "function nj(e)")
    # Keep the video decryptor's JS fallback aligned with the WASM ISAAC64.
    source = replace_once(source, "11400714819323198485n", "0x9e3779b97f4a7c13n")
    source = replace_once(source, "Number(n>>3n)&255", "Number(n>>3n&255n)")
    source = replace_once(source, "Number(r>>11n)&255", "Number(r>>11n&255n)")
    source = replace_once(source, 'let r=e.replace(`http://`,`https://`);',
                          r'let r=e.replace(/^<!\[CDATA\[([\s\S]*?)\]\]>$/,`$1`).replace(/&(?:amp|#38|#x26);/gi,`&`).replace(`http://`,`https://`);')
    source = replace_once(source, 'key:n&&i||e.key,encIdx:',
                          'key:(n||ej(e.url))&&i||e.key,previousKey:!n&&ej(e.url)&&i&&i!==e.key?e.key:void 0,encIdx:')
    source = replace_once(source, "async proxyImage(e,t){", "async proxyImage(e,t,previousKey){")
    source = replace_once(source, "let i=await this.fetchAndDecryptImage(e,t);if(i.success)",
                          "let i=await this.fetchAndDecryptImage(e,t,previousKey);if(i.success)")
    source = replace_once(source, "this.fetchAndDecryptImage(e.url,e.key)",
                          "this.fetchAndDecryptImage(e.url,e.key,e.media.previousKey)")
    source = replace_once(source, 'return oj.proxyImage(n,r)}',
                          'return oj.proxyImage(n,r,typeof t===`string`?void 0:t?.previousKey)}')
    source = replace_once(source, "async fetchAndDecryptImage(e,n){", "async fetchAndDecryptImage(e,n,previousKey){")
    source = replace_once(
        source,
        'if(r)return{success:!0,cachePath:i,contentType:`video/mp4`};let e=await(0,v.readFile)(i);',
        '''let e=await(0,v.readFile)(i);
        if (r) {
          const recovered = await snsRecoverCachedVideo(e, n, previousKey);
          const contentType = snsVideoContentType(recovered);
          if (contentType) {
            if (recovered !== e) await snsSaveVideo(i, recovered);
            return {success: true, data: recovered, cachePath: i, contentType};
          }
        } else ''',
    )
    source = replace_once(source, 'try{(0,c.unlinkSync)(i)}catch{}}catch(e){console.warn(`[SnsService] 读取缓存失败:',
                          'if(!r)try{(0,c.unlinkSync)(i)}catch{}}catch(e){console.warn(`[SnsService] 读取缓存失败:')
    start = source.index('let e=await(0,v.readFile)(a);if(n&&String(n).trim().length>0)')
    end = source.index('}catch(e){console.error(`[SnsService] 视频处理失败:`', start)
    source = source[:start] + '''
        const raw = await (0,v.readFile)(a);
        const data = snsVideoContentType(raw) ? raw : await snsXorVideo(raw, n);
        const contentType = snsVideoContentType(data);
        if (!contentType) {
          r({success: false, error: "无法识别朋友圈视频或封面格式"});
          return;
        }
        try {
          await snsSaveVideo(i, data);
          r({success: true, data, cachePath: i, contentType});
        } catch (error) {
          if (contentType.startsWith("image/")) r({success: true, data, contentType});
          else throw error;
        }
    ''' + source[end:]
    source = replace_once(source, 'r({success:!1,error:e.message})}})});u.on(`error`',
                          'r({success:!1,error:e.message})}finally{await(0,v.unlink)(a).catch(()=>{})}})});u.on(`error`')
    return source.encode("utf-8")


def patch_renderer(payload):
    if sha256(payload) != RENDERER_HASH:
        raise ValueError("Expected the WeFlow 6.3.1 renderer with false deletion badges repaired")
    source = payload.decode("utf-8")
    source = replace_once(source, ";var y=", ";" + MEDIA_REQUEST + "var y=")
    source = replace_once(source,
        "let n=await window.electronAPI.sns.proxyImage({url:e.url,key:I?void 0:e.key});if(t)return;",
        """let n=await snsRequestMedia(e.url,e.thumb,I?void 0:e.key,I?void 0:e.previousKey,()=>t);
        if(t)return;
        if(n.success&&n.dataUrl){y(n.dataUrl);M(!1);d(!1);return}
        """)
    source = replace_once(source,
        "let n=await window.electronAPI.sns.proxyImage({url:F,key:I?void 0:e.key});if(t)return;",
        "let n=await snsRequestMedia(F,e.url,I?void 0:e.key,I?void 0:e.previousKey,()=>t);if(t)return;")
    source = replace_once(source,
        "let t=await window.electronAPI.sns.proxyImage({url:e.url,key:I?void 0:e.key});if(t.success&&t.videoPath)",
        "let t=await window.electronAPI.sns.proxyImage({url:e.url,key:I?void 0:e.key,previousKey:I?void 0:e.previousKey});if(t.success&&t.videoPath)")
    source = replace_once(source,
        "S(e),n(e,!0,void 0)}else alert(`视频解密失败`)",
        "S(e),n(e,!0,void 0)}else if(t.success&&t.dataUrl)n(t.dataUrl,!1,void 0);else if(_.startsWith(`data:image/`))n(_,!1,void 0);else alert(`视频解密失败`)")
    source = replace_once(source,
        "let t=await window.electronAPI.sns.proxyImage({url:e.url,key:I?void 0:e.key});if(t.success){let e=document.createElement(`a`);",
        "let t=await snsRequestMedia(e.url,e.thumb,I?void 0:e.key,I?void 0:e.previousKey);if(t.success&&(t.dataUrl||t.videoPath)){let e=document.createElement(`a`);")
    source = replace_once(source,
        '${N?`mp4`:`jpg`}',
        r'${t.videoPath?`mp4`:t.dataUrl?.match(/^data:image\/([a-z0-9]+);/i)?.[1]||`jpg`}')
    return source.encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=Path("/Applications/WeFlow.app"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check and args.output_dir is None:
        parser.error("--output-dir is required unless --check is used")
    contents = args.app / "Contents"
    result = stage_resized_entries(
        contents / "Resources/app.asar", contents / "Info.plist", args.output_dir,
        {MAIN_ENTRY: patch_main, RENDERER_ENTRY: patch_renderer}, args.check,
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
