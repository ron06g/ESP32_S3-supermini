// ===========================================================================
//  S3-KBD — client Web Bluetooth (mock). Le web envoie l'INTENTION (char/key/…),
//  la traduction AZERTY -> HID est dans le firmware. Contrat GATT : socle §5.
// ===========================================================================
const SERVICE_UUID = '9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001';
const CMD_UUID     = '9f1d0001-5b8e-4a4a-9c2a-2b7f3e6a1001';
const STATUS_UUID  = '9f1d0002-5b8e-4a4a-9c2a-2b7f3e6a1001';
const MOD = { ctrl:1, shift:2, alt:4, gui:8, altgr:16 };

let device=null, gatt=null, cmdChar=null, statusChar=null;
let deviceName='', lastStatus='';
let activeTransport = null;         // façade : transport actif (BLE ou Wi-Fi)
let WIFI_WS_PORT = 81;              // port WebSocket du S3 (résolution A)
const mods = { ctrl:0, shift:0, alt:0, gui:0, altgr:0 };   // 0 off, 1 one-shot, 2 verrou
const $ = (s) => document.querySelector(s);

// ===========================================================================
//  Envoi
// ===========================================================================
async function send(obj, reliable=false) {
  const json = JSON.stringify(obj);
  logLine('out', json);
  if (!activeTransport || !activeTransport.connected) { toast('Non connecté'); return; }
  try { await activeTransport.send(json, reliable); }
  catch (e) { logLine('err', 'écriture: ' + e.message); }
}

// ===========================================================================
//  Connexion (bouton unique état + action)
// ===========================================================================
function setConn(state) {
  const b = $('#btnConn'); b.classList.remove('primary','wait','on');
  if (state === 'off') { b.classList.add('primary'); b.innerHTML = 'Connecter'; }
  else if (state === 'wait') { b.classList.add('wait'); b.innerHTML = '<span class="cdot"></span>Connexion…'; }
  else { b.classList.add('on'); renderConn(); }
}
function renderConn() {
  $('#btnConn').innerHTML = `<span class="cdot"></span>${esc(deviceName || 'connecté')}` +
    (lastStatus ? ` · ${esc(lastStatus)}` : '');
}
function onConnClick() {
  const t = activeTransport;
  if (t && t.connected) t.disconnect(); else if (t) t.connect();
}

async function bleConnect() {
  logLine('out', 'clic « Connecter »');
  if (!navigator.bluetooth) {
    logLine('err', 'navigator.bluetooth ABSENT — Chrome/Edge via https/localhost requis.');
    toast('Web Bluetooth indisponible'); return;
  }
  try {
    if (navigator.bluetooth.getAvailability) {
      const avail = await navigator.bluetooth.getAvailability();
      logLine('in', 'Adaptateur Bluetooth actif: ' + (avail ? 'oui' : 'NON'));
      if (!avail) logLine('err', 'Aucun adaptateur Bluetooth actif — active le Bluetooth de Windows.');
    }
    setConn('wait');
    device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }], optionalServices: [SERVICE_UUID],
    });
    logLine('in', 'appareil choisi: ' + (device.name || device.id || '?'));
    device.addEventListener('gattserverdisconnected', onDisconnected);
    gatt = await device.gatt.connect();
    const svc = await gatt.getPrimaryService(SERVICE_UUID);
    cmdChar = await svc.getCharacteristic(CMD_UUID);
    logLine('in', 'caractéristique CMD OK');
    try {
      statusChar = await svc.getCharacteristic(STATUS_UUID);
      await statusChar.startNotifications();
      statusChar.addEventListener('characteristicvaluechanged', onStatus);
      logLine('in', 'notifications STATUS activées');
    } catch (e) { logLine('err', 'STATUS indisponible: ' + e.name + ' — ' + e.message); }
    deviceName = device.name || 'S3-KBD'; lastStatus = '';
    setConn('on'); logLine('in', 'connecté à ' + deviceName);
    onConnected();
  } catch (e) {
    setConn('off');
    if (e.name === 'NotFoundError') logLine('err', 'Aucun appareil sélectionné, ou aucun S3-KBD diffusé. (' + e.name + ')');
    else logLine('err', 'connexion: ' + e.name + ' — ' + e.message);
  }
}
function bleDisconnect() { if (gatt && gatt.connected) gatt.disconnect(); onDisconnected(); }
function onDisconnected() {
  cmdChar = statusChar = gatt = null; lastStatus = '';
  setConn('off'); logLine('err', 'déconnecté');
}
// Statut unifié : appelé par le BLE (après décodage DataView) et par le Wi-Fi (frame texte).
// Les préfixes cfg:/scan:/pair:/link:/gpio:/pong: sont des ÉVÉNEMENTS (dispatch) ;
// le reste (ready/busy/err:*) est l'état courant affiché sur le bouton de connexion.
function handleStatus(text) {
  logLine('in', 'STATUS ' + text);
  const i = text.indexOf(':');
  const pfx = i > 0 ? text.slice(0, i) : text, rest = i > 0 ? text.slice(i + 1) : '';
  switch (pfx) {
    case 'cfg':  onCfg(rest);  return;
    case 'scan': onScan(rest); return;
    case 'pair': onPair(rest); return;
    case 'link': onLink(rest); return;
    case 'gpio': onGpio(rest); return;
    case 'pong': onPong(rest); return;
  }
  lastStatus = text;
  if (activeTransport && activeTransport.connected) renderConn();
}
function onStatus(e) { handleStatus(new TextDecoder().decode(e.target.value)); }
// Après toute connexion (BLE ou Wi-Fi) : lire la config pour adapter l'IHM.
function onConnected() { send({ t:'cfg', a:'get' }, true); }

// ===========================================================================
//  Façade de transport : BLE (existant) + Wi-Fi (WebSocket, résolution A)
//  Interface commune : connect(), disconnect(), get connected, send(json, reliable).
//  L'IHM et l'encodage des commandes ne dépendent que de send() → aucun changement.
// ===========================================================================
const bleTransport = {
  kind: 'ble',
  connect: bleConnect,
  disconnect: bleDisconnect,
  get connected() { return !!(gatt && gatt.connected); },
  async send(json, reliable) {
    if (!cmdChar) throw new Error('CMD indisponible');
    const data = new TextEncoder().encode(json);
    if (!reliable && cmdChar.writeValueWithoutResponse) await cmdChar.writeValueWithoutResponse(data);
    else await cmdChar.writeValue(data);
  },
};
const wifiTransport = {
  kind: 'wifi', ws: null, ip: null,
  connect() { wifiConnect(this.ip); },
  disconnect() { if (this.ws) { try { this.ws.close(); } catch (e) {} } },
  get connected() { return !!(this.ws && this.ws.readyState === WebSocket.OPEN); },
  send(json /*, reliable */) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) throw new Error('WebSocket non ouvert');
    this.ws.send(json);   // TCP : fiable et ordonné, le drapeau « reliable » est sans objet
  },
};
function wifiConnect(ip) {
  const url = 'ws://' + ip + ':' + WIFI_WS_PORT + '/';
  logLine('out', 'connexion Wi-Fi ' + url);
  setConn('wait');
  let ws;
  try { ws = new WebSocket(url); }
  catch (e) { setConn('off'); logLine('err', 'WebSocket: ' + e.message); return; }
  wifiTransport.ws = ws;
  ws.onopen    = () => { deviceName = 'S3-KBD (Wi-Fi)'; lastStatus = ''; setConn('on'); logLine('in', 'connecté (Wi-Fi) à ' + ip); onConnected(); };
  ws.onmessage = (e) => { if (typeof e.data === 'string') handleStatus(e.data); };
  ws.onclose   = () => { wifiTransport.ws = null; lastStatus = ''; setConn('off'); logLine('err', 'déconnecté (Wi-Fi)'); };
  ws.onerror   = () => logLine('err', 'erreur WebSocket');
}

// ===========================================================================
//  Modificateurs collants (AZERTY)
// ===========================================================================
function cycleMod(name) { mods[name] = (mods[name] + 1) % 3; refreshMods(); }
function currentMask(includeShiftAltgr) {
  let m = 0;
  if (mods.ctrl) m |= MOD.ctrl; if (mods.alt) m |= MOD.alt; if (mods.gui) m |= MOD.gui;
  if (includeShiftAltgr) { if (mods.shift) m |= MOD.shift; if (mods.altgr) m |= MOD.altgr; }
  return m;
}
function consumeMods() {
  let ch = false; for (const k in mods) if (mods[k] === 1) { mods[k] = 0; ch = true; }
  if (ch) refreshMods();
}
function refreshMods() {
  document.querySelectorAll('.key.mod').forEach((el) => {
    const s = mods[el.dataset.mod];
    el.classList.toggle('armed', s === 1); el.classList.toggle('locked', s === 2);
    const lk = el.querySelector('.lock'); if (lk) lk.textContent = s === 2 ? '🔒' : '';
  });
}

// ===========================================================================
//  Définition des touches
// ===========================================================================
const c = (base, shift, altgr) => ({ t:'char', base, shift, altgr });
const k = (code, label, cls) => ({ t:'key', code, label, cls });
const m = (mod, label) => ({ t:'mod', mod, label });
const br = () => ({ t:'br' });

const KB_AZERTY = [
  [ c('&','1'), c('é','2','~'), c('"','3','#'), c("'",'4','{'), c('(','5','['),
    c('-','6','|'), c('è','7','`'), c('_','8','\\'), c('ç','9','^'), c('à','0','@'),
    c(')','°',']'), c('=','+','}'), k('Backspace','⌫','wide fn') ],
  [ k('Tab','Tab','wide fn'), c('a','A'), c('z','Z'), c('e','E','€'), c('r','R'),
    c('t','T'), c('y','Y'), c('u','U'), c('i','I'), c('o','O'), c('p','P'),
    c('^','¨'), c('$','£','¤'), k('Enter','⏎','wide fn') ],
  [ k('CapsLock','Maj','wide fn'), c('q','Q'), c('s','S'), c('d','D'), c('f','F'),
    c('g','G'), c('h','H'), c('j','J'), c('k','K'), c('l','L'), c('m','M'),
    c('ù','%'), c('*','µ') ],
  [ m('shift','Shift'), c('<','>'), c('w','W'), c('x','X'), c('c','C'), c('v','V'),
    c('b','B'), c('n','N'), c(',','?'), c(';','.'), c(':','/'), c('!','§'), m('shift','Shift') ],
  [ m('ctrl','Ctrl'), m('gui','Win'), m('alt','Alt'), k('Space','Espace','space fn'),
    m('altgr','AltGr'), m('ctrl','Ctrl') ],
];

const KB_NUM = [
  [ c('7'), c('8'), c('9'), c('/'), k('Backspace','⌫','fn') ],
  [ c('4'), c('5'), c('6'), c('*'), k('Escape','Échap','fn') ],
  [ c('1'), c('2'), c('3'), c('-'), k('Tab','Tab','fn') ],
  [ c('0'), c('.'), c(','), c('+'), k('ArrowUp','↑','fn') ],
  [ c('('), c(')'), c('='), c('€'), k('Enter','Entrée','wide fn') ],
];

// Raccourcis Windows courants — combinaison (gros) + fonction (petit)
const SHORTCUTS = [
  { combo:'Ctrl+C', fn:'Copier', cmd:{t:'char',v:'c',m:MOD.ctrl} },
  { combo:'Ctrl+V', fn:'Coller', cmd:{t:'char',v:'v',m:MOD.ctrl} },
  { combo:'Ctrl+X', fn:'Couper', cmd:{t:'char',v:'x',m:MOD.ctrl} },
  { combo:'Ctrl+Z', fn:'Annuler', cmd:{t:'char',v:'z',m:MOD.ctrl} },
  { combo:'Ctrl+Y', fn:'Rétablir', cmd:{t:'char',v:'y',m:MOD.ctrl} },
  { combo:'Ctrl+A', fn:'Tout sélectionner', cmd:{t:'char',v:'a',m:MOD.ctrl} },
  { combo:'Ctrl+S', fn:'Enregistrer', cmd:{t:'char',v:'s',m:MOD.ctrl} },
  { combo:'Ctrl+F', fn:'Rechercher', cmd:{t:'char',v:'f',m:MOD.ctrl} },
  { combo:'Ctrl+P', fn:'Imprimer', cmd:{t:'char',v:'p',m:MOD.ctrl} },
  { combo:'Ctrl+T', fn:'Nouvel onglet', cmd:{t:'char',v:'t',m:MOD.ctrl} },
  { combo:'Ctrl+W', fn:'Fermer l\'onglet', cmd:{t:'char',v:'w',m:MOD.ctrl} },
  { combo:'Ctrl+Maj+T', fn:'Rouvrir l\'onglet', cmd:{t:'char',v:'T',m:MOD.ctrl} },
  { combo:'Ctrl+Tab', fn:'Onglet suivant', cmd:{t:'key',c:'Tab',a:'tap',m:MOD.ctrl} },
  { combo:'Ctrl+Maj+Tab', fn:'Onglet précédent', cmd:{t:'key',c:'Tab',a:'tap',m:MOD.ctrl|MOD.shift} },
  { combo:'Alt+Tab', fn:'Basculer fenêtre', cmd:{t:'seq',n:'alt_tab'} },
  { combo:'Alt+F4', fn:'Fermer', cmd:{t:'seq',n:'alt_f4'} },
  { combo:'Ctrl+Alt+Suppr', fn:'Sécurité', cmd:{t:'seq',n:'ctrl_alt_del'} },
  { combo:'Ctrl+Échap', fn:'Menu Démarrer', cmd:{t:'seq',n:'ctrl_esc'} },
  { combo:'Ctrl+Maj+Échap', fn:'Gestion. tâches', cmd:{t:'key',c:'Escape',a:'tap',m:MOD.ctrl|MOD.shift} },
  { combo:'Win+D', fn:'Bureau', cmd:{t:'seq',n:'win_d'} },
  { combo:'Win+E', fn:'Explorateur', cmd:{t:'char',v:'e',m:MOD.gui} },
  { combo:'Win+R', fn:'Exécuter', cmd:{t:'char',v:'r',m:MOD.gui} },
  { combo:'Win+L', fn:'Verrouiller', cmd:{t:'char',v:'l',m:MOD.gui} },
  { combo:'Win+S', fn:'Recherche', cmd:{t:'char',v:'s',m:MOD.gui} },
  { combo:'Win+I', fn:'Paramètres', cmd:{t:'char',v:'i',m:MOD.gui} },
  { combo:'Win+Maj+S', fn:'Capture zone', cmd:{t:'char',v:'S',m:MOD.gui} },
  { combo:'Impr. écran', fn:'Capture écran', cmd:{t:'key',c:'PrintScreen',a:'tap'} },
];

const FN_GROUPS = [
  { title:'Télécommande / média', keys: [
    k('MediaVolumeDown','🔉 Vol−','fn nav'), k('MediaVolumeUp','🔊 Vol+','fn nav'), k('MediaMute','🔇 Muet','fn nav'),
    k('MediaPlayPause','⏯ Lecture','fn nav'), k('MediaPrevious','⏮ Préc','fn nav'), k('MediaNext','⏭ Suiv','fn nav'),
    k('MediaHome','🏠 Accueil','fn nav'), k('MediaBack','↩ Retour','fn nav') ] },
  { title:'Touches de fonction', keys: Array.from({length:12}, (_,i) => k('F'+(i+1),'F'+(i+1),'fn nav')) },
  { title:'Navigation', keys: [
    k('Escape','Échap','fn nav'), k('Tab','Tab','fn nav'), k('PrintScreen','Impr','fn nav'),
    k('Delete','Suppr','fn nav'), k('Insert','Inser','fn nav'), k('ContextMenu','Menu','fn nav'),
    k('Home','Début','fn nav'), k('End','Fin','fn nav'), k('PageUp','Pg↑','fn nav'), k('PageDown','Pg↓','fn nav'),
    br(),
    k('ArrowLeft','←','fn nav'), k('ArrowUp','↑','fn nav'), k('ArrowDown','↓','fn nav'), k('ArrowRight','→','fn nav') ] },
  { title:'Raccourcis', shortcuts: SHORTCUTS },
];

// ===========================================================================
//  Rendu des touches
// ===========================================================================
function keyLabelChar(d) {
  let h = `<span class="main">${esc(d.base)}</span>`;
  if (d.shift && d.shift !== d.base) h = `<span class="shift">${esc(d.shift)}</span>` + h;
  if (d.altgr) h += `<span class="altgr">${esc(d.altgr)}</span>`;
  return h;
}
function makeKey(d) {
  if (d.t === 'br') { const b = document.createElement('div'); b.className = 'break'; return b; }
  const el = document.createElement('div');
  if (d.t === 'char') {
    el.className = 'key charkey'; el.innerHTML = keyLabelChar(d);
    el.addEventListener('click', () => onCharKey(d, el));
  } else if (d.t === 'key') {
    el.className = 'key ' + (d.cls || 'fn'); el.textContent = d.label;
    el.addEventListener('click', () => onNamedKey(d, el));
  } else if (d.t === 'mod') {
    el.className = 'key mod fn'; el.dataset.mod = d.mod;
    el.innerHTML = `${d.label}<span class="lock"></span>`;
    el.addEventListener('click', () => cycleMod(d.mod));
  }
  return el;
}
function buildKeyboard(sel, rows) {
  const kb = $(sel); kb.innerHTML = '';
  for (const row of rows) {
    const r = document.createElement('div'); r.className = 'krow';
    for (const d of row) r.appendChild(makeKey(d));
    kb.appendChild(r);
  }
}
function buildFn() {
  const box = $('#fnpad'); box.innerHTML = '';
  for (const g of FN_GROUPS) {
    const grp = document.createElement('div'); grp.className = 'fngroup';
    grp.innerHTML = `<h3>${g.title}</h3>`;
    const cl = document.createElement('div'); cl.className = 'cluster';
    if (g.shortcuts) {
      for (const sc of g.shortcuts) {
        const el = document.createElement('button'); el.className = 'btn shortcut';
        el.innerHTML = `<span class="sc-combo">${esc(sc.combo)}</span><span class="sc-fn">${esc(sc.fn)}</span>`;
        el.addEventListener('click', () => { flash(el); send(sc.cmd, true); });
        cl.appendChild(el);
      }
    } else {
      for (const d of g.keys) cl.appendChild(makeKey(d));
    }
    grp.appendChild(cl); box.appendChild(grp);
  }
}

// ===========================================================================
//  Actions clavier (mode direct)
// ===========================================================================
function onCharKey(d, el) {
  let ch = d.base;
  if (mods.altgr && d.altgr) ch = d.altgr; else if (mods.shift && d.shift) ch = d.shift;
  flash(el);
  const mask = currentMask(false);
  send(mask ? { t:'char', v:ch, m:mask } : { t:'char', v:ch }, false);
  consumeMods();
}
function onNamedKey(d, el) {
  flash(el);
  if (d.code.startsWith('Media')) { send({ t:'key', c:d.code, a:'tap' }, false); return; }
  const mask = currentMask(true);
  const cmd = { t:'key', c:d.code, a:'tap' }; if (mask) cmd.m = mask;
  send(cmd, false); consumeMods();
}

// ===========================================================================
//  Souris — boutons à maintien (drag) + molette répétée + réglage sensibilité
// ===========================================================================
function initMouse() {
  const pad = $('#trackpad');
  let last = null, accX = 0, accY = 0, timer = null, downT = 0, moved = 0;
  const sens = () => (+$('#mouseSens').value || 8) / 8;
  const flush = () => { if (accX || accY) { send({ t:'mouse', dx:Math.round(accX), dy:Math.round(accY) }); accX = accY = 0; } timer = null; };
  pad.addEventListener('pointerdown', (e) => {
    pad.setPointerCapture(e.pointerId); pad.classList.add('active');
    last = { x:e.clientX, y:e.clientY }; downT = Date.now(); moved = 0;
  });
  pad.addEventListener('pointermove', (e) => {
    if (!last) return;
    const dx = (e.clientX - last.x) * sens(), dy = (e.clientY - last.y) * sens();
    last = { x:e.clientX, y:e.clientY }; accX += dx; accY += dy; moved += Math.abs(dx) + Math.abs(dy);
    if (!timer) timer = setTimeout(flush, 40);
  });
  const up = () => {
    if (!last) return;
    last = null; pad.classList.remove('active'); if (timer) { clearTimeout(timer); flush(); }
    if (Date.now() - downT < 220 && moved < 6) send({ t:'mouse', b:'left', a:'click' });
  };
  pad.addEventListener('pointerup', up); pad.addEventListener('pointercancel', up);

  // Boutons L/M/R : maintien (down/up) -> déplacement de fenêtre avec clic maintenu
  document.querySelectorAll('.mbtn[data-mb]').forEach((b) => {
    const btn = b.dataset.mb;
    const down = (e) => { e.preventDefault(); b.classList.add('held'); send({ t:'mouse', b:btn, a:'down' }); };
    const rel = () => { if (!b.classList.contains('held')) return; b.classList.remove('held'); send({ t:'mouse', b:btn, a:'up' }); };
    b.addEventListener('pointerdown', down); b.addEventListener('pointerup', rel);
    b.addEventListener('pointerleave', rel); b.addEventListener('pointercancel', rel);
  });
  // Molette (flèches) : impulsion + répétition au maintien
  document.querySelectorAll('.mbtn[data-mw]').forEach((b) => {
    const w = +b.dataset.mw; let iv = null;
    const start = (e) => { e.preventDefault(); send({ t:'mouse', w }); iv = setInterval(() => send({ t:'mouse', w }), 140); };
    const stop = () => { if (iv) { clearInterval(iv); iv = null; } };
    b.addEventListener('pointerdown', start); b.addEventListener('pointerup', stop);
    b.addEventListener('pointerleave', stop); b.addEventListener('pointercancel', stop);
  });
  // Popup paramètres
  $('#btnMouseSettings').addEventListener('click', () => $('#mouseSettings').classList.remove('hidden'));
  $('#btnMouseClose').addEventListener('click', () => $('#mouseSettings').classList.add('hidden'));
  $('#mouseSettings').addEventListener('click', (e) => { if (e.target.id === 'mouseSettings') $('#mouseSettings').classList.add('hidden'); });
  $('#mouseSens').addEventListener('input', () => $('#sensVal').textContent = $('#mouseSens').value);
}

// ===========================================================================
//  Saisie texte au clavier physique / OS (envoi en direct par diff)
// ===========================================================================
function initTextPass() {
  const ta = $('#passText'); let oldVal = '';
  const live = () => $('#liveToggle').checked;
  // Fenêtre de saisie (ouverte par le bouton « Texte » de l'onglet Souris).
  // Focus immédiat sur le textarea → fait apparaître le clavier du smartphone.
  const openText = () => {
    $('#textInput').classList.remove('hidden');
    ta.focus(); ta.setSelectionRange(ta.value.length, ta.value.length);
  };
  const closeText = () => $('#textInput').classList.add('hidden');
  $('#btnTextInput').addEventListener('click', openText);
  $('#btnTextClose').addEventListener('click', closeText);
  $('#textInput').addEventListener('click', (e) => { if (e.target.id === 'textInput') closeText(); });
  $('#liveToggle').addEventListener('change', () => { oldVal = ta.value; });
  ta.addEventListener('input', () => {
    if (!live()) return;
    const nv = ta.value; let p = 0; const min = Math.min(oldVal.length, nv.length);
    while (p < min && oldVal[p] === nv[p]) p++;
    const removed = oldVal.length - p, added = nv.slice(p);
    if (removed > 0) for (let i = 0; i < removed; i++) send({ t:'key', c:'Backspace', a:'tap' });
    if (added) send({ t:'txt', v:added }, true);
    oldVal = nv;
  });
  $('#btnPassClear').addEventListener('click', () => { ta.value = ''; oldVal = ''; ta.focus(); });
  $('#btnPassSend').addEventListener('click', () => {
    if (!ta.value) { toast('Vide'); return; }
    send({ t:'txt', v:ta.value }, true); oldVal = ta.value;
  });
}

// ===========================================================================
//  Séquences
// ===========================================================================
let steps = [];
function renderSteps() {
  const box = $('#stepList'); box.innerHTML = '';
  if (!steps.length) { box.innerHTML = '<div class="empty">Aucune étape. Ajoutez une frappe, une attente ou une répétition.</div>'; return; }
  steps.forEach((st, i) => box.appendChild(renderStep(st, i)));
}
function renderStep(st, i) {
  const el = document.createElement('div'); el.className = 'step'; let inner = '';
  if (st.kind === 'tap')  inner = `<span class="tag">Frappe</span><input data-i="${i}" data-f="tap" value="${esc(st.tap)}" placeholder="a, 1, Enter, F5…">`;
  if (st.kind === 'wait') inner = `<span class="tag">Attente</span><input data-i="${i}" data-f="wait" type="number" value="${st.wait}"> ms`;
  if (st.kind === 'rep')  inner = `<span class="tag">Répéter</span><input data-i="${i}" data-f="rep" type="number" value="${st.rep}"> ×
     <input data-i="${i}" data-f="every" type="number" value="${st.every}"> ms
     <input data-i="${i}" data-f="tap" value="${esc(st.tap)}" placeholder="touche">`;
  inner += `<span class="del" data-del="${i}">✕</span>`; el.innerHTML = inner; return el;
}
function addStep(kind) {
  if (kind === 'tap')  steps.push({ kind:'tap', tap:'a' });
  if (kind === 'wait') steps.push({ kind:'wait', wait:1000 });
  if (kind === 'rep')  steps.push({ kind:'rep', rep:3, every:1000, tap:'a' });
  renderSteps();
}
function presetDemo() {
  steps = [ {kind:'tap',tap:'1'},{kind:'wait',wait:1000},{kind:'tap',tap:'2'},
            {kind:'wait',wait:1000},{kind:'rep',rep:3,every:4000,tap:'3'} ];
  renderSteps();
}
function stepsToJson() {
  return steps.map((st) => st.kind === 'tap' ? { tap:st.tap }
    : st.kind === 'wait' ? { wait:Number(st.wait) }
    : { rep:Number(st.rep), every:Number(st.every), tap:st.tap });
}
function runSeq() { if (!steps.length) { toast('Séquence vide'); return; } send({ t:'seq', s:stepsToJson() }, true); }
function stopSeq() { send({ t:'seq', n:'stop' }, true); }

// ===========================================================================
//  Texte / macro rapide — mémorisation depuis la popup clavier (onglet Souris).
//  💾 enregistre le texte courant ; les chips le rechargent dans la zone de saisie.
// ===========================================================================
const savedMacros = [];
function saveMacro() {
  const t = $('#passText').value.trim();
  if (!t) { toast('Texte vide'); return; }
  savedMacros.push(t); renderMacros(); toast('Mémorisé');
}
function renderMacros() {
  const box = $('#macroList'); box.innerHTML = '';
  savedMacros.forEach((tx) => {
    const c2 = document.createElement('div'); c2.className = 'chip'; c2.textContent = tx; c2.title = tx;
    c2.addEventListener('click', () => { const ta = $('#passText'); ta.value = tx; ta.focus(); });
    box.appendChild(c2);
  });
}

// ===========================================================================
//  Réglages (cfg) — paramètres persistants du module, visibilité des onglets
// ===========================================================================
let cfg = null;                       // dernière config reçue (cfg:{…})
const ROLE_NAMES = ['standard', 'maître', 'esclave'];
// Onglet -> flag qui le rend visible. Réglages est toujours visible.
const TAB_FLAGS = { azerty:'hid_kb', num:'hid_kb', fn:'hid_kb',
                    mouse:'hid_ms', gpio:'gpio' };
function onCfg(rest) {
  if (rest === 'saved') { toast('Enregistré — le module redémarre, reconnectez'); return; }
  try { cfg = JSON.parse(rest); } catch (e) { logLine('err', 'cfg illisible : ' + e.message); return; }
  renderCfg(); applyFlags(cfg);
}
function applyFlags(c) {
  for (const [tab, flag] of Object.entries(TAB_FLAGS)) {
    const on = !!c[flag];
    const t = document.querySelector(`.tab[data-tab="${tab}"]`), p = $('#tab-' + tab);
    if (t) t.classList.toggle('hidden', !on);
    if (p) p.classList.toggle('hidden', !on);
  }
  const active = document.querySelector('.tab.active');
  if (active && active.classList.contains('hidden')) {
    const first = document.querySelector('.tab:not(.hidden)');
    if (first) switchTab(first.dataset.tab); else toggleCfg();
  }
  $('#pairBox').classList.toggle('hidden', !c.pair);
  // Bouton « Texte » (dans l'onglet Souris) : c'est une fonction clavier.
  $('#btnTextInput').classList.toggle('hidden', !c.hid_kb);
}
function renderCfg() {
  document.querySelectorAll('input[data-flag]').forEach((i) => { i.checked = !!cfg[i.dataset.flag]; });
  $('#cfgMac').textContent = cfg.mac || '–';
  $('#cfgRole').textContent = ROLE_NAMES[cfg.role] || cfg.role;
  $('#cfgPeer').textContent = cfg.peer || '–';
  const master = cfg.role === 1;
  $('#scanBox').classList.toggle('hidden', master);
  $('#linkBox').classList.toggle('hidden', !master);
  if (master) { setLinkState(!!cfg.link); if (cfg.rssi) $('#linkRssi').textContent = 'RSSI ' + cfg.rssi + ' dBm'; }
}
function saveCfg() {
  const o = { t:'cfg', a:'set' };
  document.querySelectorAll('input[data-flag]').forEach((i) => { o[i.dataset.flag] = i.checked; });
  send(o, true);
}

// ===========================================================================
//  Appairage BLE ↔ BLE (scan / bind / unbind) + état du lien maître→esclave
// ===========================================================================
function onScan(rest) {
  const list = $('#scanList');
  if (rest === 'done') {
    $('#btnScan').disabled = false;
    if (!list.children.length) list.innerHTML = '<div class="empty">Aucun module trouvé (cible libre et sous tension ?).</div>';
    return;
  }
  // scan:<mac>:<rssi>:<name> — la MAC contient des ':' mais fait toujours 17 caractères.
  const mac = rest.slice(0, 17), r2 = rest.slice(18), j = r2.indexOf(':');
  const rssi = j > 0 ? r2.slice(0, j) : r2, name = j > 0 ? r2.slice(j + 1) : '';
  const el = document.createElement('div'); el.className = 'scanitem';
  el.innerHTML = `<b>${esc(name || '?')}</b><span class="mac">${esc(mac)}</span><span class="rssi">${esc(rssi)} dBm</span>`;
  const b = document.createElement('button'); b.className = 'btn primary'; b.textContent = 'Appairer';
  b.addEventListener('click', () => {
    if (!confirm('Faire de ' + (name || mac) + ' l\'ESCLAVE de ce module ?\nLes deux modules redémarrent.')) return;
    b.disabled = true; send({ t:'pair', a:'bind', mac }, true);
  });
  el.appendChild(b); list.appendChild(el);
}
function startScan() {
  $('#scanList').innerHTML = ''; $('#btnScan').disabled = true;
  send({ t:'pair', a:'scan' }, true);
  setTimeout(() => { $('#btnScan').disabled = false; }, 8000);   // filet si scan:done n'arrive pas
}
function onPair(rest) {
  if (rest === 'ok') toast('Appairage : OK — redémarrage'); else toast('Appairage : ' + rest);
}
function setLinkState(up) {
  const el = $('#linkState'); el.textContent = up ? 'connecté' : 'coupé';
  el.classList.toggle('on', up); el.classList.toggle('off', !up);
}
function onLink(rest) {
  if (rest === 'up') { setLinkState(true); return; }
  if (rest === 'down') { setLinkState(false); $('#linkRssi').textContent = 'RSSI –'; return; }
  if (rest.startsWith('rssi:')) { $('#linkRssi').textContent = 'RSSI ' + rest.slice(5) + ' dBm'; }
}
function unbind() {
  if (!confirm('Désappairer ? Les deux modules reviennent en mode standard et redémarrent.')) return;
  send({ t:'pair', a:'unbind' }, true);
}

// ===========================================================================
//  Test de liaison : pings numérotés, RTT min/moy/max, pertes
// ===========================================================================
const pendingPings = new Map();       // n -> { t0, resolve, timer }
let pingSeq = 0;
function ping(timeoutMs = 1500) {
  const n = ++pingSeq;
  return new Promise((resolve) => {
    const t0 = performance.now();
    const timer = setTimeout(() => { pendingPings.delete(n); resolve(null); }, timeoutMs);
    pendingPings.set(n, { t0, resolve, timer });
    send({ t:'ping', n });
  });
}
function onPong(rest) {
  const p = pendingPings.get(+rest); if (!p) return;
  clearTimeout(p.timer); pendingPings.delete(+rest);
  p.resolve(performance.now() - p.t0);
}
async function linkTest(N = 20, gapMs = 100) {
  const out = $('#linkTestOut'), btn = $('#btnLinkTest');
  if (!activeTransport || !activeTransport.connected) { toast('Non connecté'); return; }
  btn.disabled = true; const rtts = [];
  for (let i = 0; i < N; i++) {
    const r = await ping(); if (r !== null) rtts.push(r);
    out.textContent = `ping ${i + 1}/${N} — ${r === null ? 'perdu' : r.toFixed(0) + ' ms'}`;
    await new Promise((res) => setTimeout(res, gapMs));
  }
  btn.disabled = false;
  if (!rtts.length) { out.textContent = `${N} pings, 100 % perdus`; return; }
  const min = Math.min(...rtts), max = Math.max(...rtts), avg = rtts.reduce((a, b) => a + b, 0) / rtts.length;
  const rssi = $('#linkRssi').textContent;
  out.textContent = `${rtts.length}/${N} reçus · RTT min ${min.toFixed(0)} / moy ${avg.toFixed(0)} / max ${max.toFixed(0)} ms` +
    ` · pertes ${(100 * (N - rtts.length) / N).toFixed(0)} %` + (cfg && cfg.role === 1 ? ' · ' + rssi : '');
  logLine('in', 'TEST LIAISON ' + out.textContent);
}

// ===========================================================================
//  GPIO — sorties (bascule) et entrées (voyants). Libellés = table du firmware
//  (gpio_panel.h) : BOOT + sorties 4..7 + entrées 8..11.
// ===========================================================================
const GPIO_OUT = ['4', '5', '6', '7'];
const GPIO_IN  = ['BOOT', '8', '9', '10', '11'];
function buildGpio() {
  const out = $('#gpioOut'), inn = $('#gpioIn'); out.innerHTML = ''; inn.innerHTML = '';
  for (const p of GPIO_OUT) {
    const el = document.createElement('div'); el.className = 'key nav gpio'; el.dataset.gpio = p; el.textContent = p;
    el.addEventListener('click', () => { flash(el); send({ t:'gpio', p, a:'tgl' }); });
    out.appendChild(el);
  }
  for (const p of GPIO_IN) {
    const el = document.createElement('span'); el.className = 'chip led'; el.dataset.gpio = p; el.textContent = p;
    inn.appendChild(el);
  }
}
function onGpio(rest) {
  const j = rest.lastIndexOf(':'); if (j < 0) return;
  const label = rest.slice(0, j), v = rest.slice(j + 1) === '1';
  document.querySelectorAll(`[data-gpio="${label}"]`).forEach((el) => el.classList.toggle('on', v));
}
function readGpio() { send({ t:'gpio', a:'read' }); }

// ===========================================================================
//  Journal / toast / util
// ===========================================================================
function esc(s) { return String(s).replace(/[&<>"]/g, (c2) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c2])); }
function two(n) { return n < 10 ? '0' + n : '' + n; }
function logLine(kind, text) {
  const n = new Date(); const ts = `${two(n.getHours())}:${two(n.getMinutes())}:${two(n.getSeconds())}`;
  const div = document.createElement('div'); div.className = 'line';
  const tag = kind === 'out' ? '→' : kind === 'in' ? '←' : '⚠';
  div.innerHTML = `<span class="t">${ts}</span> <span class="${kind}">${tag} ${esc(text)}</span>`;
  const box = $('#log'); box.appendChild(div); box.scrollTop = box.scrollHeight;
}
let toastTimer = null;
function toast(msg) {
  const el = $('#toast'); el.textContent = msg; el.classList.remove('hidden');
  clearTimeout(toastTimer); toastTimer = setTimeout(() => el.classList.add('hidden'), 1800);
}
function flash(el) { el.classList.add('flash'); setTimeout(() => el.classList.remove('flash'), 90); }

// ===========================================================================
//  Onglets, plein écran, détection téléphone / orientation
// ===========================================================================
function switchTab(name) {
  document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.dataset.tab === name));
  document.querySelectorAll('.tabpage').forEach((p) => p.classList.toggle('active', p.id === 'tab-' + name));
  $('#btnCfg').classList.toggle('active', name === 'cfg');   // Réglages = icône de l'en-tête, pas un onglet
  if (name === 'gpio' && activeTransport && activeTransport.connected) readGpio();   // état réel à l'ouverture
}
// Bascule Réglages <-> dernier onglet visité.
let lastTab = 'mouse';
function toggleCfg() {
  const cur = document.querySelector('.tab.active');
  if (cur) { lastTab = cur.dataset.tab; switchTab('cfg'); return; }
  const back = document.querySelector(`.tab[data-tab="${lastTab}"]:not(.hidden)`) || document.querySelector('.tab:not(.hidden)');
  switchTab(back ? back.dataset.tab : 'cfg');
}
function updateEnv() {
  const coarse = matchMedia('(pointer:coarse)').matches;
  const landscape = matchMedia('(orientation:landscape)').matches;
  const isPhone = coarse && Math.min(screen.width, screen.height) <= 560;
  document.body.classList.toggle('is-phone', isPhone);
  document.body.classList.toggle('landscape', landscape);
  document.body.classList.toggle('portrait', !landscape);
  $('#rotateHint').classList.toggle('hidden', !(isPhone && !landscape));
  relocateTabs(isPhone && landscape);   // paysage mobile : onglets dans le header
}
// Déplace la barre d'onglets dans le header (paysage mobile) ou la remet sous le header.
function relocateTabs(inHeader) {
  const tabs = $('#tabs'), header = $('.topbar'), content = $('.content');
  if (inHeader && tabs.parentElement !== header) {
    header.insertBefore(tabs, header.querySelector('.spacer'));
    document.body.classList.add('tabs-in-header');
  } else if (!inHeader && tabs.parentElement === header) {
    content.before(tabs);
    document.body.classList.remove('tabs-in-header');
  }
}
async function toggleFullscreen() {
  try {
    if (!document.fullscreenElement) {
      await document.documentElement.requestFullscreen();
      if (screen.orientation && screen.orientation.lock) screen.orientation.lock('landscape').catch(() => {});
    } else { await document.exitFullscreen(); }
  } catch (e) { toast('Plein écran indisponible'); }
}

// ===========================================================================
//  Câblage
// ===========================================================================
function wireUI() {
  $('#btnConn').addEventListener('click', onConnClick);
  $('#btnFull').addEventListener('click', toggleFullscreen);
  $('#btnCfg').addEventListener('click', toggleCfg);
  $('#btnLog').addEventListener('click', () => $('#logDrawer').classList.toggle('hidden'));
  $('#btnLogClose').addEventListener('click', () => $('#logDrawer').classList.add('hidden'));
  $('#btnLogClear').addEventListener('click', () => { $('#log').innerHTML = ''; });

  document.querySelectorAll('.tab').forEach((tab) => tab.addEventListener('click', () => switchTab(tab.dataset.tab)));

  $('#btnMacroSave').addEventListener('click', saveMacro);

  document.querySelectorAll('[data-add]').forEach((b) => b.addEventListener('click', () => addStep(b.dataset.add)));
  $('#btnSeqPreset').addEventListener('click', presetDemo);
  $('#btnSeqRun').addEventListener('click', runSeq);
  $('#btnSeqStop').addEventListener('click', stopSeq);
  $('#stepList').addEventListener('input', (e) => {
    const inp = e.target.closest('input'); if (!inp) return;
    const i = +inp.dataset.i, f = inp.dataset.f;
    steps[i][f] = inp.type === 'number' ? Number(inp.value) : inp.value;
  });
  $('#stepList').addEventListener('click', (e) => {
    const d = e.target.closest('[data-del]'); if (!d) return;
    steps.splice(+d.dataset.del, 1); renderSteps();
  });

  // Réglages / appairage / GPIO
  $('#btnCfgSave').addEventListener('click', saveCfg);
  $('#btnScan').addEventListener('click', startScan);
  $('#btnUnbind').addEventListener('click', unbind);
  $('#btnLinkTest').addEventListener('click', () => linkTest());
  $('#btnGpioRead').addEventListener('click', readGpio);

  matchMedia('(orientation:landscape)').addEventListener('change', updateEnv);
  window.addEventListener('resize', updateEnv);
}

// ===========================================================================
//  Sélection du transport au chargement (param D → Wi-Fi, sinon BLE)
// ===========================================================================
function b64urlDecode(s) {
  s = String(s).replace(/-/g, '+').replace(/_/g, '/');
  while (s.length % 4) s += '=';
  const bin = atob(s);
  const bytes = Uint8Array.from(bin, (ch) => ch.charCodeAt(0));
  return new TextDecoder().decode(bytes);
}
function readParamD() {
  try {
    const raw = new URLSearchParams(location.search).get('D');
    if (!raw) return null;
    const obj = JSON.parse(b64urlDecode(raw));
    logLine('in', 'param D décodé : ' + JSON.stringify(obj));
    return obj;
  } catch (e) { logLine('err', 'param D illisible : ' + e.message); return null; }
}
function selectTransport() {
  const D = readParamD();
  if (D && D.wifi) {                                  // résolution A : app servie en HTTP par le S3
    activeTransport = wifiTransport;
    wifiTransport.ip = D.wifi;
    if (D.wsport) WIFI_WS_PORT = +D.wsport;
    $('#unsupported').classList.add('hidden');        // Web Bluetooth absent en HTTP : sans objet ici
    logLine('in', 'Transport = Wi-Fi → ws://' + D.wifi + ':' + WIFI_WS_PORT + '/');
    wifiConnect(D.wifi);                              // WebSocket : pas de geste utilisateur requis
    return;
  }
  activeTransport = bleTransport;                     // mode BLE (existant)
  logLine('in', 'Transport = BLE. Web Bluetooth: ' + (navigator.bluetooth ? 'présent' : 'ABSENT — Chrome/Edge + https/localhost requis'));
  if (!navigator.bluetooth) $('#unsupported').classList.remove('hidden');
  else if (navigator.bluetooth.getAvailability)
    navigator.bluetooth.getAvailability().then((a) => logLine('in', 'Adaptateur Bluetooth actif: ' + (a ? 'oui' : 'NON — activez le Bluetooth Windows')));
}

// ===========================================================================
//  Démarrage
// ===========================================================================
function init() {
  window.addEventListener('error', (e) => logLine('err', 'JS: ' + e.message + ' @' + (e.filename || '?') + ':' + (e.lineno || '?')));
  window.addEventListener('unhandledrejection', (e) => logLine('err', 'Promesse rejetée: ' + ((e.reason && e.reason.message) ? e.reason.message : e.reason)));

  buildKeyboard('#kb-azerty', KB_AZERTY);
  buildKeyboard('#kb-num', KB_NUM);
  buildFn();
  buildGpio();
  renderSteps();
  refreshMods();
  initMouse();
  initTextPass();
  wireUI();
  updateEnv();

  logLine('in', '=== app.js v6 (BLE + Wi-Fi, réglages, appairage, GPIO) chargé ===');
  logLine('in', 'Page: ' + location.protocol + '//' + location.host + '  (sécurisé=' + window.isSecureContext + ')');
  selectTransport();
  logLine('in', 'prêt.');
}
document.addEventListener('DOMContentLoaded', init);
