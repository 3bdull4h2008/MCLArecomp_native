"""Decompile / disassemble guest functions with IDA Pro 9.5 idalib DIRECTLY (no HTTP bridge, no MCP).
Works when `tools/ida_dump.py` cannot (no listener on :8745). One process at a time owns the .i64:
a lock file serialises concurrent agents (they wait, they do not corrupt the database).

  python tools/ida_local.py <out_dir> 0x827BE010 0x827C1F08 ...      # writes <out_dir>/sub_<VA>.txt (+ prints path)
  python tools/ida_local.py --eval "<python using ida_* modules>"      # result of the last expression / prints
  python tools/ida_local.py --xrefs 0x827C7410                         # code refs to a VA (callers)

Each dump = pseudocode + RAW BYTES beside it (RULE 0: decompiler output is a model; masks need the raw word),
+ disassembly + callers. Database: build/cache/mcla_pe_ida.bin.i64 (opened without re-analysis)."""
import os, sys, time, msvcrt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
I64 = os.path.join(ROOT, "build", "cache", "mcla_pe_ida.bin.i64")
LOCK = os.path.join(ROOT, "build", "cache", ".ida_local.lock")

def _lock():
    f = open(LOCK, "a+")
    while True:
        try:
            f.seek(0); msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1); return f
        except OSError:
            time.sleep(2)

lk = _lock()
import idapro  # must be first IDA import
idapro.open_database(I64, run_auto_analysis=False)
import ida_hexrays, ida_funcs, ida_bytes, ida_idaapi, idautils, idc, ida_name  # noqa: E402
ida_hexrays.init_hexrays_plugin()

def dump(ea):
    f = ida_funcs.get_func(ea)
    out = []
    if not f:
        return "0x%08X: no function carved here (use raw bytes / ppc_disasm.py)\n" % ea
    out.append("== sub_%08X  [0x%08X..0x%08X) size=%d ==" % (f.start_ea, f.start_ea, f.end_ea, f.end_ea - f.start_ea))
    try:
        out.append(str(ida_hexrays.decompile(f.start_ea)))
    except Exception as e:  # decompiler failures are data, not errors
        out.append("DECOMPILE-FAILED: %r" % (e,))
    out.append("-- RAW BYTES (big-endian words) --")
    for a in range(f.start_ea, min(f.end_ea, f.start_ea + 0x400), 4):
        out.append("%08X: %08X" % (a, ida_bytes.get_dword(a)))
    out.append("-- DISASM --")
    a = f.start_ea
    while a < f.end_ea:
        out.append("%08X  %s" % (a, idc.GetDisasm(a))); a = idc.next_head(a, f.end_ea)
        if a == ida_idaapi.BADADDR: break
    out.append("-- CALLERS (code xrefs; '(none)' is an IDA carving artifact, not evidence) --")
    cs = [("%08X" % r) for r in idautils.CodeRefsTo(f.start_ea, 0)]
    out.append(", ".join(cs[:80]) or "(none)")
    return "\n".join(out) + "\n"

try:
    args = sys.argv[1:]
    if args and args[0] == "--eval":
        exec(compile(args[1], "<eval>", "exec"), globals())
    elif args and args[0] == "--xrefs":
        ea = int(args[1], 16)
        for r in idautils.CodeRefsTo(ea, 0):
            f = ida_funcs.get_func(r)
            print("%08X  in %s" % (r, ("sub_%08X" % f.start_ea) if f else "?"))
    else:
        outdir = args[0]; os.makedirs(outdir, exist_ok=True)
        for s in args[1:]:
            ea = int(s, 16); p = os.path.join(outdir, "sub_%08X.txt" % ea)
            open(p, "w", encoding="utf8").write(dump(ea)); print(p)
finally:
    idapro.close_database(False)
