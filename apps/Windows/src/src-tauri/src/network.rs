//! The device's existing HTTP and UDP wire protocol. No firmware changes required.
use std::{io::Read, net::{UdpSocket, SocketAddr}, thread, time::{Duration, Instant}};
use serde::Serialize;
use serde_json::Value;
use url::Url;

const DISCOVERY_PORT: u16 = 38473;
const PROBE: &[u8] = b"DC-OGX-W|DISCOVER|1";
const PREFIX: &str = "DC-OGX-W|1|";

#[derive(Clone, Debug, Serialize)]
pub struct FoundDevice {
    pub id: String,
    pub board: String,
    pub ip: String,
    pub http_port: u16,
}

#[derive(Debug)]
pub struct RequestError {
    pub status: u16,
    pub code: String,
    pub detail: String,
}

impl RequestError {
    fn network(detail: impl ToString) -> Self {
        Self { status: 0, code: "network_error".into(), detail: detail.to_string() }
    }
}

pub fn normalize_address(input: &str) -> Result<String, String> {
    let text = input.trim();
    if text.is_empty() || text.len() > 255 { return Err("Enter a device IP address or hostname.".into()); }
    let candidate = if text.contains("://") { text.to_owned() } else { format!("http://{text}") };
    let url = Url::parse(&candidate).map_err(|_| "Enter an IP or hostname with an optional port".to_string())?;
    if url.scheme() != "http" || url.host_str().is_none() ||
       !url.username().is_empty() || url.password().is_some() ||
       !matches!(url.path(), "" | "/") || url.query().is_some() || url.fragment().is_some() {
        return Err("Only plain HTTP device addresses are supported, without paths or credentials.".into());
    }
    let host = url.host_str().unwrap();
    // An untrusted address must not be able to escape into an arbitrary URL path.
    if host.len() > 253 { return Err("Hostname is too long".into()); }
    Ok(url.as_str().trim_end_matches('/').to_owned())
}

pub fn allowed_route(path: &str, post: bool) -> bool {
    if post {
        matches!(path,
            "/api/control/mode" | "/api/profile/select" |
            "/api/oled/apply" | "/api/oled/save" |
            "/api/led/apply" | "/api/led/save" | "/api/led/test" |
            "/api/network/config" | "/api/network/retry" | "/api/network/mdns/retry")
    } else {
        matches!(path,
            "/api/status" | "/api/control/mode" | "/api/profile" |
            "/api/profiles" | "/api/network" | "/api/network/config" |
            "/api/led" | "/api/oled" | "/api/update")
    }
}

// Only an explicit response that proves the operation was rejected before any
// side effect may be replayed. A timeout, EOF, or 500 is NEVER safe to retry.
pub fn request(base: &str, path: &str, payload: Option<&Value>) -> Result<Value, RequestError> {
    for attempt in 0..4 {
        match request_once(base, path, payload) {
            Err(err) if err.status == 503 &&
                (err.code == "service_busy" || err.code == "led_busy") && attempt < 3 => {
                thread::sleep(Duration::from_millis(60 * (attempt + 1)));
            },
            reply => return reply,
        }
    }
    unreachable!("bounded HTTP retry loop always returns")
}

fn request_once(base: &str, path: &str, payload: Option<&Value>) -> Result<Value, RequestError> {
    if !allowed_route(path, payload.is_some()) && path != "/api/control/acquire" && path != "/api/control/release" {
        return Err(RequestError { status: 400, code: "unsupported_route".into(), detail: "Unsupported device route".into() });
    }
    let agent = ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(2))
        .timeout_read(Duration::from_secs(5))
        .timeout_write(Duration::from_secs(3))
        .build();
    let target = format!("{base}{path}");
    let result = match payload {
        Some(data) => agent.post(&target).set("Content-Type", "application/json").send_json(data),
        None => agent.get(&target).call(),
    };
    let (status, response) = match result {
        Ok(response) => (response.status(), response),
        Err(ureq::Error::Status(status, response)) => (status, response),
        Err(err) => return Err(RequestError::network(format!("Device connection failed: {err}"))),
    };
    let mut bytes = Vec::new();
    response.into_reader().take(64 * 1024).read_to_end(&mut bytes)
        .map_err(RequestError::network)?;
    let json: Value = serde_json::from_slice(&bytes)
        .map_err(|_| RequestError::network("Adapter returned invalid JSON"))?;
    if status >= 400 || json.get("ok").and_then(Value::as_bool) == Some(false) {
        let code = json.get("error").and_then(Value::as_str).unwrap_or("request_failed").to_owned();
        return Err(RequestError { status, detail: format!("{code} (HTTP {status})"), code });
    }
    Ok(json)
}

pub fn parse_announcement(payload: &str) -> Option<FoundDevice> {
    let text = payload.trim_end_matches(|c| c == '\r' || c == '\n');
    let fields: Vec<_> = text.split('|').collect();
    if fields.len() != 6 || fields[0] != "DC-OGX-W" || fields[1] != "1" { return None; }
    let mut id = None;
    let mut board = None;
    let mut ip = None;
    let mut port = None;
    for f in &fields[2..] {
        let (key, value) = f.split_once('=')?;
        match key {
            "id" => id = Some(value.to_string()),
            "board" => board = Some(value.to_string()),
            "ip" => ip = Some(value.to_string()),
            "http" => port = value.parse::<u16>().ok(),
            _ => return None,
        }
    }
    let result = FoundDevice { id: id?, board: board?, ip: ip?, http_port: port? };
    if result.id.is_empty() || result.id.len() > 48 || result.board.is_empty() ||
        result.http_port == 0 || result.ip.parse::<std::net::Ipv4Addr>().is_err() { return None; }
    Some(result)
}

// Accept both immediate unicast replies (on the probe socket's ephemeral
// port) and periodic UDP beacons (on the well-known discovery port).
fn consider_announcement(found: &mut Vec<FoundDevice>, packet: &[u8], source: SocketAddr) {
    if let Ok(text) = std::str::from_utf8(packet) {
        if !text.starts_with(PREFIX) { return; }
        if let Some(dev) = parse_announcement(text) {
            if source.ip().to_string() == dev.ip &&
                !found.iter().any(|other| other.id == dev.id) {
                found.push(dev);
            }
        }
    }
}

pub fn discover() -> Result<Vec<FoundDevice>, String> {
    let probes = UdpSocket::bind("0.0.0.0:0")
        .map_err(|e| format!("UDP discovery unavailable: {e}"))?;
    probes.set_broadcast(true).map_err(|e| e.to_string())?;
    probes.set_read_timeout(Some(Duration::from_millis(160)))
        .map_err(|e| e.to_string())?;

    // Older ESP-AT firmware can broadcast but cannot report the sender IP
    // and port for a directed reply. Its beacon is addressed to port 38473,
    // not to the ephemeral probe socket above. Binding this receiver is the
    // essential compatibility path. If another application owns the port,
    // still support directed replies instead of aborting discovery.
    let beacons = UdpSocket::bind(("0.0.0.0", DISCOVERY_PORT)).ok();
    if let Some(ref receiver) = beacons {
        receiver.set_nonblocking(true).map_err(|e| e.to_string())?;
    }

    let start = Instant::now();
    let mut next_probe = start;
    let mut found = Vec::new();
    let mut buf = [0u8; 256];
    // A normal probe-reply scan finishes quickly. If there is no reply,
    // listen for slightly longer than one five-second beacon period.
    while start.elapsed() < Duration::from_millis(6200) {
        if !found.is_empty() && start.elapsed() >= Duration::from_millis(1900) { break; }
        if Instant::now() >= next_probe {
            probes.send_to(PROBE, ("255.255.255.255", DISCOVERY_PORT))
                .map_err(|e| format!("Unable to broadcast discovery: {e}"))?;
            next_probe = Instant::now() + Duration::from_millis(450);
        }
        if let Ok((len, source)) = probes.recv_from(&mut buf) {
            consider_announcement(&mut found, &buf[..len], source);
        }
        if let Some(ref receiver) = beacons {
            loop {
                match receiver.recv_from(&mut buf) {
                    Ok((len, source)) => consider_announcement(&mut found, &buf[..len], source),
                    Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                    Err(_) => break,
                }
            }
        }
    }
    Ok(found)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn discovery_roundtrip() {
        let payload = "DC-OGX-W|1|id=ABCD|board=pico_w|ip=192.168.1.12|http=80\n";
        let parsed = parse_announcement(payload).unwrap();
        assert_eq!(parsed.ip, "192.168.1.12");
        assert!(parse_announcement("anything else").is_none());
    }
    #[test]
    fn discovery_unicast_and_beacon_parser() {
        use std::net::{IpAddr, Ipv4Addr, SocketAddr};
        let source = SocketAddr::new(IpAddr::V4(Ipv4Addr::new(192,168,1,73)), 38473);
        let packet = b"DC-OGX-W|1|id=E0123456789ABCDF|board=pico_w_2023|ip=192.168.1.73|http=80\n";
        let mut found = Vec::new();
        consider_announcement(&mut found, packet, source);
        consider_announcement(&mut found, packet, source); // duplicate beacon
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].board, "pico_w_2023");
        let wrong_source = SocketAddr::new(IpAddr::V4(Ipv4Addr::new(192,168,1,74)), 38473);
        consider_announcement(&mut found, packet, wrong_source);
        assert_eq!(found.len(), 1);
    }
    #[test]
    fn address_and_routes() {
        assert_eq!(normalize_address("192.168.4.1").unwrap(), "http://192.168.4.1");
        assert!(normalize_address("https://example.com").is_err());
        assert!(normalize_address("http://192.168.4.1/evil").is_err());
        assert!(allowed_route("/api/oled/apply", true));
        assert!(!allowed_route("/api/oled/apply", false));
        assert!(!allowed_route("/api/control/acquire", true));
    }
}
