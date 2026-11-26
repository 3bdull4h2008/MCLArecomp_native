"""2D gate: bounded boot + real-window screenshots. usage: gate2d.py NAME EXE SECONDS [K=V ...]"""
import subprocess,sys,time,os,ctypes,re
from ctypes import wintypes as W
from PIL import Image
name,exe,secs=sys.argv[1],sys.argv[2],int(sys.argv[3])
env=dict(os.environ); env.update(a.split('=',1) for a in sys.argv[4:] if '=' in a)
# TEMP filter bridge (star/ring/city long runs): TEMP-* probes are MCLA_LOG_ERROR
# with flush-on-error (src/common/logging.h:48), so each row sync-flushes to the
# stdout/file sinks. For >=1200s runs pass the filter through as MCLA_TEMP_FILTER
# (e.g. MCLA_TEMP_FILTER=star,ring,city or off) so the exe can suppress
# per-draw fmt/scan + log before the probe work, not just post-filter the log.
# Bridge: gate2d sets env below -- GATE_TEMP_FILTER is forwarded to
# MCLA_TEMP_FILTER, which now reaches the gate in d3d12_backend.cpp (suppression
# happens in the exe before probe work). gate2d does NOT post-filter the log:
# post-filtering saves no wall time.
if 'GATE_TEMP_FILTER' in env: env['MCLA_TEMP_FILTER']=env['GATE_TEMP_FILTER']
B=r"C:\Users\abdullah\OneDrive\Documents\mcla pc\build"
out=os.path.join(os.environ.get('GATE_OUT',r"C:\mcla-backup-20261105\gate"),name); os.makedirs(out,exist_ok=True)
log=os.path.join(B,f"{name}.log")
assert not os.path.exists(log),"never reuse a log name"
u=ctypes.windll.user32; u.SetProcessDPIAware()
def win(pid):
    r=[]
    @ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    def cb(h,_):
        p=W.DWORD(); u.GetWindowThreadProcessId(h,ctypes.byref(p))
        if p.value==pid and u.IsWindowVisible(h):
            buf=ctypes.create_unicode_buffer(256); u.GetClassNameW(h,buf,256)
            if buf.value=='SDL_app': r.append(h)
        return True
    u.EnumWindows(cb,0); return r[0] if r else None
g=ctypes.windll.gdi32
def grab(h):
    # PrintWindow reads this window's own surface: no focus steal, cannot capture what covers it
    rc=W.RECT(); u.GetClientRect(h,ctypes.byref(rc)); w,ht=rc.right,rc.bottom
    if w<=0 or ht<=0: raise RuntimeError('empty client rect')
    u.GetDC.restype=W.HDC; g.CreateCompatibleDC.restype=W.HDC; g.CreateCompatibleBitmap.restype=W.HBITMAP
    d=u.GetDC(W.HWND(h)); m=g.CreateCompatibleDC(W.HDC(d)); b=g.CreateCompatibleBitmap(W.HDC(d),w,ht)
    if not d or not m or not b: raise RuntimeError('GDI alloc failed')
    try:
        g.SelectObject(W.HDC(m),W.HBITMAP(b))
        ok=u.PrintWindow(W.HWND(h),W.HDC(m),3)
        if ok!=1: raise RuntimeError('PrintWindow failed')
        bi=(ctypes.c_uint32*10)(40,w,ctypes.c_uint32(-ht).value,0x200001,0,0,0,0,0,0); buf=ctypes.create_string_buffer(w*ht*4)
        got=g.GetDIBits(W.HDC(m),W.HBITMAP(b),0,ht,buf,ctypes.byref(bi),0)
        if got!=ht: raise RuntimeError('GetDIBits failed')
        img=Image.frombytes('RGB',(w,ht),buf.raw,'raw','BGRX',0,1)
        return img
    finally:
        try: g.DeleteObject(W.HBITMAP(b))
        except Exception: pass
        try: g.DeleteDC(W.HDC(m))
        except Exception: pass
        try: u.ReleaseDC(W.HWND(h),W.HDC(d))
        except Exception: pass
def tail_drawn(logpath,nbytes=65536):
    """Best-effort guest depth at this wall time: last drawn=N in the tail.
    Wall-clock t alone cannot tell stalled depth from progressing depth.
    Returns None when no drawn= is in the tail. drawn= lives on
    DRAW-CP-CENSUS (MCLA_LOG_WARN) and the D3D12 frame rows (INFO), so at
    MCLA_LOG_LEVEL=ERROR (WARN/INFO suppressed) tail_drawn returns None by
    design -- that is a log-level artifact, not a zero. The honest depth
    aboard MCLA_LOG_LEVEL is TEMP-RENDER-1S (render_thread.cpp, MCLA_LOG_ERROR,
    1/s consumed/drawn/skip), which survives ERROR level. For depth at
    ERROR level, parse TEMP-RENDER-1S, not tail_drawn."""
    try:
        with open(logpath,'rb') as f:
            f.seek(0,os.SEEK_END); pos=f.tell()
            f.seek(max(0,pos-nbytes))
            m=re.findall(rb'drawn=(\d+)',f.read())
            return int(m[-1]) if m else None
    except Exception:
        return None
def stream_drawn_max(logpath):
    """Chunked full-log scan. Whole-file .read() OOMs on multi-GB soaks."""
    mx=0; rows=0
    try:
        with open(logpath,'rb') as f:
            prev=b''
            while True:
                chunk=f.read(1<<20)
                if not chunk: break
                seg=prev+chunk
                vals=re.findall(rb'drawn=(\d+)',seg)
                rows+=len(vals)
                for v in vals:
                    n=int(v)
                    if n>mx: mx=n
                prev=seg[-32:]
    except FileNotFoundError:
        pass
    return mx,rows
def mcla_pids():
    k=ctypes.WinDLL("kernel32",use_last_error=True)
    class PE(ctypes.Structure):
        _fields_=[("dwSize",W.DWORD),("cntUsage",W.DWORD),("th32ProcessID",W.DWORD),
                  ("th32DefaultHeapID",ctypes.c_size_t),("th32ModuleID",W.DWORD),
                  ("cntThreads",W.DWORD),("th32ParentProcessID",W.DWORD),
                  ("pcPriClassBase",W.LONG),("dwFlags",W.DWORD),("szExeFile",ctypes.c_char*260)]
    snap=k.CreateToolhelp32Snapshot(2,0); e=PE(); e.dwSize=ctypes.sizeof(e)
    ok=k.Process32First(snap,ctypes.byref(e)); pids=[]
    while ok:
        if e.szExeFile.lower()==b"mcla.exe": pids.append(e.th32ProcessID)
        ok=k.Process32Next(snap,ctypes.byref(e))
    k.CloseHandle(snap); return pids
# --- event schedule: screenshots, samples, keys and maximize are independent axes ---
# Old code nested samples/keys/max under the screenshot tick list, so any GATE_SAMPLES=380
# (HANDOFF 2026-11-09) or KEYS/MAX tick not in the 39 hard-coded screenshots silently never
# fired, and screenshots stopped entirely after 540s. Merge all axes onto one wall timeline.
# Default GATE_SAMPLES is 45 for 60s gates so the sample fires (60/140 never
# fire when secs=60: ticks only fire when s < secs, so a 60s gate would
# otherwise take no sample at all). Long gates keep 60,140.
def _ints(s,default):
    s=env.get(s,os.environ.get(s,default))
    return {int(x) for x in s.split(',') if x.strip()}
sample_ticks=_ints('GATE_SAMPLES','45' if secs<=60 else '60,140')
key_map={}
for kv in env.get('GATE_KEYS',os.environ.get('GATE_KEYS','')).split(','):
    if not kv.strip(): continue
    kt,vk=kv.split(':'); key_map.setdefault(int(kt),[]).append(int(vk))
max_tick=int(env['GATE_MAX']) if env.get('GATE_MAX','').strip() else None
shot_ticks={t for t in (15,20,25,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,50,55,60,75,90,105,120,140,150,160,170,180,240,300,360,420,480,540) if t<secs}
t=600
while t<secs:
    shot_ticks.add(t); t+=60
ticks=sorted(shot_ticks|{s for s in sample_ticks if s<secs}|{k for k in key_map if k<secs}|({max_tick} if max_tick is not None and max_tick<secs else set()))
for label,s in (('SAMPLES',sample_ticks),('KEYS',set(key_map)),('MAX',{max_tick} if max_tick is not None else set())):
    for s_ in sorted(s):
        if s_>=secs: print(f'WARN {label} tick {s_} >= secs {secs}: ignored',flush=True)
map_path=os.path.join(B,"mcla.map")
# Rule-19 firing proof for the sampler: is_call_before is the only decoder the scan
# trusts, so run its -self-test once per gate run and stamp the verdict into every
# sample file. A zero in a file without this line proves nothing.
selftest_line="selftest NOT RUN"
try:
    _st=subprocess.run([sys.executable,os.path.join(os.path.dirname(__file__),"thread_sample.py"),"--self-test"],capture_output=True,text=True,timeout=15)
    _st0=((_st.stdout or _st.stderr or "").strip().splitlines() or ["<no output>"])[0]
    selftest_line=f"selftest rc={_st.returncode} {_st0}"
    print(' thread_sample '+selftest_line,flush=True)
except Exception as e:
    print('WARN thread_sample self-test failed:',e,flush=True)
    selftest_line=f"selftest FAILED {e}"
try:
    print(' exe mtime',os.path.getmtime(exe),'map mtime',os.path.getmtime(map_path),flush=True)
    if os.path.getmtime(map_path)<os.path.getmtime(exe): print('WARN map older than exe: thread_sample symbols may be stale',flush=True)
except Exception as e:
    print('WARN cannot compare exe/map mtime:',e,flush=True)
log_f=open(log,'wb'); err_f=open(os.path.join(B,f"{name}_err.log"),'wb')
p=subprocess.Popen([exe],cwd=B,env=env,stdout=log_f,stderr=err_f)
t0=time.time(); shots=[]
for t in ticks:
    time.sleep(max(0,t0+t-time.time()))
    actual=(time.time()-t0)
    if p.poll() is not None:
        shots.append((t,round(actual,1),'exited',p.poll(),None))
        # Rule-19: a missing *_samples_t*.txt is otherwise indistinguishable from
        # "attempted, zero tids". Emit an explicit stub for every future sample tick
        # so absence is a recorded cause, not a silent zero.
        for _st in sorted(s for s in sample_ticks if s>=t):
            _sf=os.path.join(B,f"{name}_samples_t{_st:03d}.txt")
            if not os.path.exists(_sf):
                with open(_sf,"w") as _f:
                    _f.write(f"=== gate {name} samples planned={_st} NOT TAKEN: guest exited rc={p.poll()} at actual={actual:.2f}s ===\n")
                    _f.write(f"=== {selftest_line} drawn={tail_drawn(log)} pids={mcla_pids()} ===\n")
        break
    h=win(p.pid)
    # Maximize BEFORE the shot so the PNG proves the resized state (old code shot first).
    if max_tick==t and h:
        u.ShowWindow(W.HWND(h),3); shots.append((t,round(actual,1),'maximize',0,None)); time.sleep(0.5)
        actual=(time.time()-t0)
        if t not in shot_ticks:
            try:
                im=grab(h); im.save(os.path.join(out,f"t{t:03d}_max.png"))
                c=im.convert('RGB').resize((320,180)).getcolors(1<<20)
                n=len(c) if c else 1<<20; top=max(c)[1] if c else 0
                shots.append((t,round(actual,1),'maxshot',n,top,tail_drawn(log)))
            except Exception as e: shots.append((t,round(actual,1),'grabfail',str(e)[:30],None))
    if t in shot_ticks:
        if not h: shots.append((t,round(actual,1),'nowin',0,None))
        else:
            try: im=grab(h)
            except Exception as e: shots.append((t,round(actual,1),'grabfail',str(e)[:30],None))
            else:
                im.save(os.path.join(out,f"t{t:03d}.png"))
                c=im.convert('RGB').resize((320,180)).getcolors(1<<20)
                n=len(c) if c else 1<<20; top=max(c)[1] if c else 0
                shots.append((t,round(actual,1),n,top,tail_drawn(log)))
    # GATE_KEYS="170:13" posts a real Windows key (VK code) to the game window at t:
    # tests the native input path end to end; nothing is injected inside the guest.
    if t in key_map:
        if not h: shots.append((t,round(actual,1),'nowin-key',0,None))
        else:
            for vk in key_map[t]:
                sc=u.MapVirtualKeyW(int(vk),0)
                u.PostMessageW(W.HWND(h),0x100,int(vk),1|(sc<<16)); time.sleep(0.25)
                u.PostMessageW(W.HWND(h),0x101,int(vk),1|(sc<<16)|(3<<30)); shots.append((t,round(actual,1),'key',vk,tail_drawn(log)))
            # Post-key frame 2s after the key: M4 gates "changes screen within 2s".
            # Old code shot only the pre-key frame and waited for the next coarse tick.
            time.sleep(1.75)
            actual=(time.time()-t0)
            try:
                im=grab(h); im.save(os.path.join(out,f"t{t:03d}_key.png"))
                c=im.convert('RGB').resize((320,180)).getcolors(1<<20)
                n=len(c) if c else 1<<20; top=max(c)[1] if c else 0
                shots.append((t,round(actual,1),'keyshot',n,top,tail_drawn(log)))
            except Exception as e: shots.append((t,round(actual,1),'keyshotfail',str(e)[:30],None))
    if t in sample_ticks:
        sample_file=os.path.join(B,f"{name}_samples_t{t:03d}.txt")
        _pids=mcla_pids()
        with open(sample_file,"w") as sf:
            sf.write(f"=== gate {name} samples planned={t} actual={time.time()-t0:.2f}s pid={p.pid} exe={exe} map={map_path} drawn={tail_drawn(log)} pids={_pids} {selftest_line} ===\n")
            if len(_pids)!=1:
                sf.write(f"=== WARN pids!=1: thread_sample find_pid keeps the LAST mcla.exe, gate pid={p.pid} may differ ===\n")
            try:
                if os.path.getmtime(map_path)<os.path.getmtime(exe):
                    sf.write("=== WARN map older than exe: symbols may be stale ===\n")
            except Exception:
                pass
            if not os.path.exists(map_path):
                sf.write("=== MAP MISSING: thread_sample not invoked ===\n")
            else:
                for s_idx in range(3):
                    sf.write(f"=== SAMPLE {s_idx+1} at t={time.time()-t0:.2f}s pids={mcla_pids()} ===\n")
                    sf.flush()
                    try:
                        res=subprocess.run([sys.executable,os.path.join(os.path.dirname(__file__),"thread_sample.py"),map_path],
                                           capture_output=True,text=True,timeout=30)
                        sf.write(res.stdout or "")
                        if res.stderr: sf.write(res.stderr)
                        _out=res.stdout or ""
                        _tids=sum(1 for _L in _out.splitlines() if _L.startswith("tid="))
                        sf.write(f"=== tids={_tids} bytes={len(_out)} rc={res.returncode} ===\n")
                        if _tids==0:
                            sf.write("=== ZERO TIDS: sampler did not fire (mcla.exe not running / OpenThread/GetThreadContext failed); do NOT read as guest-idle ===\n")
                    except subprocess.TimeoutExpired:
                        sf.write("=== TIMEOUT 30s: thread_sample hung, continuing ===\n")
                        sf.write("=== WARN timeout kill bypasses thread_sample finally/resume: later samples in this file may see suspended threads ===\n")
                    sf.flush()
                    if s_idx<2: time.sleep(1)
rc=p.poll()
if rc is None:
    time.sleep(max(0,t0+secs-time.time())); rc=p.poll()
    if rc is None: p.kill(); p.wait(); rc='killed(alive)'
try: log_f.close()
except Exception: pass
try: err_f.close()
except Exception: pass
print(name,'rc',rc,'elapsed',round(time.time()-t0))
for s in shots: print(' t=%s actual=%s %s'%(s[0],s[1],' '.join(str(x) for x in s[2:])))
mx,rows=stream_drawn_max(log)
print(' log MB',os.path.getsize(log)>>20,'drawn max',mx,'rows',rows)
