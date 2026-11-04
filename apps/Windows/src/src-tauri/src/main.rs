#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod control;
mod network;

use std::{fs, path::PathBuf, sync::{Arc, Mutex}};
use serde_json::Value;
use tauri::{Manager, State, WindowEvent};

struct AppState {
    device: Mutex<Option<String>>,
    control: Arc<control::ControlManager>,
    http_gate: Arc<Mutex<()>>,
}

fn current_device(state: &AppState) -> Result<String, String> {
    state.device.lock().map_err(|_| "Connection lock failed".to_string())?
        .clone().ok_or_else(|| "Connect to an adapter first.".to_string())
}

#[tauri::command]
async fn discover_devices() -> Result<Vec<network::FoundDevice>, String> {
    tauri::async_runtime::spawn_blocking(network::discover)
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn connect_device(address: String, state: State<'_, AppState>) -> Result<Value, String> {
    let base = network::normalize_address(&address)?;
    let copy = base.clone();
    let gate = Arc::clone(&state.http_gate);
    let status = tauri::async_runtime::spawn_blocking(move || {
        let _guard = gate.lock().map_err(|_| network::RequestError { status: 0, code: "locked".into(), detail: "HTTP queue unavailable".into() })?;
        network::request(&copy, "/api/status", None)
    })
        .await.map_err(|e| e.to_string())?.map_err(|e| e.detail)?;
    if status.get("remote_api").and_then(Value::as_u64).unwrap_or(0) < 2 {
        return Err("This firmware does not advertise the required remote API v2.".into());
    }
    state.control.stop();
    let manager = Arc::clone(&state.control);
    tauri::async_runtime::spawn_blocking(move || manager.wait_for_release())
        .await.map_err(|e| e.to_string())??;
    *state.device.lock().map_err(|_| "Device lock failed".to_string())? = Some(base);
    Ok(status)
}

#[tauri::command]
fn disconnect_device(state: State<'_, AppState>) -> Result<(), String> {
    state.control.stop();
    *state.device.lock().map_err(|_| "Device lock failed".to_string())? = None;
    Ok(())
}

#[tauri::command]
async fn device_api(path: String, body: Option<Value>, state: State<'_, AppState>) -> Result<Value, String> {
    if !network::allowed_route(&path, body.is_some()) {
        return Err("This request is not supported by the desktop companion.".into());
    }
    let base = current_device(&state)?;
    let gate = Arc::clone(&state.http_gate);
    tauri::async_runtime::spawn_blocking(move || {
        let _guard = gate.lock().map_err(|_| network::RequestError { status: 0, code: "locked".into(), detail: "HTTP queue unavailable".into() })?;
        network::request(&base, &path, body.as_ref())
    })
        .await.map_err(|e| e.to_string())?.map_err(|e| e.detail)
}

#[tauri::command]
async fn take_control(state: State<'_, AppState>) -> Result<(), String> {
    let base = current_device(&state)?;
    let manager = Arc::clone(&state.control);
    let gate = Arc::clone(&state.http_gate);
    tauri::async_runtime::spawn_blocking(move || {
        let _guard = gate.lock().map_err(|_| "HTTP queue unavailable".to_string())?;
        manager.take(&base)
    })
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
async fn release_control(state: State<'_, AppState>) -> Result<(), String> {
    let manager = Arc::clone(&state.control);
    manager.stop();
    tauri::async_runtime::spawn_blocking(move || manager.wait_for_release())
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
fn set_keyboard_input(input: control::Pad, state: State<'_, AppState>) -> Result<(), String> {
    state.control.set_keyboard(input)
}

#[tauri::command]
fn set_input_source(source: String, slot: u8, state: State<'_, AppState>) -> Result<(), String> {
    state.control.set_source(&source, slot)
}

#[tauri::command]
fn set_input_focus(focused: bool, state: State<'_, AppState>) {
    state.control.set_focused(focused);
}

#[tauri::command]
fn control_status(state: State<'_, AppState>) -> control::ControlStatus {
    state.control.status()
}

fn settings_path() -> Result<PathBuf, String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let dir = exe.parent().ok_or("Unable to resolve application folder")?;
    Ok(dir.join("app-data").join("settings.json"))
}

#[tauri::command]
fn load_preferences() -> Result<Value, String> {
    let path = settings_path()?;
    if !path.exists() { return Ok(serde_json::json!({})); }
    let bytes = fs::read(&path).map_err(|e| e.to_string())?;
    if bytes.len() > 64 * 1024 { return Err("Preferences file is too large.".into()); }
    serde_json::from_slice(&bytes).map_err(|e| format!("Invalid settings file: {e}"))
}

#[tauri::command]
fn save_preferences(preferences: Value) -> Result<(), String> {
    if !preferences.is_object() { return Err("Preferences must be an object".into()); }
    let bytes = serde_json::to_vec_pretty(&preferences).map_err(|e| e.to_string())?;
    if bytes.len() > 64 * 1024 { return Err("Preferences are too large".into()); }
    let path = settings_path()?;
    fs::create_dir_all(path.parent().ok_or("No settings directory")?)
        .map_err(|e| format!("Cannot create portable settings directory: {e}"))?;
    let tmp = path.with_extension("tmp");
    fs::write(&tmp, bytes).map_err(|e| e.to_string())?;
    if path.exists() { fs::remove_file(&path).map_err(|e| e.to_string())?; }
    fs::rename(tmp, path).map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .manage(AppState { device: Mutex::new(None), control: Arc::new(control::ControlManager::new()),
            http_gate: Arc::new(Mutex::new(())) })
        .invoke_handler(tauri::generate_handler![
            discover_devices, connect_device, disconnect_device, device_api,
            take_control, release_control, set_keyboard_input, set_input_source,
            set_input_focus, control_status, load_preferences, save_preferences
        ])
        .on_window_event(|window, event| {
            if let WindowEvent::Focused(focused) = event {
                window.state::<AppState>().control.set_focused(*focused);
            }
            if let WindowEvent::CloseRequested { .. } = event {
                window.state::<AppState>().control.stop();
            }
        })
        .run(tauri::generate_context!())
        .expect("Failed to start DC-OGX-W Windows companion");
}
