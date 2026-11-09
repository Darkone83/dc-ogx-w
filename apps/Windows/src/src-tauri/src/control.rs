//! Native controller session. All messages are complete snapshots, never a replay queue.
//! Lease: 5 s, input deadman: 250 ms; pause means send neutral and keep ownership.
use std::{
    net::TcpStream,
    sync::{atomic::{AtomicBool, AtomicU8, AtomicU32, Ordering}, Arc, Mutex},
    thread,
    time::{Duration, Instant},
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tungstenite::{client::client_with_config, protocol::WebSocketConfig, Message, WebSocket};
use super::network;

#[derive(Debug, Default, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub struct Pad {
    pub buttons: u16,
    pub dpad: u8,
    pub lx: i16,
    pub ly: i16,
    pub rx: i16,
    pub ry: i16,
    pub lt: u8,
    pub rt: u8,
}
impl Pad {
    // Allow normal stick drift only when arming after a pause. Outgoing stick
    // values are never filtered here; the adapter's calibration stays authoritative.
    fn neutral(self) -> bool {
        self.buttons == 0 && self.dpad == 0 && self.lt <= 8 && self.rt <= 8 &&
            [self.lx, self.ly, self.rx, self.ry].iter().all(|v| v.unsigned_abs() <= 4096)
    }
    fn valid(self) -> bool {
        self.buttons <= 0x0fff && self.dpad <= 15 &&
            (self.dpad & 3) != 3 && (self.dpad & 12) != 12
    }
}

struct Session {
    wanted: AtomicBool,
    alive: AtomicBool,
    token: AtomicU32,
    rate_hz: u32,
    return_physical: bool,
    release_error: Mutex<Option<String>>,
    error: Mutex<Option<String>>,
    last_sent: Mutex<Pad>,
}
impl Session {
    fn new(rate_hz: u32, token: u32, return_physical: bool) -> Self {
        Self { wanted: AtomicBool::new(true), alive: AtomicBool::new(true), token: AtomicU32::new(token),
            rate_hz, return_physical, release_error: Mutex::new(None), error: Mutex::new(None), last_sent: Mutex::new(Pad::default()) }
    }
}

#[derive(Serialize)]
pub struct ControlStatus {
    pub active: bool,
    pub token_held: bool,
    pub rate_hz: u32,
    pub source: &'static str,
    pub focused: bool,
    pub error: Option<String>,
    pub last_sent: Pad,
    pub starting: bool,
    pub needs_neutral: bool,
    pub gamepad_connected: Option<bool>,
}

struct StartingGuard<'a>(&'a AtomicBool);
impl Drop for StartingGuard<'_> {
    fn drop(&mut self) { self.0.store(false, Ordering::SeqCst); }
}

pub struct ControlManager {
    current: Mutex<Option<Arc<Session>>>,
    keyboard: Mutex<Pad>,
    source: AtomicU8, // 0 = keyboard, 1 = native XInput, 2 = standard browser gamepad
    slot: AtomicU8,
    focused: AtomicBool,
    needs_neutral: AtomicBool,
    starting: AtomicBool,
    generation: AtomicU32,
    browser_updated: Mutex<Option<Instant>>,
    gamepad_connected: AtomicBool,
}
impl ControlManager {
    pub fn new() -> Self {
        Self { current: Mutex::new(None), keyboard: Mutex::new(Pad::default()),
            source: AtomicU8::new(0), slot: AtomicU8::new(0),
            focused: AtomicBool::new(true), needs_neutral: AtomicBool::new(true),
            starting: AtomicBool::new(false), generation: AtomicU32::new(0),
            browser_updated: Mutex::new(None), gamepad_connected: AtomicBool::new(false) }
    }
    pub fn set_keyboard(&self, state: Pad) -> Result<(), String> {
        if !state.valid() { return Err("Invalid controller state".into()); }
        *self.keyboard.lock().map_err(|_| "Input lock failed")? = state;
        if self.source.load(Ordering::SeqCst) == 2 {
            *self.browser_updated.lock().map_err(|_| "Input clock unavailable")? = Some(Instant::now());
        }
        Ok(())
    }
    pub fn set_source(&self, source: &str, slot: u8) -> Result<(), String> {
        if slot > 3 { return Err("Controller slot must be 0-3".into()); }
        let value = match source { "keyboard" => 0, "gamepad" => 1, "browser" => 2, _ => return Err("Unsupported input source".into()) };
        self.source.store(value, Ordering::SeqCst);
        self.slot.store(slot, Ordering::SeqCst);
        self.gamepad_connected.store(false, Ordering::SeqCst);
        self.needs_neutral.store(true, Ordering::SeqCst);
        *self.keyboard.lock().map_err(|_| "Input lock failed")? = Pad::default();
        *self.browser_updated.lock().map_err(|_| "Input clock unavailable")? = None;
        Ok(())
    }
    pub fn set_focused(&self, focused: bool) {
        self.focused.store(focused, Ordering::SeqCst);
        if !focused {
            self.needs_neutral.store(true, Ordering::SeqCst);
            if let Ok(mut pad) = self.keyboard.lock() { *pad = Pad::default(); }
        }
    }
    pub fn stop(&self) {
        self.generation.fetch_add(1, Ordering::SeqCst);
        if let Ok(guard) = self.current.lock() {
            if let Some(s) = guard.as_ref() { s.wanted.store(false, Ordering::SeqCst); }
        }
        if let Ok(mut pad) = self.keyboard.lock() { *pad = Pad::default(); }
        self.needs_neutral.store(true, Ordering::SeqCst);
    }
    pub fn wait_for_release(&self) -> Result<(), String> {
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            let session = self.current.lock().map_err(|_| "Session lock failed")?.clone();
            if !self.starting.load(Ordering::SeqCst) &&
                session.as_ref().is_none_or(|s| !s.alive.load(Ordering::SeqCst)) {
                return session.as_ref().and_then(|s| s.release_error.lock().ok().and_then(|mut e| e.take()))
                    .map_or(Ok(()), Err);
            }
            if Instant::now() >= deadline { return Err("The controller is still releasing. Try again after it finishes.".into()); }
            thread::sleep(Duration::from_millis(20));
        }
    }
    pub fn status(&self) -> ControlStatus {
        let source = match self.source.load(Ordering::SeqCst) { 1 => "gamepad", 2 => "browser", _ => "keyboard" };
        let mut result = ControlStatus { active: false, token_held: false, rate_hz: 60, source,
            focused: self.focused.load(Ordering::SeqCst), error: None, last_sent: Pad::default(),
            starting: self.starting.load(Ordering::SeqCst), needs_neutral: self.needs_neutral.load(Ordering::SeqCst),
            gamepad_connected: if source == "gamepad" { Some(self.gamepad_connected.load(Ordering::SeqCst)) } else { None } };
        if let Ok(guard) = self.current.lock() {
            if let Some(session) = guard.as_ref() {
                result.active = session.alive.load(Ordering::SeqCst) && session.wanted.load(Ordering::SeqCst);
                result.token_held = session.token.load(Ordering::SeqCst) != 0;
                result.rate_hz = session.rate_hz;
                result.error = session.error.lock().ok().and_then(|e| e.clone());
                result.last_sent = session.last_sent.lock().map(|s| *s).unwrap_or_default();
            }
        }
        result
    }
    pub fn generation(&self) -> u32 { self.generation.load(Ordering::SeqCst) }
    pub fn take(self: &Arc<Self>, base: &str, generation: u32) -> Result<(), String> {
        if self.starting.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst).is_err() {
            return Err("A controller session is already starting.".into());
        }
        let _starting = StartingGuard(&self.starting);
        if self.generation() != generation { return Err("Controller acquisition cancelled.".into()); }
        let existing = self.current.lock().map_err(|_| "Session lock failed")?.clone();
        if existing.as_ref().is_some_and(|s| s.alive.load(Ordering::SeqCst)) {
            return Err("A controller session is already active or releasing.".into());
        }
        let status = network::request(base, "/api/status", None).map_err(|e| e.detail)?;
        self.check_generation(generation)?;
        let control = status.get("control").cloned().unwrap_or(Value::Null);
        if control.get("maintenance").and_then(Value::as_bool) == Some(true) {
            return Err("Firmware maintenance is active; input is unavailable.".into());
        }
        if control.get("active").and_then(Value::as_bool) == Some(true) {
            return Err("Another client has control. Release it before taking control.".into());
        }
        let physical_supported = control.get("physical_supported").and_then(Value::as_bool) == Some(true) ||
            control.get("bluetooth_supported").and_then(Value::as_bool) == Some(true);
        let rate = status.pointer("/network/input_hz").and_then(Value::as_u64)
            .unwrap_or(60).clamp(10, 60) as u32;
        let mut return_physical = false;
        let mut granted_token = None;
        let mut websocket = None;
        let result = (|| -> Result<(), String> {
            // Follow the WebUI: mutate once, then poll only the small mode reply.
            if control.get("mode").and_then(Value::as_str) != Some("wifi") ||
                control.get("mode_pending").and_then(Value::as_bool) == Some(true) {
                let state = request_mode(base, "wifi")?;
                return_physical = physical_supported;
                wait_for_mode(base, "wifi", state, || self.check_generation(generation))?;
            }
            self.check_generation(generation)?;
            websocket = Some(connect_stream(base)?);
            self.check_generation(generation)?;
            let reply = network::request(base, "/api/control/acquire", Some(&json!({"client":"windows"})))
                .map_err(|err| err.detail)?;
            let token = reply.get("token").and_then(Value::as_u64).ok_or("Adapter did not return an ownership token")?;
            if token == 0 || token > u32::MAX as u64 { return Err("Invalid ownership token".into()); }
            granted_token = Some(token as u32);
            return_physical = physical_supported;
            self.check_generation(generation)?;
            self.needs_neutral.store(true, Ordering::SeqCst);
            *self.keyboard.lock().map_err(|_| "Input lock failed")? = Pad::default();
            let session = Arc::new(Session::new(rate, token as u32, return_physical));
            let mut guard = self.current.lock().map_err(|_| "Session lock failed")?;
            self.check_generation(generation)?;
            *guard = Some(Arc::clone(&session));
            let manager = Arc::clone(self);
            let worker_base = base.to_string();
            let worker_session = Arc::clone(&session);
            let mut ws = websocket.take().unwrap();
            match thread::Builder::new().name("ogx-w-input".into()).spawn(move || {
                run_session(&manager, &worker_session, &worker_base, &mut ws);
            }) {
                Ok(_) => { granted_token = None; return_physical = false; Ok(()) },
                Err(err) => { *guard = None; Err(format!("Cannot start the input worker: {err}")) }
            }
        })();
        if let Err(mut error) = result {
            let released = granted_token.is_some_and(|token|
                network::request(base, "/api/control/release", Some(&json!({"token":token}))).is_ok());
            if let Some(mut ws) = websocket { close_stream(&mut ws); }
            if return_physical {
                if let Err(cleanup) = restore_physical(base, released) {
                    error.push_str(&format!(" Physical-mode handback failed: {cleanup}"));
                }
            }
            return Err(error);
        }
        Ok(())
    }
    fn check_generation(&self, generation: u32) -> Result<(), String> {
        if self.generation() != generation { Err("Controller acquisition cancelled.".into()) } else { Ok(()) }
    }
    fn sample(&self) -> Pad {
        if !self.focused.load(Ordering::SeqCst) { return Pad::default(); }
        let source = self.source.load(Ordering::SeqCst);
        if source == 2 && !self.browser_updated.lock().ok().and_then(|t| *t)
            .is_some_and(|t| t.elapsed() < Duration::from_millis(200)) {
            self.needs_neutral.store(true, Ordering::SeqCst);
            return Pad::default();
        }
        let input = if source != 1 {
            self.keyboard.lock().map(|p| *p).unwrap_or_default()
        } else {
            match xinput::read(self.slot.load(Ordering::SeqCst)) {
                Some(pad) => { self.gamepad_connected.store(true, Ordering::SeqCst); pad },
                None => { self.gamepad_connected.store(false, Ordering::SeqCst); self.needs_neutral.store(true, Ordering::SeqCst); return Pad::default(); }
            }
        };
        if !input.valid() { self.needs_neutral.store(true, Ordering::SeqCst); return Pad::default(); }
        if self.needs_neutral.load(Ordering::SeqCst) {
            if input.neutral() { self.needs_neutral.store(false, Ordering::SeqCst); }
            return Pad::default();
        }
        input
    }
}

// A mode POST with a lost reply is reconciled by GET, never replayed blindly.
fn request_mode(base: &str, mode: &str) -> Result<Value, String> {
    match network::request(base, "/api/control/mode", Some(&json!({"mode":mode}))) {
        Ok(state) => Ok(state),
        Err(error) if error.status == 0 => {
            let state = network::request(base, "/api/control/mode", None).map_err(|e| e.detail)?;
            if state.get("mode").and_then(Value::as_str) == Some(mode) ||
                state.get("requested_mode").and_then(Value::as_str) == Some(mode) { Ok(state) }
            else { Err(error.detail) }
        },
        Err(error) => Err(error.detail),
    }
}

fn wait_for_mode(base: &str, mode: &str, mut state: Value,
                 check: impl Fn() -> Result<(), String>) -> Result<(), String> {
    let deadline = Instant::now() + Duration::from_secs(8);
    loop {
        check()?;
        if state.get("mode").and_then(Value::as_str) == Some(mode) &&
            state.get("mode_pending").and_then(Value::as_bool) != Some(true) { return Ok(()); }
        if Instant::now() >= deadline { return Err(format!("The adapter did not finish switching to {mode} input.")); }
        thread::sleep(Duration::from_millis(150));
        check()?;
        state = network::request(base, "/api/control/mode", None).map_err(|e| e.detail)?;
    }
}

fn restore_physical(base: &str, released: bool) -> Result<(), String> {
    if !released {
        // The firmware also releases on socket close. Check for a new owner
        // before changing modes, which would otherwise clear that lease.
        let status = network::request(base, "/api/status", None).map_err(|e| e.detail)?;
        let control = status.get("control").cloned().unwrap_or(Value::Null);
        if control.get("active").and_then(Value::as_bool) == Some(true) {
            return Err("Another client owns control; Physical mode was not selected.".into());
        }
        if control.get("mode").and_then(Value::as_str) != Some("wifi") &&
            control.get("requested_mode").and_then(Value::as_str) != Some("wifi") { return Ok(()); }
    }
    let state = request_mode(base, "physical")?;
    wait_for_mode(base, "physical", state, || Ok(()))
}

fn close_stream(ws: &mut WebSocket<TcpStream>) {
    let _ = ws.close(None);
    // Free the stream before the mode handback opens its HTTP connection.
    let _ = ws.get_mut().shutdown(std::net::Shutdown::Both);
}

fn connect_stream(base: &str) -> Result<WebSocket<TcpStream>, String> {
    let url = url::Url::parse(base).map_err(|e| e.to_string())?;
    let addresses = url.socket_addrs(|| Some(80)).map_err(|e| format!("Cannot resolve adapter: {e}"))?;
    let mut stream = None;
    for address in addresses {
        if let Ok(socket) = TcpStream::connect_timeout(&address, Duration::from_secs(2)) { stream = Some(socket); break; }
    }
    let stream = stream.ok_or("Cannot connect to the adapter WebSocket.")?;
    stream.set_nodelay(true).map_err(|e| e.to_string())?;
    stream.set_read_timeout(Some(Duration::from_secs(2))).map_err(|e| e.to_string())?;
    stream.set_write_timeout(Some(Duration::from_secs(2))).map_err(|e| e.to_string())?;
    let mut config = WebSocketConfig::default();
    config.write_buffer_size = 0;
    config.max_write_buffer_size = 2048;
    let uri = base.replacen("http://", "ws://", 1) + "/ws";
    let (mut ws, _) = client_with_config(uri.as_str(), stream, Some(config))
        .map_err(|e| format!("Could not open the controller WebSocket: {e}"))?;
    ws.get_mut().set_read_timeout(Some(Duration::from_millis(2))).map_err(|e| e.to_string())?;
    ws.get_mut().set_write_timeout(Some(Duration::from_millis(100))).map_err(|e| e.to_string())?;
    Ok(ws)
}

fn run_session(manager: &ControlManager, session: &Session, base: &str,
               ws: &mut WebSocket<TcpStream>) {
    let token = session.token.load(Ordering::SeqCst);
    let interval = Duration::from_micros(1_000_000u64 / u64::from(session.rate_hz));
    let mut seq = 0u32;
    let mut next_frame = Instant::now();
    let mut last_frame = Instant::now();
    let mut error: Option<String> = None;
    while session.wanted.load(Ordering::SeqCst) {
        if last_frame.elapsed() >= Duration::from_millis(250) { manager.needs_neutral.store(true, Ordering::SeqCst); }
        last_frame = Instant::now();
        let pad = manager.sample();
        let msg = json!({"type":"input", "token":token, "seq":seq,
            "buttons":pad.buttons, "dpad":pad.dpad,
            "lx":pad.lx, "ly":pad.ly, "rx":pad.rx, "ry":pad.ry,
            "lt":pad.lt, "rt":pad.rt});
        if let Err(err) = ws.send(Message::Text(msg.to_string().into())) {
            error = Some(format!("Input stream disconnected: {err}"));
            break;
        }
        // Flush immediately; never buffer stale gamepad snapshots in userspace.
        if let Err(err) = ws.flush() {
            error = Some(format!("Input output stalled: {err}"));
            break;
        }
        if let Ok(mut last) = session.last_sent.lock() { *last = pad; }
        seq = seq.wrapping_add(1);
        match ws.read() {
            Ok(Message::Close(frame)) => {
                error = Some(match frame {
                    Some(frame) => format!("Adapter closed the control stream ({}): {}", frame.code, frame.reason),
                    None => "Adapter closed the control stream without a close reason.".into(),
                });
                break;
            }
            Ok(Message::Text(text)) => {
                if let Ok(reply) = serde_json::from_str::<Value>(&text) {
                    if reply.get("ok").and_then(Value::as_bool) == Some(false) &&
                        reply.get("error").and_then(Value::as_str) != Some("service_busy") {
                        error = Some(format!("Adapter rejected control: {}",
                            reply.get("error").and_then(Value::as_str).unwrap_or("unknown error")));
                        break;
                    }
                }
            }
            Ok(_) => {},
            Err(tungstenite::Error::Io(err)) if err.kind() == std::io::ErrorKind::WouldBlock ||
                err.kind() == std::io::ErrorKind::TimedOut => {},
            Err(err) => { error = Some(format!("Control socket error: {err}")); break; },
        }
        next_frame += interval;
        let now = Instant::now();
        if next_frame > now { thread::sleep(next_frame - now); }
        else { next_frame = now; }
    }
    // Send a neutral final state when possible; then relinquish only this token.
    session.wanted.store(false, Ordering::SeqCst);
    let neutral = json!({"type":"input","token":token,"seq":seq,
        "buttons":0,"dpad":0,"lx":0,"ly":0,"rx":0,"ry":0,"lt":0,"rt":0});
    let _ = ws.send(Message::Text(neutral.to_string().into()));
    let _ = ws.flush();
    let release = network::request(base, "/api/control/release", Some(&json!({"token":token})));
    close_stream(ws);
    let cleanup = if session.return_physical { restore_physical(base, release.is_ok()) }
                  else { release.map(|_| ()).map_err(|e| e.detail) };
    if let Err(detail) = cleanup {
        let detail = format!("Controller stopped, but release/Physical-mode handback was not confirmed: {detail}");
        if let Ok(mut slot) = session.release_error.lock() { *slot = Some(detail.clone()); }
        error = Some(error.map_or(detail.clone(), |prior| format!("{prior} {detail}")));
    }
    if let Ok(mut last) = session.last_sent.lock() { *last = Pad::default(); }
    if let Ok(mut slot) = session.error.lock() { *slot = error; }
    session.token.store(0, Ordering::SeqCst);
    session.alive.store(false, Ordering::SeqCst);
    manager.needs_neutral.store(true, Ordering::SeqCst);
}

mod xinput {
    use super::Pad;
    #[cfg(any(windows, test))]
    pub(super) fn translate(raw: u16, lx: i16, ly: i16, rx: i16, ry: i16, lt: u8, rt: u8) -> Pad {
        let mut buttons = 0u16;
        for (xinput, ogx) in [
            (0x1000, 1), (0x2000, 2), (0x4000, 4), (0x8000, 8),
            (0x0040, 16), (0x0080, 32), (0x0020, 64), (0x0010, 128),
            (0x0100, 256), (0x0200, 512),
        ] { if raw & xinput != 0 { buttons |= ogx; } }
        let mut dpad = (raw & 15) as u8;
        if dpad & 3 == 3 { dpad &= !3; }
        if dpad & 12 == 12 { dpad &= !12; }
        Pad { buttons, dpad, lx, ly, rx, ry, lt, rt }
    }
    #[cfg(windows)]
    #[repr(C)]
    #[allow(non_snake_case)]
    #[derive(Default, Clone, Copy)]
    struct XInputGamepad {
        wButtons: u16,
        bLeftTrigger: u8,
        bRightTrigger: u8,
        sThumbLX: i16,
        sThumbLY: i16,
        sThumbRX: i16,
        sThumbRY: i16,
    }
    #[cfg(windows)]
    #[repr(C)]
    #[allow(non_snake_case)]
    #[derive(Default)]
    struct XInputState { dwPacketNumber: u32, Gamepad: XInputGamepad }
    #[cfg(windows)]
    #[link(name = "Xinput9_1_0")]
    extern "system" { fn XInputGetState(index: u32, state: *mut XInputState) -> u32; }

    pub fn read(slot: u8) -> Option<Pad> {
        #[cfg(windows)]
        {
            let mut state = XInputState::default();
            if unsafe { XInputGetState(u32::from(slot), &mut state) } != 0 { return None; }
            let g = state.Gamepad;
            Some(translate(g.wButtons, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY,
                g.bLeftTrigger, g.bRightTrigger))
        }
        #[cfg(not(windows))]
        { let _ = slot; None }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn pad_rejects_opposite_dpad() {
        assert!(Pad::default().valid());
        assert!(!Pad{dpad:3,..Pad::default()}.valid());
        assert!(!Pad{dpad:12,..Pad::default()}.valid());
    }
    #[test]
    fn xinput_preserves_buttons_axes_and_white_black() {
        let p = xinput::translate(0x1301, i16::MIN, i16::MAX, 123, -456, 17, 255);
        assert_eq!(p.buttons, 1 | 256 | 512);
        assert_eq!(p.dpad, 1);
        assert_eq!((p.lx, p.ly, p.rx, p.ry, p.lt, p.rt), (i16::MIN, i16::MAX, 123, -456, 17, 255));
        assert_eq!(xinput::translate(15, 0, 0, 0, 0, 0, 0).dpad, 0);
    }
    #[test]
    fn focus_and_browser_freshness_require_neutral_before_resume() {
        let manager = ControlManager::new();
        manager.set_source("browser", 0).unwrap();
        let held = Pad { buttons: 256, ..Pad::default() };
        manager.set_keyboard(held).unwrap();
        assert_eq!(manager.sample(), Pad::default());
        // Stick drift is accepted for arming, but its raw values remain intact.
        let centre = Pad { lx: 512, ..Pad::default() };
        manager.set_keyboard(centre).unwrap();manager.sample();
        assert_eq!(manager.sample(), centre);
        manager.set_keyboard(held).unwrap();assert_eq!(manager.sample(), held);
        *manager.browser_updated.lock().unwrap() = Some(Instant::now() - Duration::from_secs(1));
        assert_eq!(manager.sample(), Pad::default());
        manager.set_keyboard(held).unwrap();assert_eq!(manager.sample(), Pad::default());
        manager.set_keyboard(Pad::default()).unwrap();manager.sample();
        manager.set_keyboard(held).unwrap();assert_eq!(manager.sample(), held);
        manager.set_focused(false);assert_eq!(manager.sample(), Pad::default());
        manager.set_keyboard(held).unwrap();manager.set_focused(true);
        assert_eq!(manager.sample(), Pad::default());
    }
}
