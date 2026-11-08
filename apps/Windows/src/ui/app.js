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
  toastTimer, pollBusy = false, connectionFailures = 0, actionBusy = false,
  connectionEpoch = 0, controlPollBusy = false, sessionBusy = false, sourceBusy = false,
  inputFocused = true, browserNeedsNeutral = true, browserMessage = "Press a button to expose your controller to Windows.";
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
  if(online!==connected){connectionEpoch++;connectionFailures=0;}
  online = connected;
  $("sidebar-connection").textContent = connected ? "● Adapter connected" : "● Not connected";
  $("sidebar-connection").classList.toggle("online",connected);
  $("top-status").textContent = connected ? connectionFailures>=2?"● Status delayed":"● Connected" : "● Offline";
  $("top-status").classList.toggle("online",connected&&connectionFailures<2);
  $("connect-button").disabled = connected || actionBusy;
  $("disconnect-button").disabled = !connected || actionBusy;
  $("take-button").disabled = !connected || actionBusy || sessionBusy || currentControl.active || currentControl.starting || currentControl.token_held;
  $("footer-connection").textContent = connected ? $("device-address").value : "DC-OGX-W · PORTABLE";
  for (const id of ["oled-apply","oled-save"]) $(id).disabled = !connected || !oled?.available || actionBusy;
  for (const id of ["led-apply","led-save"]) $(id).disabled = !connected || !led || led.available===false || actionBusy;
  for (const id of ["wifi-save","wifi-forget","wifi-retry"]) $(id).disabled = !connected || !wifi || actionBusy;
  for(const button of document.querySelectorAll("[data-mode]")) button.disabled = !connected || actionBusy ||
    lastStatus?.control?.mode_pending===true || (lastStatus?.control?.physical_supported===false && button.dataset.mode!=="wifi");
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
  const names={physical:"Physical (USB / Bluetooth)",wifi:"Wi-Fi remote",configure:"Configuration"};
  setText("mode-status",`Current: ${names[c.mode]||c.mode||"?"}${c.mode_pending?" (transition pending)":""}`+
    (c.bluetooth_supported===false?" · Bluetooth unavailable on this board; wired USB remains available when supported.":""));
  setConnection(online);
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
    resetKeyboard();
    try{await invoke("disconnect_device");}
    finally{setConnection(false);lastStatus=null;oled=led=wifi=null;}
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
  pollBusy=true;const epoch=connectionEpoch;
  try {const status=await invoke("device_api",{path:"/api/status",body:null});
    if(!online||epoch!==connectionEpoch)return;
    connectionFailures=0;updateStatus(status);}
  catch(e){if(online&&epoch===connectionEpoch){connectionFailures++;
    if(connectionFailures>=2){setText("top-status","● Status delayed");$("top-status").classList.remove("online");
      if(connectionFailures===2)notify("Status telemetry is delayed. Check the adapter connection.",true);}}}
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
  if(!online||actionBusy||sessionBusy||sourceBusy||currentControl.starting||currentControl.token_held)return;
  sessionBusy=true;setConnection(online);
  try {
    await resetKeyboard();await invoke("set_input_source",{source:$("control-source").value,slot:Number($("gamepad-slot").value)});
    await invoke("take_control");
    if(document.activeElement instanceof HTMLButtonElement) document.activeElement.blur();
    notify("Controller session acquired. Press Esc to release.");
  } catch(e) { inform(e); } finally{sessionBusy=false;}
  await refreshControl();
}
async function releaseControl(show=true) {
  resetKeyboard();
  try {await invoke("release_control");if(show)notify("Controller released; output neutralized.");}
  catch(e){if(show)inform(e);}
  await refreshControl();
}
async function refreshControl() {
  if(controlPollBusy)return;
  controlPollBusy=true;
  try {
    const data=await invoke("control_status");currentControl=data;
    const active=data.active===true;
    $("take-button").disabled=!online||active||actionBusy||sessionBusy||sourceBusy||data.starting||data.token_held;
    $("release-button").disabled=!active&&!data.starting;
    $("gamepad-slot").disabled=$("control-source").value==="keyboard"||sourceBusy;
    setText("control-tag",active?"● Owned":data.starting?"Starting…":data.token_held?"Releasing…":"Released");
    showPad(data.last_sent||NEUTRAL());
    updateInteractiveAvailability();
    const sourceName={gamepad:"Native XInput",browser:"Standard gamepad",keyboard:"Keyboard + on-screen"}[data.source];
    $("control-message").textContent=active
      ? !data.focused?"Input paused while the app is unfocused. Centre controls before resuming."
        : data.source==="gamepad"&&data.gamepad_connected===false?"No XInput controller in this slot. Check the slot or choose Standard gamepad."
        : data.source==="browser"&&browserMessage?browserMessage
        : data.needs_neutral?"Centre sticks and release all controls to begin."
        : `Controller active · ${sourceName}. F8 or Esc releases.`
      : data.starting?"Starting a controller session…":data.token_held?"Sending neutral and releasing ownership…"
      : online?"Connected. Take control to use keyboard or a PC controller.":"Connect to an adapter to take control.";
    if(data.error&&!active&&data.error!==refreshControl.previousError) notify(data.error,true);
    refreshControl.previousError=data.error;
  } catch(e){ /* App may be closing. */ } finally{controlPollBusy=false;}
}
function resetKeyboard(){
  pressed.clear();resetOnscreen();browserNeedsNeutral=true;
  return queueInput(NEUTRAL());
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
let pendingInput=null,inputFlight=null;
function queueInput(snapshot){
  // At most one IPC in flight plus the newest snapshot. Release replaces held input.
  pendingInput=snapshot;
  if(!inputFlight)inputFlight=(async()=>{
    while(pendingInput){const input=pendingInput;pendingInput=null;
      try{await invoke("set_keyboard_input",{input});}catch(e){pendingInput=null;}}
  })().finally(()=>{inputFlight=null;if(pendingInput)void queueInput(pendingInput);});
  return inputFlight;
}
function pushKeyboard() {
  if(!inputFocused||sourceBusy||!currentControl.active||$("control-source").value!=="keyboard")return;
  void queueInput(buildKeyboardState());
}
function standardPad(gamepad){
  const p=NEUTRAL(),pressed=i=>Boolean(gamepad.buttons[i]?.pressed);
  for(const [index,bit] of [[0,1],[1,2],[2,4],[3,8],[4,256],[5,512],[8,64],[9,128],[10,16],[11,32],[16,1024]])
    if(pressed(index))p.buttons|=bit;
  for(const [index,bit] of [[12,1],[13,2],[14,4],[15,8]])if(pressed(index))p.dpad|=bit;
  if((p.dpad&3)===3)p.dpad&=~3;if((p.dpad&12)===12)p.dpad&=~12;
  const clamp=(n,min,max)=>Number.isFinite(n)?Math.max(min,Math.min(max,n)):0;
  const axis=n=>Math.round(clamp(n,-1,1)*(n<0?32768:32767));
  [p.lx,p.ly,p.rx,p.ry]=[axis(gamepad.axes[0]),axis(-gamepad.axes[1]),axis(gamepad.axes[2]),axis(-gamepad.axes[3])];
  p.lt=Math.round(clamp(gamepad.buttons[6]?.value,0,1)*255);p.rt=Math.round(clamp(gamepad.buttons[7]?.value,0,1)*255);
  return p;
}
function centred(p){return !p.buttons&&!p.dpad&&p.lt<=8&&p.rt<=8&&[p.lx,p.ly,p.rx,p.ry].every(n=>Math.abs(n)<=4096);}
function pollGamepad(){
  if(sourceBusy||!currentControl.active||!inputFocused||document.hidden||currentPage!=="control"||$("control-source").value!=="browser")return;
  let pad;try{pad=navigator.getGamepads?.()[Number($("gamepad-slot").value)];}catch(e){/* Runtime may deny gamepad access. */}
  if(!pad?.connected||pad.mapping!=="standard"){
    browserNeedsNeutral=true;browserMessage=pad?"This controller has no standard mapping. Use XInput or a Windows mapping tool.":"No standard gamepad in this slot. Press a controller button, then check the slot.";
    void queueInput(NEUTRAL());return;
  }
  const p=standardPad(pad);
  if(browserNeedsNeutral){if(centred(p))browserNeedsNeutral=false;
    browserMessage=browserNeedsNeutral?"Centre sticks and release all controls to begin.":"";void queueInput(NEUTRAL());return;}
  browserMessage="";void queueInput(p);
}
async function changeSource(){
  if(sourceBusy)return;sourceBusy=true;
  $("control-source").disabled=true;$("gamepad-slot").disabled=true;
  try{await resetKeyboard();prefs.source=$("control-source").value;prefs.slot=Number($("gamepad-slot").value);
    await invoke("set_input_source",{source:prefs.source,slot:prefs.slot});await savePrefs();}
  catch(e){inform(e);}finally{sourceBusy=false;$("control-source").disabled=false;await refreshControl();}
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
  if(!online)return;const epoch=connectionEpoch;
  try {
    const data=await invoke("device_api",{path:"/api/profiles",body:null});
    if(!online||epoch!==connectionEpoch)return;
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
  }catch(e){if(online&&epoch===connectionEpoch)inform(e);}
}
async function changeMode(mode) {
  if(!online)return;
  if(lastStatus?.control?.physical_supported===false&&mode!=="wifi")return;
  await runBusy(async()=>{await releaseControl(false);
    await invoke("device_api",{path:"/api/control/mode",body:{mode}});
    notify("Mode change requested. Wait for the adapter to finish transitioning.");
    await refreshStatus();
  });
}
function percent(raw){return Math.round(Number(raw)/255*100)+"%";}
function updateRangeLabels(){setText("oled-bright-label",percent($("oled-bright").value));setText("oled-idle-label",percent($("oled-idle").value));setText("led-bright-label",percent($("led-bright").value));}
async function loadOled() {
  if(!online)return;const epoch=connectionEpoch;
  try{
    const data=await invoke("device_api",{path:"/api/oled",body:null});
    if(!online||epoch!==connectionEpoch)return;oled=data;
    $("oled-enabled").checked=Boolean(oled.enabled);$("oled-bright").value=oled.brightness;
    $("oled-idle").value=oled.idle_brightness;$("oled-dim").value=oled.dim_after_s;
    $("oled-off").value=oled.off_after_s;$("oled-invert").checked=Boolean(oled.invert);
    $("oled-flip").checked=Boolean(oled.flip);$("oled-fade").checked=Boolean(oled.fade);
    $("oled-note").textContent=oled.available?"Apply is temporary. Save & restart writes persistent settings.":"OLED is not enabled in this firmware build.";
    for(const id of ["oled-apply","oled-save"])$(id).disabled=!oled.available;
    updateRangeLabels();
  }catch(e){if(online&&epoch===connectionEpoch){oled=null;inform(e);}}
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
    if(action==="save")setConnection(false);
    notify(action==="save"?"Settings saved; adapter restarting. Reconnect after boot.":"Settings applied for this session.");
  });
}
function makeColorField(name,id,value){
  const row=document.createElement("label"),input=document.createElement("input"),span=document.createElement("span");
  row.className="color-field";input.type="color";input.value=/^#[0-9a-fA-F]{6}$/.test(value)?value:"#000000";
  input.id=id;span.textContent=name;row.append(input,span);return row;
}
async function loadLed() {
  if(!online)return;const epoch=connectionEpoch;
  try{
    const data=await invoke("device_api",{path:"/api/led",body:null});
    if(!online||epoch!==connectionEpoch)return;led=data;
    $("led-enabled").checked=Boolean(led.enabled);$("led-bright").value=led.brightness;
    $("led-mode").value=led.mode;
    const profiles=$("led-profile-colors"),diagnostics=$("led-diag-colors");profiles.replaceChildren();diagnostics.replaceChildren();
    (led.profile_colors||[]).forEach((color,i)=>profiles.append(makeColorField(`Profile ${i+1}`,`led-profile-${i}`,color)));
    for(const [name,color] of Object.entries(led.diagnostic_colors||{}))diagnostics.append(makeColorField(name,`led-diag-${name}`,color));
    $("led-note").textContent=led.available===false?"RGB LED unavailable in this build.":"Apply to preview. Save & restart to retain settings.";
    for(const id of ["led-apply","led-save"])$(id).disabled=led.available===false;
    updateRangeLabels();
  }catch(e){if(online&&epoch===connectionEpoch){led=null;inform(e);}}
}
function collectLed(){
  if(!led)throw Error("Load LED settings first.");
  return {enabled:$("led-enabled").checked,brightness:Number($("led-bright").value),mode:$("led-mode").value,
    profile_colors:(led.profile_colors||[]).map((_,i)=>$("led-profile-"+i).value.toUpperCase()),
    diagnostic_colors:Object.fromEntries(Object.keys(led.diagnostic_colors||{}).map(k=>[k,$("led-diag-"+k).value.toUpperCase()]))};
}
async function loadWifi() {
  if(!online)return;const epoch=connectionEpoch;
  try{
    const data=await invoke("device_api",{path:"/api/network/config",body:null});
    if(!online||epoch!==connectionEpoch)return;wifi=data;
    $("wifi-enabled").checked=Boolean(wifi.station_enabled||!wifi.station_ssid);
    $("wifi-ssid").value=wifi.station_ssid||"";$("wifi-password").value="";
    $("wifi-hostname").value=wifi.hostname||"dc-ogx-w";
    $("wifi-open").checked=Boolean(wifi.station_enabled&&wifi.station_ssid&&!wifi.password_saved);
    $("wifi-note").textContent="Changes restart the adapter. After a successful station join, the recovery AP shuts down.";
    for(const id of ["wifi-save","wifi-forget","wifi-retry"])$(id).disabled=false;
  }catch(e){if(online&&epoch===connectionEpoch){wifi=null;inform(e);}}
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
    setConnection(false);
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
  $("control-source").value=["gamepad","browser"].includes(prefs.source)?prefs.source:"keyboard";
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
  $("control-source").addEventListener("change",()=>void changeSource());
  $("gamepad-slot").addEventListener("change",()=>void changeSource());
  $("reset-keys").addEventListener("click",()=>{if(confirm("Reset keyboard mapping to defaults?")){keyBindings={...DEFAULT_KEYS};prefs.keyBindings={...keyBindings};renderKeymap();void savePrefs();}});
  window.addEventListener("keydown",keyDown);window.addEventListener("keyup",keyUp);
  window.addEventListener("blur",()=>{inputFocused=false;resetKeyboard();void invoke("set_input_focus",{focused:false}).catch(()=>{});});
  window.addEventListener("focus",()=>{inputFocused=!document.hidden;void invoke("set_input_focus",{focused:inputFocused}).catch(()=>{});});
  document.addEventListener("visibilitychange",()=>{inputFocused=!document.hidden&&document.hasFocus();resetKeyboard();void invoke("set_input_focus",{focused:inputFocused}).catch(()=>{});});
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
  setInterval(pollGamepad,16);
  setConnection(false);notify("Portable companion ready. Connect to your adapter.");
}
void boot();
