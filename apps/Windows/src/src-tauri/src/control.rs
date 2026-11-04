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
use tungstenite::{connect, stream::MaybeTlsStream, Message, WebSocket};
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
    fn neutral(self) -> bool { self == Self::default() }
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
    error: Mutex<Option<String>>,
    last_sent: Mutex<Pad>,
}
impl Session {
    fn new(rate_hz: u32, token: u32) -> Self {
        Self { wanted: AtomicBool::new(true), alive: AtomicBool::new(true), token: AtomicU32::new(token),
            rate_hz, error: Mutex::new(None), last_sent: Mutex::new(Pad::default()) }
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
}

pub struct ControlManager {
    current: Mutex<Option<Arc<Session>>>,
    keyboard: Mutex<Pad>,
    source: AtomicU8, // 0 = keyboard, 1 = native XInput
    slot: AtomicU8,
    focused: AtomicBool,
    needs_neutral: AtomicBool,
}
impl ControlManager {
    pub fn new() -> Self {
        Self { current: Mutex::new(None), keyboard: Mutex::new(Pad::default()),
            source: AtomicU8::new(0), slot: AtomicU8::new(0),
            focused: AtomicBool::new(true), needs_neutral: AtomicBool::new(true) }
    }
    pub fn set_keyboard(&self, state: Pad) -> Result<(), String> {
        if !state.valid() { return Err("Invalid controller state".into()); }
        *self.keyboard.lock().map_err(|_| "Input lock failed")? = state;
        Ok(())
    }
    pub fn set_source(&self, source: &str, slot: u8) -> Result<(), String> {
        if slot > 3 { return Err("XInput slot must be 0-3".into()); }
        let value = match source { "keyboard" => 0, "gamepad" => 1, _ => return Err("Unsupported input source".into()) };
        self.source.store(value, Ordering::SeqCst);
        self.slot.store(slot, Ordering::SeqCst);
        self.needs_neutral.store(true, Ordering::SeqCst);
        *self.keyboard.lock().map_err(|_| "Input lock failed")? = Pad::default();
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
        if let Ok(guard) = self.current.lock() {
            if let Some(s) = guard.as_ref() { s.wanted.store(false, Ordering::SeqCst); }
        }
        if let Ok(mut pad) = self.keyboard.lock() { *pad = Pad::default(); }
        self.needs_neutral.store(true, Ordering::SeqCst);
    }
    pub fn wait_for_release(&self) -> Result<(), String> {
        let session = self.current.lock().map_err(|_| "Session lock failed")?.clone();
        if let Some(session) = session {
            for _ in 0..100 {
                if !session.alive.load(Ordering::SeqCst) { return Ok(()); }
                thread::sleep(Duration::from_millis(30));
            }
            return Err("The controller is still releasing. Try again after it finishes.".into());
        }
        Ok(())
    }
    pub fn status(&self) -> ControlStatus {
        let source = if self.source.load(Ordering::SeqCst) == 1 { "gamepad" } else { "keyboard" };
        let mut result = ControlStatus { active: false, token_held: false, rate_hz: 60, source,
            focused: self.focused.load(Ordering::SeqCst), error: None, last_sent: Pad::default() };
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
    pub fn take(self: &Arc<Self>, base: &str) -> Result<(), String> {
        let mut guard = self.current.lock().map_err(|_| "Session lock failed")?;
        if guard.as_ref().is_some_and(|s| s.alive.load(Ordering::SeqCst)) {
            return Err("A controller session is already active or releasing.".into());
        }
        // Check mode BEFORE taking ownership; switching modes is an explicit user action.
        let status = network::request(base, "/api/status", None).map_err(|e| e.detail)?;
        if status.pointer("/control/maintenance").and_then(Value::as_bool) == Some(true) {
            return Err("Firmware maintenance is active; input is unavailable.".into());
        }
        if status.pointer("/control/mode_pending").and_then(Value::as_bool) == Some(true) {
            return Err("The controller mode transition has not finished.".into());
        }
        if status.pointer("/control/mode").and_then(Value::as_str) != Some("wifi") {
            return Err("Select Wi-Fi remote control mode before taking ownership.".into());
        }
        let rate = status.pointer("/network/input_hz").and_then(Value::as_u64)
            .unwrap_or(60).clamp(10, 60) as u32;
        let url = base.replacen("http://", "ws://", 1) + "/ws";
        let (mut websocket, _) = connect(url.as_str())
            .map_err(|e| format!("Could not open the controller WebSocket: {e}"))?;
        if let MaybeTlsStream::Plain(stream) = websocket.get_mut() {
            let _ = stream.set_read_timeout(Some(Duration::from_millis(2)));
            let _ = stream.set_write_timeout(Some(Duration::from_millis(100)));
        }
        // Only a definite 503 service_busy is safe to retry; never repeat a
        // timed-out acquire because a lease could have been granted.
        let mut granted = None;
        for attempt in 0..=8 {
            match network::request(base, "/api/control/acquire", Some(&json!({"client":"windows"}))) {
                Ok(reply) => { granted = reply.get("token").and_then(Value::as_u64); break; }
                Err(err) if err.status == 503 && err.code == "service_busy" && attempt < 8 => {
                    thread::sleep(Duration::from_millis(100 * (attempt + 1).min(4)));
                }
                Err(err) => return Err(err.detail),
            }
        }
        let token = granted.ok_or("Adapter did not return an ownership token")?;
        if token == 0 || token > u32::MAX as u64 { return Err("Invalid ownership token".into()); }
        self.needs_neutral.store(true, Ordering::SeqCst);
        *self.keyboard.lock().map_err(|_| "Input lock failed")? = Pad::default();
        let session = Arc::new(Session::new(rate, token as u32));
        *guard = Some(Arc::clone(&session));
        let manager = Arc::clone(self);
        let worker_base = base.to_string();
        let worker_session = Arc::clone(&session);
        match thread::Builder::new().name("ogx-w-input".into()).spawn(move || {
            run_session(&manager, &worker_session, &worker_base, &mut websocket);
        }) {
            Ok(_) => Ok(()),
            Err(err) => {
                session.wanted.store(false, Ordering::SeqCst);
                session.alive.store(false, Ordering::SeqCst);
                session.token.store(0, Ordering::SeqCst);
                *guard = None;
                let _ = network::request(base, "/api/control/release", Some(&json!({"token":token})));
                Err(format!("Cannot start the input worker: {err}"))
            }
        }
    }
    fn sample(&self) -> Pad {
        if !self.focused.load(Ordering::SeqCst) { return Pad::default(); }
        let input = if self.source.load(Ordering::SeqCst) == 0 {
            self.keyboard.lock().map(|p| *p).unwrap_or_default()
        } else {
            xinput::read(self.slot.load(Ordering::SeqCst)).unwrap_or_default()
        };
        if self.needs_neutral.load(Ordering::SeqCst) {
            if input.neutral() { self.needs_neutral.store(false, Ordering::SeqCst); }
            return Pad::default();
        }
        input
    }
}

fn run_session(manager: &ControlManager, session: &Session, base: &str,
               ws: &mut WebSocket<MaybeTlsStream<TcpStream>>) {
    let token = session.token.load(Ordering::SeqCst);
    let interval = Duration::from_micros(1_000_000u64 / u64::from(session.rate_hz));
    let mut seq = 0u32;
    let mut next_frame = Instant::now();
    let mut last_ok = Instant::now();
    let mut last_heartbeat = Instant::now();
    let mut error: Option<String> = None;
    while session.wanted.load(Ordering::SeqCst) {
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
        last_ok = Instant::now();
        if last_heartbeat.elapsed() >= Duration::from_secs(1) {
            // Input messages maintain the owner lease. A separate heartbeat is
            // intentionally unnecessary while fresh samples are flowing.
            last_heartbeat = Instant::now();
        }
        match ws.read() {
            Ok(Message::Close(_)) => { error = Some("Adapter closed the control stream.".into()); break; }
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
        // A very long system suspension must never replay old input.
        if last_ok.elapsed() > Duration::from_secs(2) { manager.needs_neutral.store(true, Ordering::SeqCst); }
    }
    // Send a neutral final state when possible; then relinquish only this token.
    let neutral = json!({"type":"input","token":token,"seq":seq,
        "buttons":0,"dpad":0,"lx":0,"ly":0,"rx":0,"ry":0,"lt":0,"rt":0});
    let _ = ws.send(Message::Text(neutral.to_string().into()));
    let _ = ws.flush();
    let _ = ws.close(None);
    let _ = network::request(base, "/api/control/release", Some(&json!({"token":token})));
    if let Ok(mut last) = session.last_sent.lock() { *last = Pad::default(); }
    if let Ok(mut slot) = session.error.lock() { *slot = error; }
    session.token.store(0, Ordering::SeqCst);
    session.alive.store(false, Ordering::SeqCst);
    manager.needs_neutral.store(true, Ordering::SeqCst);
}

mod xinput {
    use super::Pad;
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
            let mut buttons = 0u16;
            for (xinput, ogx) in [
                (0x1000, 1), (0x2000, 2), (0x4000, 4), (0x8000, 8),
                (0x0040, 16), (0x0080, 32), (0x0020, 64), (0x0010, 128),
                (0x0100, 256), (0x0200, 512),
            ] { if g.wButtons & xinput != 0 { buttons |= ogx; } }
            let mut dpad = 0;
            for (xinput, ogx) in [(0x0001,1),(0x0002,2),(0x0004,4),(0x0008,8)] {
                if g.wButtons & xinput != 0 { dpad |= ogx; }
            }
            Some(Pad { buttons, dpad, lx:g.sThumbLX, ly:g.sThumbLY, rx:g.sThumbRX,
                ry:g.sThumbRY, lt:g.bLeftTrigger, rt:g.bRightTrigger })
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
}
