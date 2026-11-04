"use strict";
// Standalone, bundled desktop UI. Only Rust performs device/network I/O.
const $ = id => document.getElementById(id);
const invoke = (name, args={}) => {
  if (!window.__TAURI__?.core?.invoke) return Promise.reject(new Error("Launch this interface through the Tauri Windows application."));
  return window.__TAURI__.core.invoke(name, args);
};
const THEMES = ["darkone","theia-dark","midnight","synthwave","ember","terminal","theia-light","arctic-light"];
const DEFAULT_KEYS = {
  a:"Space", b:"KeyE", x:"KeyQ", y:"KeyR", sys:null, l3:"KeyF", r3:"KeyG",
  back:"Backspace",start:"Enter",lb:"KeyZ",rb:"KeyC",lt:"Digit1",rt:"Digit2",
  up:"ArrowUp",down:"ArrowDown",left:"ArrowLeft",right:"ArrowRight",
  lx_pos:"KeyD",lx_neg:"KeyA",ly_pos:"KeyW",ly_neg:"KeyS",
  rx_pos:"KeyL",rx_neg:"KeyJ",ry_pos:"KeyI",ry_neg:"KeyK"
};
const KEY_GROUPS = [
  ["FACE / MENU",[["a","A"],["b","B"],["x","X"],["y","Y"],["start","Start"],["back","Back"],["sys","Guide"]]],
  ["SHOULDERS / OG XBOX",[["lb","White / LB"],["rb","Black / RB"],["lt","LT"],["rt","RT"],["l3","L3"],["r3","R3"]]],
  ["D-PAD",[["up","Up"],["down","Down"],["left","Left"],["right","Right"]]],
  ["LEFT STICK",[["ly_pos","Up"],["ly_neg","Down"],["lx_neg","Left"],["lx_pos","Right"]]],
  ["RIGHT STICK",[["ry_pos","Up"],["ry_neg","Down"],["rx_neg","Left"],["rx_pos","Right"]]]
];
const BUTTONS = {a:1,b:2,x:4,y:8,l3:16,r3:32,back:64,start:128,lb:256,rb:512,sys:1024,misc:2048};
const DPAD = {up:1,down:2,left:4,right:8};
const NEUTRAL = () => ({buttons:0,dpad:0,lx:0,ly:0,rx:0,ry:0,lt:0,rt:0});
let prefs = {theme:"darkone",lastHost:"",keyBindings:{...DEFAULT_KEYS},source:"keyboard",slot:0};
let keyBindings = {...DEFAULT_KEYS}, listening = null, pressed = new Set(), online = false,
  currentPage = "control", lastStatus = null, lastProfile = null,
  oled = null, led = null, wifi = null, currentControl = {active:false},
  toastTimer, pollBusy = false, connectionFailures = 0, actionBusy = false;
// On-screen controls are merged into the same protocol bits used by keyboard and XInput.
// Original Xbox White = LB/0x0100; Black = RB/0x0200. No new firmware bits.
const onscreenHeld = new Set();
const onscreenAxes = {lx:0,ly:0,rx:0,ry:0};
const onscreenTriggers = {lt:0,rt:0};
const val = (x,fallback="—") => x === null || x === undefined || x === "" ? fallback : String(x);
function notify(text,error=false) {
  const toast = $("toast"); toast.textContent = text; toast.className = "toast visible" + (error?" error":"");
  $("footer-message").textContent = text;
  clearTimeout(toastTimer);toastTimer = setTimeout(()=>toast.classList.remove("visible"),5200);
}
function inform(error) { notify(typeof error === "string" ? error : error?.message || String(error),true); }
function setText(id,value) { $(id).textContent = val(value); }
function setConnection(connected) {
  online = connected;
  $("sidebar-connection").textContent = connected ? "● Adapter connected" : "● Not connected";
  $("sidebar-connection").classList.toggle("online",connected);
  $("top-status").textContent = connected ? "● Connected" : "● Offline";
  $("top-status").classList.toggle("online",connected);
  $("connect-button").disabled = connected || actionBusy;
  $("disconnect-button").disabled = !connected || actionBusy;
  $("take-button").disabled = !connected || actionBusy;
  $("footer-connection").textContent = connected ? $("device-address").value : "DC-OGX-W · PORTABLE";
  for (const id of ["oled-apply","oled-save"]) $(id).disabled = !connected || !oled || actionBusy;
  for (const id of ["led-apply","led-save"]) $(id).disabled = !connected || !led || actionBusy;
  for (const id of ["wifi-save","wifi-forget","wifi-retry"]) $(id).disabled = !connected || !wifi || actionBusy;
  updateInteractiveAvailability();
}
function formatFactList(target,items) {
  const dl=$(target); dl.replaceChildren();
  for (const [name,value] of items) {
    const row=document.createElement("div"),dt=document.createElement("dt"),dd=document.createElement("dd");
    dt.textContent=name;dd.textContent=val(value);row.append(dt,dd);dl.append(row);
  }
}
function updateStatus(s) {
  lastStatus=s;
  const n=s.network||{},c=s.control||{};
  setText("stat-board",val(n.board,s.board));setText("stat-board-sub",val(n.transport,"Board transport"));
  setText("stat-firmware",s.firmware);setText("stat-profile",s.applied_profile_id||s.profile_id);
  setText("stat-input",c.active?"In use":n.control_ready===false?"Starting":"Ready");
  setText("stat-input-sub",c.active?"Remote session active":"Controller available");
  formatFactList("health-facts",[
    ["IP address",n.ip],["Network mode",n.station_connected?"Station / LAN":n.ap_active?"Recovery AP":"Offline"],
    ["Radio",n.transport],["USB output",n.usb_configured?"Connected":"Not configured"],
    ["Bluetooth",c.bluetooth_supported?(c.bluetooth_connected?"Connected":"Available"):"Not supported"],
    ["Input mode",c.mode],["Update / maintenance",c.maintenance?"Busy":"Idle"]
  ]);
  formatFactList("controller-facts",[
    ["Board",n.board||s.board],["Output",n.usb_configured?"Connected":"Not configured"],
    ["Profile",s.applied_profile_id||s.profile_id],["Radio",n.station_connected?"Station / LAN":n.ap_active?"Recovery AP":"Offline"]
  ]);
  formatFactList("network-facts",[
    ["Connection",n.station_connected?"Home Wi-Fi":n.ap_active?"Recovery AP":"Disconnected"],
    ["Network name",n.ssid],["Address",n.ip],["Station IP",n.station_ip],["Hostname",n.hostname],
    ["AP active",n.ap_active?"Yes":"No"],["Reconnects",n.station_reconnects],
    ["Modem timeouts",n.command_timeouts],["Radio error",n.station_error||n.error||"None"]
  ]);
  for(const button of document.querySelectorAll("[data-mode]")) button.classList.toggle("selected",button.dataset.mode===c.mode);
  setText("mode-status",c.bluetooth_supported?`Current: ${c.mode||"?"}${c.mode_pending?" (transition pending)":""}`:
    "Wi-Fi-only board: Bluetooth controller and Configuration modes are unavailable.");
  for(const button of document.querySelectorAll("[data-mode]")) button.disabled = !online || (c.bluetooth_supported===false && button.dataset.mode!=="wifi");
}
async function runBusy(fn) {
  if (actionBusy) return;
  actionBusy=true;setConnection(online);
  try { await fn(); } catch(e) { inform(e); }
  finally {actionBusy=false;setConnection(online);}
}
async function connect() {
  if(!$("device-address").value.trim()) {notify("Enter an adapter address first.",true);return;}
  await runBusy(async()=>{
    await releaseControl(false);
    const status = await invoke("connect_device",{address:$("device-address").value.trim()});
    setConnection(true); updateStatus(status); prefs.lastHost=$("device-address").value.trim(); await savePrefs();
    notify("Connected to " + val(status.board,"DC-OGX-W"));
    if(currentPage!=="overview") await loadPage(currentPage);
  });
}
async function disconnect() {
  await runBusy(async()=>{
    await invoke("disconnect_device");setConnection(false);lastStatus=null;oled=led=wifi=null;
    setText("stat-board","—");setText("stat-firmware","—");setText("stat-profile","—");setText("stat-input","Offline");
    notify("Disconnected. The previous controller session was released.");
  });
}
async function discover() {
  $("discover-button").disabled=true;
  setText("footer-message","Looking for devices on your local network…");
  try {
    const list=await invoke("discover_devices");
    const target=$("discovered-list");target.replaceChildren();
    for (const item of list) {
      const button=document.createElement("button"),name=document.createElement("strong"),details=document.createElement("small");
      name.textContent=item.board;details.textContent=`${item.ip} · ${item.id.slice(-8)}`;
      button.append(name,details);button.addEventListener("click",()=>{$("device-address").value=`${item.ip}:${item.http_port}`;if(!online)void connect();});
      target.append(button);
    }
    notify(list.length?`Found ${list.length} adapter${list.length===1?"":"s"}.`:
      "No discovery reply yet. Check the same Wi-Fi/LAN, firmware version and Windows firewall.",!list.length);
  }catch(e){inform(e);}finally{$("discover-button").disabled=false;}
}
async function loadPage(page) {
  if(!online)return;
  if(page==="profiles") await loadProfiles();
  else if(page==="led") await loadLed();
  else if(page==="device") await loadOled();
  else if(page==="network") {await Promise.all([loadWifi(),refreshStatus()]);}
  else if(page==="overview") await refreshStatus();
}
async function showPage(page) {
  if(page===currentPage)return;
  if(currentPage==="control") await releaseControl(false);
  resetKeyboard();currentPage=page;
  document.querySelectorAll(".page").forEach(x=>x.classList.toggle("active",x.id===`page-${page}`));
  document.querySelectorAll(".nav-item").forEach(x=>{
    const selected=x.dataset.page===page;
    x.classList.toggle("selected",selected);
    x.setAttribute("aria-selected",String(selected));
  });
  setText("page-title",document.querySelector(`.nav-item[data-page="${page}"]`)?.textContent.trim()||page);
  await loadPage(page);
}
async function refreshStatus() {
  if(!online||pollBusy)return;
  pollBusy=true;
  try {const status=await invoke("device_api",{path:"/api/status",body:null});updateStatus(status);connectionFailures=0;}
  catch(e){connectionFailures++;if(connectionFailures>=2) notify("Status telemetry is delayed. Check the adapter connection.",true);}
  finally{pollBusy=false;}
}

// Same WebUI button presentation: every visible control is an actionable input.
function showPad(p) {
  for (const button of document.querySelectorAll("#pad-zone [data-action]")) {
    const action=button.dataset.action;
    const bit=BUTTONS[action]||0;
    const d=DPAD[action]||0;
    button.classList.toggle("pressed",Boolean(bit ? (p.buttons & bit) : d && (p.dpad & d)));
    button.setAttribute("aria-pressed",String(Boolean(bit ? (p.buttons & bit) : d && (p.dpad & d))));
  }
  setText("left-axis",`${p.lx} / ${p.ly}`);
  setText("right-axis",`${p.rx} / ${p.ry}`);
  setText("trigger-axis",`${p.lt} / ${p.rt}`);
  setText("pad-owner",currentControl.active?"Owned":"Released");
  // Use real outgoing values; don't overwrite a knob while it is being dragged.
  for (const [id,x,y] of [["l",p.lx,p.ly],["r",p.rx,p.ry]]) {
    if (!$("stick-"+id).classList.contains("knob-active"))
      $("knob-"+id).style.transform=`translate(${Math.round(x / 32767 * 34)}px, ${Math.round(-y / 32767 * 34)}px)`;
  }
  if(document.activeElement!==$("lt")) $("lt").value=String(p.lt);
  if(document.activeElement!==$("rt")) $("rt").value=String(p.rt);
}
function canUseOnscreen() {
  return online && currentControl.active && $("control-source").value==="keyboard" && currentPage==="control";
}
function updateInteractiveAvailability() {
  const available=canUseOnscreen();
  for(const button of document.querySelectorAll("#pad-zone button.onscreen")) button.disabled=!available;
  for(const id of ["lt","rt"]) $(id).disabled=!available;
  $("pad-zone").classList.toggle("input-disabled",!available);
}
function resetOnscreen() {
  onscreenHeld.clear();
  for (const key of Object.keys(onscreenAxes)) onscreenAxes[key]=0;
  onscreenTriggers.lt=onscreenTriggers.rt=0;
  for(const id of ["lt","rt"]) $(id).value="0";
  for(const id of ["l","r"]) {
    $("stick-"+id).classList.remove("knob-active");
    $("knob-"+id).style.transform="translate(0px,0px)";
  }
}
function wireOnscreenControls() {
  for(const button of document.querySelectorAll("#pad-zone button.onscreen")) {
    const action=button.dataset.action;
    const release=()=>{
      if(onscreenHeld.delete(action))pushKeyboard();
      button.classList.remove("pressed");
      if(document.activeElement===button)button.blur();
    };
    button.addEventListener("pointerdown",event=>{
      if(!canUseOnscreen())return;
      if(event.button!==0 && event.pointerType==="mouse")return;
      event.preventDefault();
      button.setPointerCapture(event.pointerId);
      onscreenHeld.add(action);
      button.classList.add("pressed");
      pushKeyboard();
    });
    button.addEventListener("pointerup",release);
    button.addEventListener("pointercancel",release);
    button.addEventListener("lostpointercapture",release);
  }
  for (const id of ["lt","rt"]) {
    const range=$(id);
    range.addEventListener("input",()=>{
      if(!canUseOnscreen())return;
      onscreenTriggers[id]=Number(range.value);
      pushKeyboard();
    });
    const reset=()=>{
      if(!onscreenTriggers[id])return;
      onscreenTriggers[id]=0;range.value="0";pushKeyboard();
    };
    range.addEventListener("pointerup",reset);
    range.addEventListener("pointercancel",reset);
    range.addEventListener("keyup",reset);
    range.addEventListener("blur",reset);
  }
  for(const side of ["l","r"]) {
    const stick=$("stick-"+side),knob=$("knob-"+side);
    const update=event=>{
      const rect=stick.getBoundingClientRect();
      let dx=(event.clientX-rect.left-rect.width/2)/(rect.width*.30);
      let dy=(event.clientY-rect.top-rect.height/2)/(rect.height*.30);
      const size=Math.hypot(dx,dy);if(size>1){dx/=size;dy/=size;}
      onscreenAxes[side+"x"]=Math.round(dx*32767);
      onscreenAxes[side+"y"]=Math.round(-dy*32767);
      knob.style.transform=`translate(${Math.round(dx*34)}px,${Math.round(dy*34)}px)`;
      pushKeyboard();
    };
    stick.addEventListener("pointerdown",event=>{
      if(!canUseOnscreen())return;
      if(event.button!==0 && event.pointerType==="mouse")return;
      event.preventDefault();stick.setPointerCapture(event.pointerId);
      stick.classList.add("knob-active");update(event);
    });
    stick.addEventListener("pointermove",event=>{
      if(stick.hasPointerCapture(event.pointerId)&&canUseOnscreen())update(event);
    });
    const reset=()=>{
      if(!stick.classList.contains("knob-active"))return;
      stick.classList.remove("knob-active");
      onscreenAxes[side+"x"]=onscreenAxes[side+"y"]=0;
      knob.style.transform="translate(0px,0px)";pushKeyboard();
    };
    stick.addEventListener("pointerup",reset);
    stick.addEventListener("pointercancel",reset);
    stick.addEventListener("lostpointercapture",reset);
  }
}
async function takeControl() {
  if(!online||actionBusy)return;
  try {
    resetKeyboard();await invoke("set_input_source",{source:$("control-source").value,slot:Number($("gamepad-slot").value)});
    await invoke("take_control");
    if(document.activeElement instanceof HTMLButtonElement) document.activeElement.blur();
    notify("Controller session acquired. Press Esc to release.");
  } catch(e) { inform(e); }
  await refreshControl();
}
async function releaseControl(show=true) {
  resetKeyboard();
  try {await invoke("release_control");if(show)notify("Controller released; output neutralized.");}
  catch(e){if(show)inform(e);}
  await refreshControl();
}
async function refreshControl() {
  try {
    const data=await invoke("control_status");currentControl=data;
    const active=data.active===true;
    $("take-button").disabled=!online||active||actionBusy;
    $("release-button").disabled=!active;
    setText("control-tag",active?"● Owned":data.token_held?"Releasing":"Released");
    showPad(data.last_sent||NEUTRAL());
    updateInteractiveAvailability();
    $("control-message").textContent=active
      ? `Controller active · ${data.source==="gamepad"?"XInput":"Keyboard + on-screen"}. F8 or Esc releases.`
      : online?"Connected. Take control to use keyboard, on-screen buttons or XInput.":"Connect to an adapter to take control.";
    if(data.error&&!active&&data.error!==refreshControl.previousError) notify(data.error,true);
    refreshControl.previousError=data.error;
  } catch(e){ /* App may be closing. */ }
}
function resetKeyboard(){
  pressed.clear();resetOnscreen();
  if(currentControl.source==="keyboard")void invoke("set_keyboard_input",{input:NEUTRAL()}).catch(()=>{});
}
function buildKeyboardState() {
  const has=action=>onscreenHeld.has(action)||Boolean(keyBindings[action]&&pressed.has(keyBindings[action]));
  let buttons=0,dpad=0;
  for(const [action,bit] of Object.entries(BUTTONS))if(has(action))buttons|=bit;
  for(const [action,bit] of Object.entries(DPAD))if(has(action))dpad|=bit;
  if((dpad&3)===3)dpad&=~3;if((dpad&12)===12)dpad&=~12;
  const axis=(p,n)=>has(p)===has(n)?0:has(p)?32767:-32768;
  return {buttons,dpad,
    lx:onscreenAxes.lx||axis("lx_pos","lx_neg"), ly:onscreenAxes.ly||axis("ly_pos","ly_neg"),
    rx:onscreenAxes.rx||axis("rx_pos","rx_neg"), ry:onscreenAxes.ry||axis("ry_pos","ry_neg"),
    lt:Math.max(onscreenTriggers.lt,has("lt")?255:0),rt:Math.max(onscreenTriggers.rt,has("rt")?255:0)};
}
let inputChain=Promise.resolve();
function pushKeyboard() {
  if(!currentControl.active||$("control-source").value!=="keyboard")return;
  const snapshot=buildKeyboardState();
  inputChain=inputChain.then(()=>invoke("set_keyboard_input",{input:snapshot})).catch(()=>{});
}
function keyLabel(code){return code==null?"Unassigned":code.startsWith("Key")?code.slice(3):code.startsWith("Digit")?code.slice(5):code;}
function validKey(code){return /^(Key[A-Z]|Digit[0-9]|Numpad[0-9]|Arrow(Up|Down|Left|Right)|Space|Enter|Backspace|Minus|Equal|BracketLeft|BracketRight|Semicolon|Quote|Comma|Period|Slash|Backslash|Backquote)$/.test(code);}
function renderKeymap() {
  const list=$("keymap");list.replaceChildren();
  for(const [title,entries] of KEY_GROUPS){
    const group=document.createElement("div"),name=document.createElement("strong"),grid=document.createElement("div");
    group.className="key-group";grid.className="key-items";name.textContent=title;
    for(const [action,label] of entries){
      const row=document.createElement("div"),text=document.createElement("span"),button=document.createElement("button");
      row.className="key-row";text.textContent=label;
      button.textContent=listening===action?"Press key…":keyLabel(keyBindings[action]);
      button.classList.toggle("listening",listening===action);
      button.addEventListener("click",async()=>{await releaseControl(false);resetKeyboard();listening=action;renderKeymap();});
      row.append(text,button);grid.append(row);
    }
    group.append(name,grid);list.append(group);
  }
}
function keyDown(ev) {
  if(listening!==null){
    ev.preventDefault();const action=listening;listening=null;
    if(ev.code==="Escape"){renderKeymap();return;}
    if(!validKey(ev.code)){notify("Choose a normal key, number, or arrow key.",true);renderKeymap();return;}
    const existing=Object.keys(keyBindings).find(x=>x!==action&&keyBindings[x]===ev.code);
    if(existing)keyBindings[existing]=keyBindings[action];
    keyBindings[action]=ev.code;prefs.keyBindings={...keyBindings};renderKeymap();void savePrefs();return;
  }
  if(ev.code==="Escape"&&currentControl.active){ev.preventDefault();void releaseControl();return;}
  if(ev.code==="F8"&&!ev.repeat&&currentPage==="control"){
    ev.preventDefault();if(currentControl.active)void releaseControl();else void takeControl();return;
  }
  if(currentPage!=="control"||$("control-source").value!=="keyboard"||!currentControl.active)return;
  if(ev.target?.closest?.("input,select,textarea,button"))return;
  if(!Object.values(keyBindings).includes(ev.code))return;
  ev.preventDefault();if(pressed.has(ev.code))return;pressed.add(ev.code);pushKeyboard();
}
function keyUp(ev){if(pressed.delete(ev.code)){ev.preventDefault();pushKeyboard();}}

async function loadProfiles() {
  try {
    const data=await invoke("device_api",{path:"/api/profiles",body:null});
    const list=$("profile-list");list.replaceChildren();
    for(const profile of data.profiles||[]){
      const button=document.createElement("button"),title=document.createElement("strong"),desc=document.createElement("span");
      button.className="profile-item"+(data.profile_id===profile.id?" active-profile":"");
      title.textContent=profile.name||`Profile ${profile.id}`;
      desc.textContent=data.applied_profile_id===profile.id?"Active":"Select →";
      button.append(title,desc);button.addEventListener("click",async()=>{
        if(lastStatus?.dirty&&!confirm("This will replace unsaved profile changes on the adapter. Continue?"))return;
        try {const result=await invoke("device_api",{path:"/api/profile/select",body:{id:profile.id}});
          lastProfile=result;notify(`Selected profile ${profile.id}.`);await loadProfiles();}
        catch(e){inform(e);}
      });list.append(button);
    }
  }catch(e){inform(e);}
}
async function changeMode(mode) {
  if(!online)return;
  if(lastStatus?.control?.bluetooth_supported===false&&mode!=="wifi")return;
  await runBusy(async()=>{await releaseControl(false);
    await invoke("device_api",{path:"/api/control/mode",body:{mode}});
    notify("Mode change requested. Wait for the adapter to finish transitioning.");
    await refreshStatus();
  });
}
function percent(raw){return Math.round(Number(raw)/255*100)+"%";}
function updateRangeLabels(){setText("oled-bright-label",percent($("oled-bright").value));setText("oled-idle-label",percent($("oled-idle").value));setText("led-bright-label",percent($("led-bright").value));}
async function loadOled() {
  if(!online)return;
  try{
    oled=await invoke("device_api",{path:"/api/oled",body:null});
    $("oled-enabled").checked=Boolean(oled.enabled);$("oled-bright").value=oled.brightness;
    $("oled-idle").value=oled.idle_brightness;$("oled-dim").value=oled.dim_after_s;
    $("oled-off").value=oled.off_after_s;$("oled-invert").checked=Boolean(oled.invert);
    $("oled-flip").checked=Boolean(oled.flip);$("oled-fade").checked=Boolean(oled.fade);
    $("oled-note").textContent=oled.available?"Apply is temporary. Save & restart writes persistent settings.":"OLED is not enabled in this firmware build.";
    for(const id of ["oled-apply","oled-save"])$(id).disabled=!oled.available;
    updateRangeLabels();
  }catch(e){oled=null;inform(e);}
}
function collectOled(){
  const dim=Number($("oled-dim").value),off=Number($("oled-off").value);
  if(!Number.isInteger(dim)||dim<0||dim>3600||!Number.isInteger(off)||off<0||off>3600)throw Error("OLED timeouts must be 0–3600 seconds.");
  return {enabled:$("oled-enabled").checked,brightness:Number($("oled-bright").value),
    idle_brightness:Number($("oled-idle").value),dim_after_s:dim,off_after_s:off,
    invert:$("oled-invert").checked,flip:$("oled-flip").checked,fade:$("oled-fade").checked};
}
async function sendConfig(kind,action,payload) {
  if(!online)return;
  if(action==="save"&&!confirm(`Save ${kind.toUpperCase()} settings to the adapter and restart?`))return;
  await runBusy(async()=>{
    await releaseControl(false);
    await invoke("device_api",{path:`/api/${kind}/${action}`,body:payload});
    notify(action==="save"?"Settings saved; adapter restarting. Reconnect after boot.":"Settings applied for this session.");
  });
}
function makeColorField(name,id,value){
  const row=document.createElement("label"),input=document.createElement("input"),span=document.createElement("span");
  row.className="color-field";input.type="color";input.value=/^#[0-9a-fA-F]{6}$/.test(value)?value:"#000000";
  input.id=id;span.textContent=name;row.append(input,span);return row;
}
async function loadLed() {
  if(!online)return;
  try{
    led=await invoke("device_api",{path:"/api/led",body:null});
    $("led-enabled").checked=Boolean(led.enabled);$("led-bright").value=led.brightness;
    $("led-mode").value=led.mode;
    const profiles=$("led-profile-colors"),diagnostics=$("led-diag-colors");profiles.replaceChildren();diagnostics.replaceChildren();
    (led.profile_colors||[]).forEach((color,i)=>profiles.append(makeColorField(`Profile ${i+1}`,`led-profile-${i}`,color)));
    for(const [name,color] of Object.entries(led.diagnostic_colors||{}))diagnostics.append(makeColorField(name,`led-diag-${name}`,color));
    $("led-note").textContent=led.available===false?"RGB LED unavailable in this build.":"Apply to preview. Save & restart to retain settings.";
    for(const id of ["led-apply","led-save"])$(id).disabled=led.available===false;
    updateRangeLabels();
  }catch(e){led=null;inform(e);}
}
function collectLed(){
  if(!led)throw Error("Load LED settings first.");
  return {enabled:$("led-enabled").checked,brightness:Number($("led-bright").value),mode:$("led-mode").value,
    profile_colors:(led.profile_colors||[]).map((_,i)=>$("led-profile-"+i).value.toUpperCase()),
    diagnostic_colors:Object.fromEntries(Object.keys(led.diagnostic_colors||{}).map(k=>[k,$("led-diag-"+k).value.toUpperCase()]))};
}
async function loadWifi() {
  if(!online)return;
  try{
    wifi=await invoke("device_api",{path:"/api/network/config",body:null});
    $("wifi-enabled").checked=Boolean(wifi.station_enabled||!wifi.station_ssid);
    $("wifi-ssid").value=wifi.station_ssid||"";$("wifi-password").value="";
    $("wifi-hostname").value=wifi.hostname||"dc-ogx-w";
    $("wifi-open").checked=Boolean(wifi.station_enabled&&wifi.station_ssid&&!wifi.password_saved);
    $("wifi-note").textContent="Changes restart the adapter. After a successful station join, the recovery AP shuts down.";
    for(const id of ["wifi-save","wifi-forget","wifi-retry"])$(id).disabled=false;
  }catch(e){wifi=null;inform(e);}
}
async function saveWifi(forget=false){
  if(!online||!wifi)return;
  const body=forget?{forget:true}:{station_enabled:$("wifi-enabled").checked,
    station_ssid:$("wifi-ssid").value,hostname:$("wifi-hostname").value.trim().toLowerCase()};
  if(!forget){
    if(!/^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$/.test(body.hostname)){notify("Enter a valid local hostname.",true);return;}
    if(body.station_enabled&&!$("wifi-open").checked&&!$("wifi-password").value&&
       !(body.station_ssid===wifi.station_ssid&&wifi.password_saved)) {
      notify("Enter a Wi-Fi password or select an open network.",true);return;
    }
    if($("wifi-open").checked)body.station_password="";
    else if($("wifi-password").value)body.station_password=$("wifi-password").value;
  }
  if(!confirm(forget?"Forget the saved network and restart the adapter?":"Save Wi-Fi settings and restart the adapter?"))return;
  await runBusy(async()=>{await releaseControl(false);
    await invoke("device_api",{path:"/api/network/config",body});
    $("wifi-password").value="";notify("Network settings queued. Reconnect to the new address after restart.");
  });
}
async function savePrefs(){
  try{await invoke("save_preferences",{preferences:prefs});}
  catch(e){inform("Could not write portable preferences: "+e);}
}
async function loadPrefs(){
  try{
    const saved=await invoke("load_preferences");
    if(saved&&typeof saved==="object"){
      prefs={...prefs,...saved};
      if(!prefs.keyBindings||typeof prefs.keyBindings!=="object")prefs.keyBindings={...DEFAULT_KEYS};
      const values=Object.values(prefs.keyBindings).filter(Boolean);
      if(Object.keys(DEFAULT_KEYS).some(k=>!(k in prefs.keyBindings))||new Set(values).size!==values.length||
         values.some(v=>!validKey(v)))prefs.keyBindings={...DEFAULT_KEYS};
      keyBindings={...prefs.keyBindings};
    }
  }catch(e){inform("Preferences could not be loaded: "+e);}
  $("device-address").value=prefs.lastHost||"192.168.4.1";
  document.documentElement.dataset.theme=THEMES.includes(prefs.theme)?prefs.theme:"darkone";
  $("theme-select").value=document.documentElement.dataset.theme;
  $("control-source").value=prefs.source==="gamepad"?"gamepad":"keyboard";
  $("gamepad-slot").value=String([0,1,2,3].includes(Number(prefs.slot))?prefs.slot:0);
  renderKeymap();
}
function wire() {
  $("connect-button").addEventListener("click",()=>void connect());
  $("header-connect").addEventListener("click",()=>void showPage("overview"));
  $("device-address").addEventListener("keydown",e=>{if(e.code==="Enter"){e.preventDefault();void connect();}});
  $("disconnect-button").addEventListener("click",()=>void disconnect());
  $("discover-button").addEventListener("click",()=>void discover());
  document.querySelectorAll("[data-page]").forEach(b=>b.addEventListener("click",()=>void showPage(b.dataset.page)));
  document.querySelectorAll("[data-go]").forEach(b=>b.addEventListener("click",()=>void showPage(b.dataset.go)));
  $("theme-select").addEventListener("change",()=>{prefs.theme=$("theme-select").value;document.documentElement.dataset.theme=prefs.theme;void savePrefs();});
  wireOnscreenControls();
  $("take-button").addEventListener("click",()=>void takeControl());
  $("release-button").addEventListener("click",()=>void releaseControl());
  $("control-source").addEventListener("change",async()=>{resetKeyboard();prefs.source=$("control-source").value;await savePrefs();try{await invoke("set_input_source",{source:prefs.source,slot:Number($("gamepad-slot").value)});}catch(e){inform(e);}});
  $("gamepad-slot").addEventListener("change",async()=>{prefs.slot=Number($("gamepad-slot").value);await savePrefs();try{await invoke("set_input_source",{source:$("control-source").value,slot:prefs.slot});}catch(e){inform(e);}});
  $("reset-keys").addEventListener("click",()=>{if(confirm("Reset keyboard mapping to defaults?")){keyBindings={...DEFAULT_KEYS};prefs.keyBindings={...keyBindings};renderKeymap();void savePrefs();}});
  window.addEventListener("keydown",keyDown);window.addEventListener("keyup",keyUp);
  window.addEventListener("blur",()=>{resetKeyboard();void invoke("set_input_focus",{focused:false});});
  window.addEventListener("focus",()=>void invoke("set_input_focus",{focused:true}));
  document.addEventListener("visibilitychange",()=>{resetKeyboard();void invoke("set_input_focus",{focused:!document.hidden});});
  for(const b of document.querySelectorAll("[data-mode]"))b.addEventListener("click",()=>void changeMode(b.dataset.mode));
  $("reload-profiles").addEventListener("click",()=>void loadProfiles());
  for(const id of ["oled-bright","oled-idle","led-bright"])$(id).addEventListener("input",updateRangeLabels);
  $("reload-oled").addEventListener("click",()=>void loadOled());
  $("oled-apply").addEventListener("click",()=>{try{void sendConfig("oled","apply",collectOled());}catch(e){inform(e);}});
  $("oled-save").addEventListener("click",()=>{try{void sendConfig("oled","save",collectOled());}catch(e){inform(e);}});
  $("reload-led").addEventListener("click",()=>void loadLed());
  $("led-apply").addEventListener("click",()=>{try{void sendConfig("led","apply",collectLed());}catch(e){inform(e);}});
  $("led-save").addEventListener("click",()=>{try{void sendConfig("led","save",collectLed());}catch(e){inform(e);}});
  $("reload-wifi").addEventListener("click",()=>void loadWifi());
  $("reload-network").addEventListener("click",()=>void refreshStatus());
  $("wifi-save").addEventListener("click",()=>void saveWifi(false));
  $("wifi-forget").addEventListener("click",()=>void saveWifi(true));
  $("wifi-retry").addEventListener("click",()=>void runBusy(async()=>{await releaseControl(false);await invoke("device_api",{path:"/api/network/retry",body:{}});notify("Station join retry requested.");}));
}
async function boot(){wire();await loadPrefs();await refreshControl();
  setInterval(()=>{if(online&&!pollBusy&&!actionBusy&&!currentControl.active)void refreshStatus();},6500);
  setInterval(()=>void refreshControl(),320);
  setConnection(false);notify("Portable companion ready. Connect to your adapter.");
}
void boot();
