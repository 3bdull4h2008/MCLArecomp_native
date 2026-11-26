#!/usr/bin/env python3
"""Read a Xenia --trace_function_data_path file (the reference run's ground truth).

Record layout, from .research/xenia/src/xenia/cpu/function_trace_data.h (Header):
  +0 u32 data_size  +4 u32 start  +8 u32 end  +12 u32 type  +16 u64 thread_mask
  +24 u64 call_count  +32 u32 caller_history[4]  +48 u64 exec_count[instruction_count]
instruction_count = (end - start) / 4 + 1. Host-endian (little) on x64.
Records are packed back to back in a chunked mapped file; zero data_size = unused tail.

  python tools/xenia_trace.py summary <trace>                  counts, top functions
  python tools/xenia_trace.py fn <trace> 0x826E3EC8 [...]       calls, callers, per-instruction counts
  python tools/xenia_trace.py ran <trace> 0x826E3F80 [...]      did this exact instruction execute?
  python tools/xenia_trace.py overrides <trace> <src_dir>       Xenia call counts for every PPC_FUNC(sub_X) we override
  python tools/xenia_trace.py --selftest
"""
import os
import re
import struct
import sys

HDR = struct.Struct("<IIIIQQ4I")  # 48 bytes


def records(path):
    data = open(path, "rb").read()
    off, n = 0, len(data)
    while off + HDR.size <= n:
        size, start, end, typ, threads, calls, *callers = HDR.unpack_from(data, off)
        if size == 0:
            # A chunk tail is zero-filled; skip to the next 4-byte boundary with data.
            off += 4
            continue
        if size < HDR.size or off + size > n or end < start or (end - start) > 0x400000:
            off += 4  # not a record header; resync
            continue
        cnt = (end - start) // 4 + 1
        execs = struct.unpack_from("<%dQ" % cnt, data, off + HDR.size) if HDR.size + 8 * cnt <= size else ()
        yield {"start": start, "end": end, "type": typ, "threads": threads, "calls": calls,
               "callers": [c for c in callers if c], "exec": execs}
        off += size


def load(path):
    by = {}
    for r in records(path):
        prev = by.get(r["start"])
        if prev is None or r["calls"] >= prev["calls"]:
            by[r["start"]] = r
    return by


def find(by, va):
    if va in by:
        return by[va]
    for r in by.values():
        if r["start"] <= va <= r["end"]:
            return r
    return None


def main(argv):
    if argv[:1] == ["--selftest"]:
        return selftest()
    if len(argv) < 2:
        print(__doc__); return 2
    cmd, path = argv[0], argv[1]
    by = load(path)
    if cmd == "summary":
        ran = [r for r in by.values() if r["calls"]]
        print(f"functions defined={len(by)} called={len(ran)}")
        for r in sorted(ran, key=lambda r: -r["calls"])[:25]:
            print(f"  {r['start']:08X} calls={r['calls']}")
    elif cmd in ("fn", "ran"):
        for a in argv[2:]:
            va = int(a, 16)
            r = find(by, va)
            if r is None:
                print(f"{va:08X}: NOT DEFINED in this trace (never translated => never executed)")
                continue
            if cmd == "ran":
                i = (va - r["start"]) // 4
                c = r["exec"][i] if i < len(r["exec"]) else None
                print(f"{va:08X}: in sub_{r['start']:08X} executed={c}")
                continue
            print(f"sub_{r['start']:08X}..{r['end']:08X} calls={r['calls']} threads={r['threads']:#x} "
                  f"callers={[hex(c) for c in r['callers']]}")
            for i, c in enumerate(r["exec"]):
                print(f"   {r['start'] + 4 * i:08X} {c}")
    elif cmd == "overrides":
        src = argv[2]
        seen = set()
        for root, _, files in os.walk(src):
            for f in files:
                if f.endswith(".cpp"):
                    t = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
                    seen.update(int(m, 16) for m in re.findall(r"PPC_FUNC\(sub_([0-9A-F]{8})\)", t))
        for va in sorted(seen):
            r = by.get(va)
            print(f"sub_{va:08X} xenia_calls={r['calls'] if r else 0}")
    else:
        print(__doc__); return 2
    return 0


def selftest():
    import tempfile
    rec = HDR.pack(HDR.size + 8 * 3, 0x82000010, 0x82000018, 0, 1, 7, 0x82000100, 0, 0, 0) + struct.pack("<3Q", 7, 0, 7)
    blob = b"\0" * 16 + rec + b"\0" * 64
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(blob)
    by = load(t.name)
    os.unlink(t.name)
    r = by.get(0x82000010)
    assert r and r["calls"] == 7 and r["exec"] == (7, 0, 7) and r["callers"] == [0x82000100], r
    assert find(by, 0x82000014)["start"] == 0x82000010
    print("selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
