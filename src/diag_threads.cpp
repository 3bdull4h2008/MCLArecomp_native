#include "diag_threads.h"
#include "logging.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mcla::diag {
namespace {
bool Env(const char* n) { const char* e = std::getenv(n); return e && e[0] == '1'; }
std::mutex g_mu;
std::vector<std::pair<std::string, std::function<std::string()>>> g_growth;

std::unordered_map<uint32_t, std::string> g_modNames;
struct T { HANDLE h = nullptr; std::string name; std::unordered_map<uint64_t, uint32_t> hist; uint32_t n = 0; uint64_t cpu0 = 0, cpuPrev = 0; bool busy = true; };

uint64_t CpuNs100(HANDLE h) { FILETIME c, e, k, u; return GetThreadTimes(h, &c, &e, &k, &u) ? ((uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime)) : 0; }

std::string NameOf(HANDLE h, DWORD tid) {
  PWSTR w = nullptr; std::string s;
  if (SUCCEEDED(GetThreadDescription(h, &w)) && w) { for (const wchar_t* p = w; *p; ++p) s += char(*p < 128 ? *p : '?'); LocalFree(w); }
  return s.empty() ? "?" : s;
}

void SamplerMain() {
  NameThisThread("diag-sampler");
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const DWORD self = GetCurrentThreadId();
  std::unordered_map<DWORD, T> th;
  auto lastRefresh = std::chrono::steady_clock::time_point{}, lastRep = std::chrono::steady_clock::now();
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastRefresh > std::chrono::seconds(1)) {  // threads come and go
      lastRefresh = now;
      std::vector<DWORD> ids;
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
          if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self) ids.push_back(te.th32ThreadID);
        CloseHandle(snap);
      }
      for (auto it = th.begin(); it != th.end();) {  // drop dead
        if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) { CloseHandle(it->second.h); it = th.erase(it); } else ++it;
      }
      for (DWORD id : ids) {
        auto& t = th[id];
        if (!t.h) { t.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id); if (t.h) t.cpu0 = CpuNs100(t.h); }
        if (t.h) { const uint64_t c = CpuNs100(t.h); t.busy = (c - t.cpuPrev) > 500000ull; t.cpuPrev = c; }  // >5% of the last 1 s; idle threads are not suspended (less perturbation)
        if (t.h && (t.name.empty() || t.name == "?")) t.name = NameOf(t.h, id);
      }
    }
    for (auto& [id, t] : th) {
      if (!t.h || !t.busy) continue;
      if (SuspendThread(t.h) == DWORD(-1)) continue;
      CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(t.h, &ctx)) {
        uint64_t key = ctx.Rip - base;
        HMODULE hm = nullptr;
        if (key >= 0x4000000ull) {  // outside the exe: key = 0xF<<60 | hash(module)<<24? keep simple: absolute, tagged
          GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(ctx.Rip), &hm);
          wchar_t nm[MAX_PATH] = {}; if (hm) GetModuleFileNameW(hm, nm, MAX_PATH);
          std::wstring w(nm); size_t sl = w.find_last_of(L'\\'); uint32_t hs = 0; for (size_t i = sl == std::wstring::npos ? 0 : sl + 1; i < w.size(); ++i) hs = hs * 131u + w[i];
          hs &= 0xFFFFFFu; g_modNames[hs] = std::string(w.begin() + (sl == std::wstring::npos ? 0 : sl + 1), w.end());
          key = (0xFull << 60) | (uint64_t(hs & 0xFFFFFFu) << 32) | ((ctx.Rip - reinterpret_cast<uintptr_t>(hm)) & 0xFFFFFFFFull);
        }
        ++t.hist[key]; ++t.n;
        // inclusive profile: unwind (<=24 frames) and count every distinct exe function-entry RVA once per sample
        {
          CONTEXT uc{}; uc.ContextFlags = CONTEXT_FULL;
          if (GetThreadContext(t.h, &uc)) {
            uint64_t seen[24]; int ns = 0;
            for (int f = 0; f < 24 && uc.Rip; ++f) {
              DWORD64 ib = 0; auto* rf = RtlLookupFunctionEntry(uc.Rip, &ib, nullptr);
              if (ib == base && rf) {
                const uint64_t fn = rf->BeginAddress; bool dup = false;
                for (int q = 0; q < ns; ++q) dup |= seen[q] == fn;
                if (!dup) { seen[ns++] = fn; ++t.hist[(0xEull << 60) | fn]; }
              }
              if (!rf) { if (!uc.Rsp) break; uc.Rip = *reinterpret_cast<DWORD64*>(uc.Rsp); uc.Rsp += 8; continue; }
              PVOID hd = nullptr; DWORD64 ef = 0;
              RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, uc.Rip, rf, &uc, &hd, &ef, nullptr);
            }
          }
        }
      }
      ResumeThread(t.h);
    }
    if (now - lastRep > std::chrono::seconds(20)) {
      const double secs = std::chrono::duration<double>(now - lastRep).count(); lastRep = now;
      MCLA_LOG_ERROR("ALLTHR-REPORT threads={}", th.size());
      for (auto& [id, t] : th) {
        const uint64_t c = t.h ? CpuNs100(t.h) : 0; const double pct = (c - t.cpu0) / 1e7 / secs * 100.0; t.cpu0 = c;
        if (pct >= 1.0 || t.n == 0) {
          MCLA_LOG_ERROR("ALLTHR tid={} name={} cpu={:.0f}% samples={}", id, t.name, pct, t.n);
          std::vector<std::pair<uint32_t, uint64_t>> v; for (auto& kv : t.hist) v.emplace_back(kv.second, kv.first);
          std::sort(v.begin(), v.end(), std::greater<>());
          int shown = 0, shownC = 0;
          for (auto& [cnt, key] : v) {
            const bool caller = (key >> 60) == 0xE;
            if (caller ? shownC >= 14 : shown >= 8) continue;
            (caller ? shownC : shown)++;
            if ((key >> 60) == 0xF) { auto m = g_modNames.find(uint32_t((key >> 32) & 0xFFFFFF)); MCLA_LOG_ERROR("ALLTHR-MOD tid={} {} {:X} {}", id, m != g_modNames.end() ? m->second : "?", key & 0xFFFFFFFFull, cnt); }
            else MCLA_LOG_ERROR("ALLTHR-{} tid={} {:X} {}", caller ? "INCL" : "RVA", id, key & 0x0FFFFFFFFFFFFFFFull, cnt);
          }
        }
        t.hist.clear(); t.n = 0;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void GrowthMain() {
  NameThisThread("diag-growth");
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof pmc}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
    MCLA_LOG_ERROR("GROWTH process private={:.0f}MB ws={:.0f}MB handles={}", pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0, handles);
    {  // true per-thread CPU from cycle counters (GetThreadTimes is tick-sampled and bills 1 ms-polling threads ~100%)
      static std::unordered_map<DWORD, std::pair<HANDLE, uint64_t>> prev; static auto tPrev = std::chrono::steady_clock::now();
      static double ghz = [] { DWORD mhz = 0, sz = 4; RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\DESCRIPTION\System\CentralProcessor\0", "~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &sz); return mhz ? mhz / 1000.0 : 3.0; }();
      const auto tn = std::chrono::steady_clock::now(); const double dt = std::chrono::duration<double>(tn - tPrev).count(); tPrev = tn;
      std::vector<std::pair<double, std::string>> rows;
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
          if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
          auto& e = prev[te.th32ThreadID];
          if (!e.first) e.first = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          ULONG64 cy = 0; if (!e.first || !QueryThreadCycleTime(e.first, &cy)) continue;
          const double pct = e.second ? double(cy - e.second) / (ghz * 1e9 * dt) * 100.0 : 0; e.second = cy;
          if (pct >= 5.0) { MCLA_LOG_ERROR("GROWTH-CPU-RAW tid={} cyc_M={}", te.th32ThreadID, cy / 1000000); } if (pct >= 5.0) rows.emplace_back(pct, std::to_string(te.th32ThreadID) + ":" + NameOf(e.first, te.th32ThreadID) + "=" + std::to_string(int(pct)) + "%");
        }
        CloseHandle(snap);
      }
      std::sort(rows.begin(), rows.end(), std::greater<>()); std::string o; for (size_t i = 0; i < rows.size() && i < 8; ++i) o += rows[i].second + " ";
      MCLA_LOG_ERROR("GROWTH-CPU(cycles, {:.2f}GHz nominal) {}", ghz, o);
    }
    std::vector<std::pair<std::string, std::function<std::string()>>> copy; { std::lock_guard<std::mutex> lk(g_mu); copy = g_growth; }
    for (auto& [n, f] : copy) { std::string s; try { s = f(); } catch (...) { s = "EXC"; } MCLA_LOG_ERROR("GROWTH {} {}", n, s); }
  }
}
// MCLA_GUEST_PROFILE=1: ~1 kHz stack sampler of the threads that run guest code (guest-* / boot-main) plus render*.
// Unlike SamplerMain it samples idle threads too (so running-vs-blocked is measurable), keeps whole stacks,
// and dumps cumulative histograms to guest_profile_<tid>.txt every 10 s for tools/guest_profile_report.py.
void GuestProfMain() {
  NameThisThread("guest-prof");
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const DWORD self = GetCurrentThreadId();
  struct P { HANDLE h = nullptr; std::string name; bool want = false; std::map<std::vector<uint64_t>, uint32_t> stacks; uint32_t n = 0, blocked = 0; ULONG64 cyc0 = 0, cycLast = 0; const std::vector<uint64_t>* last = nullptr; };
  std::unordered_map<DWORD, P> th;
  std::map<uintptr_t, uint32_t> mods;  // module base -> index into modNames (resolved after resume: GetModuleHandleEx takes the loader lock)
  std::vector<std::string> modNames;
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  // optional window: MCLA_GUEST_PROFILE_START=<s> delay, MCLA_GUEST_PROFILE_SECS=<s> then a final dump and stop
  const char* es = std::getenv("MCLA_GUEST_PROFILE_START"); const char* ed = std::getenv("MCLA_GUEST_PROFILE_SECS");
  if (es) std::this_thread::sleep_for(std::chrono::seconds(atoi(es)));
  const int durS = ed ? atoi(ed) : 0;
  const auto t0 = std::chrono::steady_clock::now();
  auto lastRefresh = std::chrono::steady_clock::time_point{}, lastDump = t0;
  MCLA_LOG_ERROR("GUEST-PROFILE on: sampling guest-*/boot-main threads, dump every 10 s to guest_profile_<tid>.txt");
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastRefresh > std::chrono::seconds(1)) {
      lastRefresh = now;
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
          if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
          auto& t = th[te.th32ThreadID];
          if (!t.h) t.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          if (t.h && !t.want && (t.name.empty() || t.name == "?")) {  // names are set at thread start; retry until one appears
            t.name = NameOf(t.h, te.th32ThreadID);
            t.want = t.name.rfind("guest-", 0) == 0 || t.name == "boot-main" || t.name.rfind("render", 0) == 0;  // render* = the consumer, for "who bounds the frame"
            if (t.want) QueryThreadCycleTime(t.h, &t.cyc0);
          }
        }
        CloseHandle(snap);
      }
    }
    for (auto& [id, t] : th) {
      if (!t.want) continue;
      {  // no cycles since the last sample = it never ran, so its stack is unchanged: re-count it without suspending
        ULONG64 cy = 0; QueryThreadCycleTime(t.h, &cy);
        const bool idle = cy == t.cycLast && t.last; t.cycLast = cy;
        if (idle) { ++t.stacks[*t.last]; ++t.n; t.blocked += ((*t.last)[0] >> 62 & 1) != 0; continue; }
      }
      uint64_t fr[64]; int nf = 0; bool blocked = false;
      if (SuspendThread(t.h) == DWORD(-1)) continue;
      {  // no allocation / no loader lock while the target is suspended
        CONTEXT uc{}; uc.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t.h, &uc)) {
          // leaf right after a `syscall` (0F 05) = blocked in the kernel (wait/sleep/yield)
          blocked = uc.Rip > 2 && !IsBadReadPtr(reinterpret_cast<const void*>(uc.Rip - 2), 2) &&
                    reinterpret_cast<const uint8_t*>(uc.Rip)[-2] == 0x0F && reinterpret_cast<const uint8_t*>(uc.Rip)[-1] == 0x05;
          for (; nf < 64 && uc.Rip; ++nf) {
            fr[nf] = uc.Rip;
            DWORD64 ib = 0; auto* rf = RtlLookupFunctionEntry(uc.Rip, &ib, nullptr);
            if (!rf) { if (!uc.Rsp) break; uc.Rip = *reinterpret_cast<DWORD64*>(uc.Rsp); uc.Rsp += 8; continue; }
            PVOID hd = nullptr; DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, uc.Rip, rf, &uc, &hd, &ef, nullptr);
          }
        }
      }
      ResumeThread(t.h);
      // encode: exe frame = rva; other module = 1<<63 | base-index<<32 | offset; bit 62 on frame 0 = blocked
      std::vector<uint64_t> key(nf);
      for (int i = 0; i < nf; ++i) {
        if (fr[i] - base < 0x10000000ull) { key[i] = fr[i] - base; continue; }
        HMODULE hm = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(fr[i]), &hm);
        const uintptr_t mb = reinterpret_cast<uintptr_t>(hm);
        auto it = mods.find(mb);
        if (it == mods.end()) {
          wchar_t nm[MAX_PATH] = {}; if (hm) GetModuleFileNameW(hm, nm, MAX_PATH);
          std::wstring w(nm); const size_t sl = w.find_last_of(L'\\');
          std::string s; for (size_t k = sl == std::wstring::npos ? 0 : sl + 1; k < w.size(); ++k) s += char(w[k] < 128 ? w[k] : '?');
          modNames.push_back(s.empty() ? "?" : s);
          it = mods.emplace(mb, uint32_t(modNames.size() - 1)).first;
        }
        key[i] = (1ull << 63) | (uint64_t(it->second) << 32) | ((fr[i] - mb) & 0xFFFFFFFFull);
      }
      if (blocked && nf) key[0] |= 1ull << 62;
      auto& slot = *t.stacks.try_emplace(std::move(key), 0).first; ++slot.second; ++t.n; t.blocked += blocked;
      t.last = slot.first.empty() ? nullptr : &slot.first;  // std::map keys are address-stable
    }
    const bool final = durS > 0 && now - t0 > std::chrono::seconds(durS);
    if (final || now - lastDump > std::chrono::seconds(10)) {
      lastDump = now;
      const double wall = std::chrono::duration<double>(now - t0).count();
      for (auto& [id, t] : th) {
        if (!t.want || !t.n) continue;
        ULONG64 cy = 0; QueryThreadCycleTime(t.h, &cy);
        FILE* f = nullptr;
        char path[64]; snprintf(path, sizeof path, "guest_profile_%lu.txt", static_cast<unsigned long>(id));
        if (fopen_s(&f, path, "w") || !f) continue;
        fprintf(f, "# tid=%lu name=%s samples=%u blocked=%u wall_s=%.1f cycles=%llu\n", static_cast<unsigned long>(id), t.name.c_str(), t.n, t.blocked, wall, cy - t.cyc0);
        for (size_t mi = 0; mi < modNames.size(); ++mi) fprintf(f, "# mod %zu %s\n", mi, modNames[mi].c_str());
        for (auto& [k, c] : t.stacks) {
          fprintf(f, "%u", c);
          for (uint64_t v : k) fprintf(f, " %llx", static_cast<unsigned long long>(v));
          fputc('\n', f);
        }
        fclose(f);
      }
      if (final) { MCLA_LOG_ERROR("GUEST-PROFILE done: {} s window dumped", durS); return; }
    }
    LARGE_INTEGER due; due.QuadPart = -10000;  // 1 ms
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
    else std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
}  // namespace

void NameThisThread(const char* name) {
  wchar_t w[64] = {}; for (int i = 0; name[i] && i < 63; ++i) w[i] = name[i];
  SetThreadDescription(GetCurrentThread(), w);
}
void RegisterGrowth(const char* name, std::function<std::string()> fn) {
  std::lock_guard<std::mutex> lk(g_mu); g_growth.emplace_back(name, std::move(fn));
}
void StartDiagThreads() {
  static std::once_flag f;
  std::call_once(f, [] {
    if (Env("MCLA_TEMP_SAMPLER_ALL")) std::thread(SamplerMain).detach();
    if (Env("MCLA_TEMP_GROWTH")) std::thread(GrowthMain).detach();
    if (Env("MCLA_GUEST_PROFILE")) std::thread(GuestProfMain).detach();
  });
}
}  // namespace mcla::diag
