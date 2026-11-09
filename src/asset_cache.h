// Host-side "ready file" cache for decoded RSC5 resource bodies.
//
// WHY (2026-10-31). Every RSC5 member the guest opens is decoded at serve time
// (src/xmem_lzx.h, the frame-chain rule). Since the non-UI decode was admitted
// (RSC5-DECODE-NONUI), a boot serves thousands of members, and the same
// container is decoded again on every open attempt -- measured in
// build/rsc5a.log: 8,538 successful decodes for 2,845 DISTINCT members.
//
// WHAT THIS IS. A cache of the DECODED BODY, keyed by the member path ALONE
// (owner decision 2026-10-31: path-only key, no hash of the archive). The
// decoded body is a pure function of the container bytes, and the container is
// fixed retail content, so a path identifies it. The blob's length is checked
// against the container's own declared length on every load, and a mismatch is
// a MISS (logged ASSET-CACHE-BAD), never a served body: a stale or truncated
// file cannot reach the guest.
//
// WHAT IT IS NOT. Not a substitute for the container, not a guest-visible
// allocation, and not a new source of truth: on a miss the decode runs exactly
// as before and the result is written here for the next open. First launch is
// therefore a self-populating pass; later launches serve from disk.
//
// LAYOUT.  <cache dir>/blobs/<sanitised path>.bin
// The sanitiser is the SAME rule tools/rsc5_precache.py uses, so a cache built
// offline is byte-identical to one built by a boot:
//   runs of characters outside [A-Za-z0-9._-]  ->  "__"
// The cache dir is resolved at call time, in this order:
//   $MCLA_ASSET_CACHE  ->  <exe dir>/asset_cache  ->  build/asset_cache
#ifndef MCLA_ASSET_CACHE_H_
#define MCLA_ASSET_CACHE_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace mcla_asset_cache {

inline const char* DirOverride() {
  const char* e = std::getenv("MCLA_ASSET_CACHE");
  return (e && *e) ? e : nullptr;
}

// Directory the cache lives in. Resolved once (the answer cannot change during
// a run) and created if missing.
inline const std::string& Directory() {
  static std::string dir = []() -> std::string {
    std::string d;
    if (const char* o = DirOverride())
      d = o;
    if (d.empty()) {
#if defined(_WIN32)
      char exe[4096] = {0};
      const DWORD n = GetModuleFileNameA(nullptr, exe, (DWORD)sizeof(exe) - 1);
      if (n > 0 && n < sizeof(exe)) {
        std::string p(exe, n);
        const size_t cut = p.find_last_of("\\/");
        if (cut != std::string::npos) {
          const std::string beside = p.substr(0, cut) + "\\asset_cache";
          const DWORD attr = GetFileAttributesA(beside.c_str());
          if (attr != INVALID_FILE_ATTRIBUTES &&
              (attr & FILE_ATTRIBUTE_DIRECTORY))
            d = beside;
          else
            d = p.substr(0, cut) + "\\build\\asset_cache";
        }
      }
#endif
    }
    if (d.empty())
      d = "build/asset_cache";
    return d;
  }();
  return dir;
}

// Same rule as tools/rsc5_precache.py:sanitise().
inline std::string Sanitise(const char* path) {
  std::string out;
  const char* p = path ? path : "";
  size_t run = 0;
  for (; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                      c == '-';
    if (keep) {
      if (run) {
        out += "__";
        run = 0;
      }
      out.push_back((char)c);
    } else {
      ++run;
    }
  }
  if (run)
    out += "__";
  return out;
}

inline std::string BlobPath(const char* path) {
  return Directory() + "\\blobs\\" + Sanitise(path) + ".bin";
}

inline bool MakeDir(const std::string& d) {
#if defined(_WIN32)
  if (CreateDirectoryA(d.c_str(), nullptr))
    return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
#else
  return mkdir(d.c_str(), 0777) == 0;
#endif
}

inline void EnsureDirs() {
  MakeDir(Directory());
  MakeDir(Directory() + "\\blobs");
}

// Load a cached body. `expected` is the container's own declared length; a blob
// of any other size is refused and reported, so the caller decodes instead.
inline bool Load(const char* path, uint32_t expected, std::vector<uint8_t>& out,
                 bool* sizeMismatch = nullptr) {
  if (sizeMismatch)
    *sizeMismatch = false;
  if (!path || expected == 0)
    return false;
  const std::string f = BlobPath(path);
  std::FILE* fp = std::fopen(f.c_str(), "rb");
  if (!fp)
    return false;
  std::fseek(fp, 0, SEEK_END);
  const long sz = std::ftell(fp);
  if (sz < 0 || (uint64_t)sz != (uint64_t)expected) {
    std::fclose(fp);
    if (sizeMismatch)
      *sizeMismatch = true;
    return false;
  }
  out.resize((size_t)sz);
  std::fseek(fp, 0, SEEK_SET);
  const size_t got = std::fread(out.data(), 1, out.size(), fp);
  std::fclose(fp);
  if (got != out.size()) {
    out.clear();
    return false;
  }
  return true;
}

// Store a decoded body. Best effort: a read-only or full disk must never affect
// the boot, so the result is only used for the marker.
inline bool Store(const char* path, const std::vector<uint8_t>& body) {
  if (!path || body.empty())
    return false;
  EnsureDirs();
  const std::string f = BlobPath(path);
  std::FILE* fp = std::fopen(f.c_str(), "wb");
  if (!fp)
    return false;
  const size_t put = std::fwrite(body.data(), 1, body.size(), fp);
  std::fclose(fp);
  return put == body.size();
}

inline bool Ready() {
#if defined(_WIN32)
  const DWORD attr = GetFileAttributesA((Directory() + "\\blobs").c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  return true;
#endif
}

}  // namespace mcla_asset_cache

#endif  // MCLA_ASSET_CACHE_H_
