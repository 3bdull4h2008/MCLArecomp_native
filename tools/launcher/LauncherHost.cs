using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.Wpf;

namespace MclaLauncher
{
    // WPF per-pixel-alpha WebView2 host: AllowsTransparency=True + DefaultBackgroundColor
    // Transparent + the UI's transparent-window class => true see-through with working input.
    public class LauncherHost : Window
    {
        private readonly WebView2 _wv;
        private readonly string _exeDir, _uiDir, _repoRoot, _mclaExe, _cacheDir;
        private Process _game;
        private Dictionary<string, object> _lastResolved, _lastRawConfig;

        public LauncherHost()
        {
            _exeDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/');
            _repoRoot = FindRepoRoot(_exeDir);
            _uiDir = ResolveUiDir(_exeDir, _repoRoot);
            _mclaExe = ResolveMclaExe(_exeDir, _repoRoot);
            _cacheDir = ResolveCacheDir(_exeDir, _repoRoot);
            Title = "Midnight Club: Los Angeles — Launcher";
            Width = 650; Height = 446;
            WindowStyle = WindowStyle.None;
            ResizeMode = ResizeMode.NoResize;
            AllowsTransparency = true;
            Background = System.Windows.Media.Brushes.Transparent;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            _wv = new WebView2();
            Content = _wv;
            Loaded += async (s, e) => await InitWebView();
        }

        static string FindRepoRoot(string d0)
        { var d = new DirectoryInfo(d0); for (int i=0;i<8&&d!=null;i++,d=d.Parent) if (File.Exists(Path.Combine(d.FullName,"CMakeLists.txt")) && Directory.Exists(Path.Combine(d.FullName,"src"))) return d.FullName; return d0; }
        static string ResolveUiDir(string e, string r)
        { foreach (var c in new[]{Path.Combine(e,"launcher","ui"),Path.Combine(e,"ui"),Path.Combine(r,"tools","launcher","ui")}) { var p=Path.GetFullPath(c); if(File.Exists(Path.Combine(p,"index.html"))) return p; } return Path.Combine(r,"tools","launcher","ui"); }
        static string ResolveMclaExe(string e, string r)
        { foreach (var c in new[]{Path.Combine(e,"mcla.exe"),Path.Combine(e,"build","mcla.exe"),Path.Combine(r,"build","mcla.exe")}) { var p=Path.GetFullPath(c); if(File.Exists(p)) return p; } return Path.Combine(r,"build","mcla.exe"); }
        static string ResolveCacheDir(string e, string r)
        { var m=Path.GetDirectoryName(ResolveMclaExe(e,r))??e; foreach(var c in new[]{Path.Combine(e,"cache"),Path.Combine(m,"cache"),Path.Combine(r,"build","cache")}){var p=Path.GetFullPath(c); if(Directory.Exists(p)) return p;} var d=Path.Combine(r,"build","cache"); Directory.CreateDirectory(d); return d; }

        async System.Threading.Tasks.Task InitWebView()
        {
            try
            {
                var ud = Path.Combine(Path.GetTempPath(), "mcla-launcher-webview2");
                Directory.CreateDirectory(ud);
                var env = await CoreWebView2Environment.CreateAsync(null, ud);
                await _wv.EnsureCoreWebView2Async(env);
                _wv.DefaultBackgroundColor = System.Drawing.Color.Transparent;
                _wv.CoreWebView2.Settings.AreDevToolsEnabled = false;
                _wv.WebMessageReceived += OnWebMessage;
                _wv.CoreWebView2.SetVirtualHostNameToFolderMapping("mcla.launcher", _uiDir, CoreWebView2HostResourceAccessKind.Allow);
                _wv.NavigationCompleted += async (s2, e2) =>
                {
                    await _wv.ExecuteScriptAsync("document.documentElement.classList.add('transparent-window');document.body.classList.add('transparent-window');");
                    PushConfigToUi();
                };
                _wv.Source = new Uri("https://mcla.launcher/index.html?transparent=1");
            }
            catch (Exception ex) { MessageBox.Show("WebView2 init failed: " + ex.Message, "MCLA Launcher"); Close(); }
        }

        void OnWebMessage(object sender, CoreWebView2WebMessageReceivedEventArgs e)
        {
            try
            {
                var msg = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(e.WebMessageAsJson);
                if (msg == null) return;
                object a; var action = msg.TryGetValue("action", out a) ? Convert.ToString(a) : "";
                object p; msg.TryGetValue("payload", out p); var payload = p as Dictionary<string, object>;
                object ro; Dictionary<string, object> resolved = null;
                if (payload != null && payload.TryGetValue("extra", out ro)) resolved = ro as Dictionary<string, object>;
                if (resolved != null && resolved.ContainsKey("toml")) _lastResolved = resolved;
                if (payload != null && payload.ContainsKey("config")) _lastRawConfig = payload["config"] as Dictionary<string, object>;
                Dispatcher.Invoke(() => HandleAction(action ?? ""));
            }
            catch (Exception ex) { Debug.WriteLine(ex); }
        }

        void HandleAction(string action)
        {
            switch (action)
            {
                case "ready": PushConfigToUi(); break;
                case "dragWindow":
                    var hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;
                    Native.ReleaseCapture();
                    Native.SendMessage(hwnd, 0x00A1, (IntPtr)0x0002, IntPtr.Zero);
                    break;
                case "minimizeWindow": WindowState = WindowState.Minimized; break;
                case "exitLauncher": Close(); break;
                case "launchGame": LaunchGame(); break;
                case "saveConfig": SaveResolved(); break;
                case "browseFiles": OpenFolder(Path.GetDirectoryName(_mclaExe) ?? _repoRoot); break;
                case "openLogs": OpenFolder(Path.Combine(Path.GetDirectoryName(_mclaExe) ?? _repoRoot, "cache")); break;
            }
        }

        void PushConfigToUi()
        {
            try
            {
                var path = Path.Combine(_cacheDir, "mcla.toml");
                if (!File.Exists(path)) return;
                var map = new Dictionary<string, object>();
                foreach (var line in File.ReadAllLines(path))
                {
                    var s = line.Trim(); if (s.Length==0||s.StartsWith("#")||s.IndexOf('=')<0) continue;
                    var eq = s.IndexOf('='); var k = s.Substring(0,eq).Trim(); var v = s.Substring(eq+1).Trim().Trim('"');
                    double num; if (double.TryParse(v, System.Globalization.NumberStyles.Any, System.Globalization.CultureInfo.InvariantCulture, out num)) map[k]=num;
                    else if (v=="true"||v=="false") map[k]=(v=="true"); else map[k]=v;
                }
                var json = new JavaScriptSerializer().Serialize(map);
                Dispatcher.Invoke(() => _wv.ExecuteScriptAsync("if(window.MCLALauncherAPI&&window.MCLALauncherAPI.loadConfig){window.MCLALauncherAPI.loadConfig(" + json + ");}"));
            }
            catch { }
        }

        static string GetOpt(Dictionary<string, object> c, string tab, string key)
        { if(c==null)return null; object t; if(!c.TryGetValue(tab,out t))return null; var o=t as Dictionary<string,object>; if(o==null)return null; object v; return o.TryGetValue(key,out v)?Convert.ToString(v):null; }

        static Dictionary<string,string> BuildEnv(Dictionary<string,object> r, Dictionary<string,object> raw)
        {
            var e = new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            int scale=2; object d;
            if(r!=null && r.TryGetValue("toml",out d) && d is Dictionary<string,object>)
            { var toml=(Dictionary<string,object>)d; object ds;
              if(toml.TryGetValue("display",out ds) && ds is Dictionary<string,object>)
              { var s=(Dictionary<string,object>)ds; object v;
                if(s.TryGetValue("resolution_scale",out v)) int.TryParse(Convert.ToString(v),out scale);
                if(s.TryGetValue("vsync",out v) && v is bool && (bool)v) e["MCLA_VSYNC"]="1";
                if(s.TryGetValue("fullscreen",out v)) e["MCLA_DISPLAY_MODE"]=(bool)v?"borderless_fullscreen":"windowed";
                if(s.TryGetValue("mcla_fps",out v)){int f;int.TryParse(Convert.ToString(v),out f); if(f>0)e["MCLA_FPS_CAP"]=Convert.ToString(f);} } }
            if(scale<1)scale=2;
            e["MCLA_RENDER_SCALE"]=Convert.ToString(scale);
            e["MCLA_WINDOW_W"]=scale==1?"1280":"2560"; e["MCLA_WINDOW_H"]=scale==1?"720":"1440";
            var pr=GetOpt(raw,"controls","prompts"); if(!string.IsNullOrEmpty(pr)) e["MCLA_BUTTON_PROMPTS"]=pr;
            return e;
        }

        static List<string> CliArgs(Dictionary<string,object> r)
        { var l=new List<string>(); object v; if(r!=null && r.TryGetValue("cliArgs",out v)){var a=v as object[]; if(a!=null)foreach(var x in a)l.Add(Convert.ToString(x));} return l; }

        static string TVal(object v)
        { if(v==null)return "\"\""; if(v is bool)return ((bool)v)?"true":"false"; if(v is int||v is long||v is short||v is byte||v is decimal||v is double||v is float)return Convert.ToString(v,System.Globalization.CultureInfo.InvariantCulture); var s=Convert.ToString(v)??""; bool b;if(bool.TryParse(s,out b))return b?"true":"false"; double d;if(double.TryParse(s,System.Globalization.NumberStyles.Any,System.Globalization.CultureInfo.InvariantCulture,out d))return s; return "\""+s.Replace("\\","\\\\").Replace("\"","\\\"")+"\""; }

        void SaveResolved()
        {
            try
            { if(_lastResolved==null)return; object o; if(!_lastResolved.TryGetValue("toml",out o))return; var toml=o as Dictionary<string,object>; if(toml==null)return;
              Directory.CreateDirectory(_cacheDir); var path=Path.Combine(_cacheDir,"mcla.toml");
              var lines=File.Exists(path)?new List<string>(File.ReadAllLines(path)):new List<string>();
              foreach(var sec in toml){ var vals=sec.Value as Dictionary<string,object>; if(vals==null)continue; int st=-1; for(int i=0;i<lines.Count;i++)if(lines[i].TrimStart().StartsWith("["+sec.Key+"]",StringComparison.OrdinalIgnoreCase)){st=i;break;} if(st<0){lines.Add("["+sec.Key+"]");st=lines.Count-1;}
                foreach(var kv in vals){ bool f=false; for(int i=st+1;i<lines.Count;i++){var t=lines[i].TrimStart(); if(t.StartsWith("["))break; if(t.StartsWith("#")||t.IndexOf('=')<0)continue; var k=t.Split('=')[0].Trim(); if(string.Equals(k,kv.Key,StringComparison.OrdinalIgnoreCase)){lines[i]=kv.Key+" = "+TVal(kv.Value);f=true;break;}} if(!f)lines.Insert(st+1,kv.Key+" = "+TVal(kv.Value)); } }
              File.WriteAllLines(path,lines.ToArray(),new UTF8Encoding(false));
            }
            catch(Exception ex){MessageBox.Show("saveConfig: "+ex.Message);}
        }

        void LaunchGame()
        {
            try
            { if(_game!=null&&!_game.HasExited)return; if(!File.Exists(_mclaExe)){MessageBox.Show("mcla.exe not found:\n"+_mclaExe);return;} SaveResolved();
              var psi=new ProcessStartInfo(_mclaExe){WorkingDirectory=Path.GetDirectoryName(_mclaExe)??_repoRoot,UseShellExecute=false};
              foreach(var kv in BuildEnv(_lastResolved,_lastRawConfig))psi.EnvironmentVariables[kv.Key]=kv.Value;
              foreach(var a in CliArgs(_lastResolved))psi.Arguments+="\""+a.Replace("\"","\\\"")+"\" ";
              _game=Process.Start(psi);
            }
            catch(Exception ex){MessageBox.Show("Launch failed: "+ex.Message);}
        }

        void OpenFolder(string path){ try{Directory.CreateDirectory(path);Process.Start(new ProcessStartInfo("explorer.exe","\""+path+"\""){UseShellExecute=true});}catch{} }

        [STAThread] public static void Main(){ var app=new Application(); app.Run(new LauncherHost()); }
    }
    internal static class Native
    {
        [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool ReleaseCapture();
        [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
    }
}
