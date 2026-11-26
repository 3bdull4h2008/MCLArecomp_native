/**
 * MCLA PC Recomp Launcher — Host Bridge Adapter
 * Auto-detects Tauri 2, WebView2 (C++/C#), Electron, or standalone browser.
 * Preserves payload contract: { action, config, cliArgs, extra: { toml, cliArgs } }.
 *
 * Host Commands Emitted by UI:
 *   - ready           (emitted once on load; host should respond with MCLALauncherAPI.loadConfig(tomlJson))
 *   - dragWindow      (emitted on mousedown of the top bezel/header strip for FormBorderStyle=None dragging)
 *   - launchGame      (payload.extra.toml + payload.extra.cliArgs)
 *   - saveConfig      (payload.extra.toml + payload.extra.cliArgs)
 *   - optionChanged   (triggered on cycle/slider change)
 *   - restoreDefaults (triggered on Defaults button)
 *   - browseFiles     (open game directory)
 *   - openLogs        (open run.log)
 *   - minimizeWindow  (minimize host window)
 *   - exitLauncher    (close host window)
 */
(function (global) {
  'use strict';

  function buildCliArgs(schema, config) {
    const args = [];
    (schema.tabs || []).forEach((tab) => {
      (tab.options || []).forEach((opt) => {
        if (opt.visible === false || !opt.cliFlag) return;
        const rawVal = config[tab.id] && config[tab.id][opt.key];
        if (rawVal !== undefined) {
          const mapped = (opt.valueMap && Object.prototype.hasOwnProperty.call(opt.valueMap, rawVal))
            ? opt.valueMap[rawVal]
            : rawVal;
          args.push(`${opt.cliFlag}=${mapped}`);
        }
      });
    });
    return args;
  }

  function dispatchNative(actionName, payload) {
    if (global.__TAURI__ && global.__TAURI__.core && typeof global.__TAURI__.core.invoke === 'function') {
      global.__TAURI__.core.invoke('launcher_command', { action: actionName, payload: payload }).catch(console.error);
      return true;
    }
    if (global.chrome && global.chrome.webview && typeof global.chrome.webview.postMessage === 'function') {
      global.chrome.webview.postMessage({ action: actionName, payload: payload });
      return true;
    }
    if (global.electronAPI && typeof global.electronAPI.send === 'function') {
      global.electronAPI.send('launcher-command', { action: actionName, payload: payload });
      return true;
    }
    return false;
  }

  global.MCLAHostBridge = {
    buildCliArgs: buildCliArgs,
    dispatch: function (actionName, schema, config, extra) {
      const payload = {
        action: actionName,
        config: config,
        cliArgs: buildCliArgs(schema, config),
        extra: extra || null
      };
      const sent = dispatchNative(actionName, payload);
      if (!sent) {
        console.log('[MCLAHostBridge]', actionName, payload);
      }
      return payload;
    }
  };
})(window);
