// Drop-in Tauri 2 command handler for rusty-mcla / mcla-recomp launcher
use serde::Deserialize;
use std::process::Command;

#[derive(Deserialize)]
pub struct LauncherPayload {
    pub action: String,
    pub payload: serde_json::Value,
}

#[tauri::command]
pub fn launcher_command(window: tauri::Window, action: String, payload: serde_json::Value) -> Result<(), String> {
    match action.as_str() {
        "minimizeWindow" => { let _ = window.minimize(); }
        "exitLauncher"   => { let _ = window.close(); }
        "saveConfig"     => {
            // payload["resolved"]["toml"] has exact mcla.toml sections & mapped values!
            let toml_data = &payload["resolved"]["toml"];
            println!("Write mcla.toml: {}", toml_data);
        }
        "launchGame"     => {
            // payload["resolved"]["cliArgs"] is a ready-to-pass array like ["--mcla_fps=60", "--fullscreen=fullscreen"]
            let args: Vec<String> = payload["resolved"]["cliArgs"]
                .as_array()
                .unwrap_or(&vec![])
                .iter()
                .filter_map(|v| v.as_str().map(String::from))
                .collect();
            let _ = Command::new("game/mcla.exe").args(&args).spawn();
        }
        _ => println!("Unhandled launcher action: {}", action),
    }
    Ok(())
}
