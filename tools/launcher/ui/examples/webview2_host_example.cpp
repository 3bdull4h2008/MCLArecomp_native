// Drop-in Win32 WebView2 message handler for C++ MCLA-Recomp launcher
// Receives JSON from window.chrome.webview.postMessage({ action, payload })
// payload.resolved.cliArgs -> ["--mcla_fps=60", "--mcla_res_scale=2", ...]
// payload.resolved.toml    -> { "display": { "frame_rate": 60, ... }, ... }
/*
webview->add_WebMessageReceived(
    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [hwnd](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string msg;
            args->get_WebMessageAsJson(&msg);
            // Parse action: "launchGame", "saveConfig", "minimizeWindow", "exitLauncher"
            return S_OK;
        }).Get(), &token);
*/
