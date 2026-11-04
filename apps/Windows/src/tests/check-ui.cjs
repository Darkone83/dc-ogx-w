'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const html=fs.readFileSync(path.join(root,'ui/index.html'),'utf8');
const script=fs.readFileSync(path.join(root,'ui/app.js'),'utf8');
const css=fs.readFileSync(path.join(root,'ui/styles.css'),'utf8');
const rust=fs.readFileSync(path.join(root,'src-tauri/src/control.rs'),'utf8');
const cargo=fs.readFileSync(path.join(root,'src-tauri/Cargo.toml'),'utf8');
const cfg=JSON.parse(fs.readFileSync(path.join(root,'src-tauri/tauri.conf.json'),'utf8'));
assert.equal(cfg.build.frontendDist,'../ui');
assert.equal(cfg.bundle.active,false);
assert.match(cargo,/tungstenite/);
assert.match(cargo,/opt-level = "s"/);
for(const page of ['overview','control','profiles','led','device','network','about']){
  assert.match(html,new RegExp(`id="page-${page}"`),`Missing ${page} page`);
  assert.match(html,new RegExp(`data-page="${page}"`),`Missing ${page} tab`);
}
const ids=[...html.matchAll(/\bid="([^"]+)"/g)].map(x=>x[1]);
const set=new Set(ids);
assert.equal(ids.length,set.size,`Duplicate IDs: ${ids.filter((id,i)=>ids.indexOf(id)!==i).join(', ')}`);
for(const [,id] of script.matchAll(/\$\("([^"]+)"\)/g)) assert(set.has(id),`Missing DOM #${id}`);
for(const theme of ['darkone','theia-dark','midnight','synthwave','ember','terminal','theia-light','arctic-light']) {
  assert(script.includes(`"${theme}"`),`Missing theme ${theme}`);
  assert(html.includes(`value="${theme}"`),`Missing theme option ${theme}`);
  assert(css.includes(`data-theme="${theme}"`),`Missing WebUI CSS theme ${theme}`);
}
assert(!html.includes('value="white"')&&!html.includes('value="black"'),'White/Black are buttons, not themes');
assert(css.includes('--green:#8cf927')&&css.includes('--ink:#eff9ef'),'Must reuse actual WebUI palette');
assert(html.includes('class="app-layout"')&&html.includes('class="app-nav"')&&html.includes('class="pad-zone"'),'Missing original WebUI structural layout');
assert(fs.existsSync(path.join(root,'ui/webui-logo.png')),'Expected actual WebUI logo');
for(const [name,id,bit] of [['White','lb','256'],['Black','rb','512']]){
  assert(html.includes(`id="pad-${id}"`),`Missing visible ${name} button`);
  assert(html.includes(`data-action="${id}"`),`Missing interactive ${name} binding`);
  assert(html.includes(name),`Missing ${name} text`);
  assert(script.includes(`"${id}","${name} / ${id.toUpperCase()}"`),`Missing ${name} remappable key`);
  assert(script.includes(`${id}:${bit}`),`Missing ${name} protocol bit`);
}
assert.match(script,/wireOnscreenControls/);
assert.match(script,/set_keyboard_input/);
assert.match(script,/pointercancel/);
assert.match(script,/resetOnscreen/);
assert.match(rust,/\(0x0100, 256\), \(0x0200, 512\)/);
console.log('PASS: WebUI layout/style, 8 genuine themes, original Xbox White/Black, keyboard/XInput mappings, native app structure');
