"""Sample every thread of a live mcla.exe and name what it is running, without a debugger.
usage: thread_sample.py <mcla.map> [--self-test] [--filter TID_HEX,TID_HEX]

Recompiled guest functions are host functions (`__imp__sub_<VA>`), so the host stack names the
guest call chain. The stack is SCANNED for return addresses (a qword in .text preceded by a call
instruction), not unwound: stale frames can appear, so treat the top frames as the evidence.
"""
import bisect, ctypes, re, struct, sys
from ctypes import wintypes as W

k = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k.OpenProcess.restype = W.HANDLE
k.OpenThread.restype = W.HANDLE
k.CreateToolhelp32Snapshot.restype = W.HANDLE
for f in (k.CloseHandle, k.SuspendThread, k.ResumeThread):
    f.argtypes = [W.HANDLE]
k.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                ctypes.POINTER(ctypes.c_size_t)]
k.GetThreadContext.argtypes = [W.HANDLE, ctypes.c_void_p]
k.Thread32First.argtypes = k.Thread32Next.argtypes = [W.HANDLE, ctypes.c_void_p]
k.Process32First.argtypes = k.Process32Next.argtypes = [W.HANDLE, ctypes.c_void_p]
psapi.EnumProcessModules.argtypes = [W.HANDLE, ctypes.c_void_p, W.DWORD, ctypes.POINTER(W.DWORD)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
                ("th32OwnerProcessID", W.DWORD), ("tpBasePri", W.LONG), ("tpDeltaPri", W.LONG),
                ("dwFlags", W.DWORD)]


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ProcessID", W.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", W.DWORD),
                ("cntThreads", W.DWORD), ("th32ParentProcessID", W.DWORD),
                ("pcPriClassBase", W.LONG), ("dwFlags", W.DWORD), ("szExeFile", ctypes.c_char * 260)]


def load_map(path):
    """Publics table: `sec:off  name  Rva+Base  [f] [i] obj`. Returns sorted (rva, name) arrays."""
    pat = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")
    rows = []
    for line in open(path, errors="replace"):
        m = pat.match(line)
        if m:
            rows.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
    rows.sort()
    return [r[0] for r in rows], [r[1] for r in rows]


def is_call_before(blob, i):
    """True if blob[:i] ends in a near call: E8 rel32, or FF /2 (2,3,6,7 bytes, optional REX)."""
    if i >= 5 and blob[i - 5] == 0xE8:
        return True
    for n in (2, 3, 6, 7):
        if i >= n and blob[i - n] == 0xFF and (blob[i - n + 1] >> 3) & 7 == 2:
            return True
    return False


def self_test():
    assert is_call_before(bytes([0xE8, 1, 2, 3, 4]), 5)
    assert is_call_before(bytes([0xFF, 0xD0]), 2)            # call rax
    assert is_call_before(bytes([0x41, 0xFF, 0x53, 0x10]), 4)  # call [r11+0x10]
    assert not is_call_before(bytes([0x90, 0x90, 0x90, 0x90, 0x90]), 5)
    assert not is_call_before(bytes([0xFF, 0xE0]), 2)        # jmp rax is not a call
    print("self-test ok")


def find_pid():
    snap = k.CreateToolhelp32Snapshot(2, 0)
    e = PROCESSENTRY32(); e.dwSize = ctypes.sizeof(e)
    ok = k.Process32First(snap, ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.lower() == b"mcla.exe":
            pid = e.th32ProcessID
        ok = k.Process32Next(snap, ctypes.byref(e))
    k.CloseHandle(snap)
    return pid


def main():
    if "--self-test" in sys.argv:
        return self_test()
    rvas, names = load_map(sys.argv[1])
    only = None
    if "--filter" in sys.argv:
        only = {int(x, 16) for x in sys.argv[sys.argv.index("--filter") + 1].split(",")}
    pid = find_pid()
    if not pid:
        sys.exit("mcla.exe not running")
    hp = k.OpenProcess(0x0410, False, pid)
    mods = (ctypes.c_void_p * 1)(); need = W.DWORD()
    psapi.EnumProcessModules(hp, mods, ctypes.sizeof(mods), ctypes.byref(need))
    base = mods[0]
    lo, hi = base + rvas[0], base + rvas[-1]

    def read(addr, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        k.ReadProcessMemory(hp, addr, buf, n, ctypes.byref(got))
        return buf.raw[:got.value]

    def sym(addr):
        i = bisect.bisect_right(rvas, addr - base) - 1
        return f"{names[i]}+0x{addr - base - rvas[i]:x}"

    snap = k.CreateToolhelp32Snapshot(4, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
    tids = []
    ok = k.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            tids.append(te.th32ThreadID)
        ok = k.Thread32Next(snap, ctypes.byref(te))
    k.CloseHandle(snap)

    ctx = ctypes.create_string_buffer(1232 + 16)
    al = (ctypes.addressof(ctx) + 15) & ~15
    for tid in tids:
        if only and tid not in only:
            continue
        ht = k.OpenThread(0x004A, False, tid)  # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION
        if not ht:
            continue
        k.SuspendThread(ht)
        try:
            ctypes.memset(al, 0, 1232)
            struct.pack_into("<I", (ctypes.c_char * 1232).from_address(al), 0x30, 0x100001)
            if not k.GetThreadContext(ht, al):
                continue
            raw = bytes((ctypes.c_char * 1232).from_address(al))
            rsp = struct.unpack_from("<Q", raw, 0x98)[0]
            rip = struct.unpack_from("<Q", raw, 0xF8)[0]
            frames = [(sym(rip) if lo <= rip < hi else f"<outside image {rip:x}>")]
            stack = read(rsp, 0x8000)
            for off in range(0, len(stack) - 7, 8):
                v = struct.unpack_from("<Q", stack, off)[0]
                code = read(v - 8, 8) if lo <= v < hi else b""
                if len(code) == 8 and is_call_before(code, 8):
                    frames.append(sym(v))
                    if len(frames) >= 60:
                        break
            print(f"tid={tid:08X} " + " < ".join(frames))
        finally:
            k.ResumeThread(ht)
            k.CloseHandle(ht)
    k.CloseHandle(hp)


if __name__ == "__main__":
    main()
