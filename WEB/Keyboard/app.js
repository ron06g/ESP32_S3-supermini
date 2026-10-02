// ===========================================================================
//  S3-KBD — client Web Bluetooth. Le web envoie l'INTENTION (char/key/…),
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
// id = carte cible pour le ROUTAGE (0 = maître/local, 1..3 = esclave). Ajouté
// seulement si non nul : commandes rétrocompatibles et trames plus courtes.
// (Les commandes « pair » portent leur propre champ id dans obj, pas via ici.)
async function send(obj, reliable=false, id=0) {
  if (id) obj = { ...obj, id };
  const json = JSON.stringify(obj);
  logLine('out', json);
  if (!activeTransport || !activeTransport.connected) { toast('Non connecté'); return; }
  try { await activeTransport.send(json, reliable); }
  catch (e) { logLine('err', 'écriture: ' + e.message); }
}

// Flux de coordonnées souris (déplacements dx/dy) : voie RAPIDE dédiée.
// - aucun acquittement (écriture BLE sans réponse / WS non bloquant) ;
// - fire-and-forget (pas d'await) → pas de sérialisation des écritures ;
// - PAS de journalisation (chaque frappe DOM du journal saccadait le pointeur).
// Réservé au déplacement continu ; clics/molette/boutons passent par send() (journalisés).
function sendMouseMove(dx, dy) {
  const t = activeTransport;
  if (!t || !t.connected || !t.sendFast) return;
  try { t.sendFast(JSON.stringify({ t:'mouse', dx, dy })); } catch (e) {}
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
  cmdChar = statusChar = gatt = null; lastStatus = ''; cfg = null;
  setCfgLocked(true);          // plus de config live : Réglages en lecture seule
  setConn('off'); logLine('err', 'déconnecté');
}
// Statut unifié : appelé par le BLE (après décodage DataView) et par le Wi-Fi (frame texte).
// Nouveau contrat : chaque frame est un objet JSON {"id":n, ...} :
//   {"id":0,"st":"ready|busy"}          état (seul l'id 0 pilote le bouton)
//   {"id":n,"err":"<code>"}             erreur
//   {"id":n,"ev":"cfg|scan|pair|link|gpio|pong", ...}   événements
// L'id indique la carte d'origine (0 = maître, 1..3 = esclave).
function handleStatus(text) {
  logLine('in', 'STATUS ' + text);
  let m;
  try { m = JSON.parse(text); } catch (e) { return; }   // frame non-JSON : ignorée
  if (m.ev) {
    switch (m.ev) {
      case 'cfg':  onCfg(m);  return;
      case 'scan': onScan(m); return;
      case 'pair': onPair(m); return;
      case 'link': onLink(m); return;
      case 'gpio': onGpio(m); return;
      case 'pong': onPong(m); return;
      case 'sec':  onSec(m);  return;
      case 'name': onName(m); return;
    }
    return;
  }
  if (m.err !== undefined) {
    lastStatus = 'err:' + m.err + (m.id ? ' (id ' + m.id + ')' : '');
    if (activeTransport && activeTransport.connected) renderConn();
    return;
  }
  if (m.st !== undefined && !m.id) {          // état courant du maître/local
    lastStatus = m.st;
    if (activeTransport && activeTransport.connected) renderConn();
  }
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
  // Voie RAPIDE pour le flux de coordonnées souris : écriture SANS RÉPONSE (aucun
  // acquittement ATT) et SANS await → pas de sérialisation ni de latence. Jamais
  // de repli sur writeValue (acquitté) qui saccaderait le pointeur.
  sendFast(json) {
    if (!cmdChar || !cmdChar.writeValueWithoutResponse) return;
    cmdChar.writeValueWithoutResponse(new TextEncoder().encode(json)).catch(() => {});
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
  // Voie rapide souris : même canal (le WebSocket n'acquitte pas au niveau appli).
  sendFast(json) { if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.send(json); },
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
  ws.onclose   = () => { wifiTransport.ws = null; lastStatus = ''; cfg = null; setCfgLocked(true); setConn('off'); logLine('err', 'déconnecté (Wi-Fi)'); };
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

// Onglet Bios — pilotage du démarrage (BIOS/UEFI, menu de boot, GRUB, Windows).
// Pavés en grille CSS (cols × rows) ; span 'c2' = 2 colonnes, 'r2' = 2 rangées.
// rep = répétition au maintien. Un BIOS lit en QWERTY US : +/− passent par le pavé
// numérique (NumpadAdd/NumpadSubtract) ; y, n, e, c, x sont au même endroit en AZERTY.
const kTap = (code, mask) => mask ? { t:'key', c:code, a:'tap', m:mask } : { t:'key', c:code, a:'tap' };
const BIOS_PADS = [
  { id:'bios-fn', cols:4, rows:4, keys: [
    ...Array.from({length:12}, (_,i) => ({ label:'F'+(i+1), cmd:kTap('F'+(i+1)) })),
    { label:'Suppr', cmd:kTap('Delete'), span:'c2' },
    { label:'Ctrl+Alt+Suppr', cmd:{t:'seq',n:'ctrl_alt_del'}, span:'c2', cls:'combo danger', reliable:true } ] },
  { id:'bios-tools', cols:2, rows:4, keys: [
    { label:'Tab', cmd:kTap('Tab') }, { label:'Maj+F10', cmd:kTap('F10', MOD.shift), cls:'combo', reliable:true },
    { label:'Y', cmd:{t:'char',v:'y'} }, { label:'N', cmd:{t:'char',v:'n'} },
    { label:'e', cmd:{t:'char',v:'e'} }, { label:'c', cmd:{t:'char',v:'c'} },
    { label:'Ctrl+X', cmd:{t:'char',v:'x',m:MOD.ctrl}, cls:'combo', reliable:true }, { label:'Pause', cmd:kTap('Pause'), cls:'combo' } ] },
  { id:'bios-nav', cols:4, rows:4, keys: [
    { label:'Pg↑', cmd:kTap('PageUp'), rep:true }, { label:'↑', cmd:kTap('ArrowUp'), rep:true, cls:'big' },
    { label:'Pg↓', cmd:kTap('PageDown'), rep:true }, { label:'+', cmd:kTap('NumpadAdd'), rep:true, span:'r2', cls:'big' },
    { label:'←', cmd:kTap('ArrowLeft'), rep:true, cls:'big' }, { label:'Entrée', cmd:kTap('Enter'), cls:'enter' },
    { label:'→', cmd:kTap('ArrowRight'), rep:true, cls:'big' },
    { label:'Début', cmd:kTap('Home') }, { label:'↓', cmd:kTap('ArrowDown'), rep:true, cls:'big' },
    { label:'Fin', cmd:kTap('End') }, { label:'−', cmd:kTap('NumpadSubtract'), rep:true, span:'r2', cls:'big' },
    { label:'Échap', cmd:kTap('Escape') }, { label:'Espace', cmd:kTap('Space') },
    { label:'⌫', cmd:kTap('Backspace'), rep:true, cls:'big' } ] },
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
// Touches qui se répètent au maintien : tous les caractères + ces touches nommées.
// Jamais les modificateurs, les bascules (Verr. Maj, Muet, Lecture…) ni les touches
// à effet unique (Échap, F1–F12, Début/Fin, Impr…).
const REPEAT_KEYS = new Set(['Backspace', 'Delete', 'Enter', 'Space', 'Tab',
  'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'PageUp', 'PageDown',
  'MediaVolumeUp', 'MediaVolumeDown']);
// deferred : la touche est dans une page qui défile (Fn/Média), cf. bindRepeat.
function makeKey(d, deferred = false) {
  if (d.t === 'br') { const b = document.createElement('div'); b.className = 'break'; return b; }
  const el = document.createElement('div');
  if (d.t === 'char') {
    el.className = 'key charkey'; el.innerHTML = keyLabelChar(d);
    bindKey(el, () => charCmd(d), true, deferred);
  } else if (d.t === 'key') {
    el.className = 'key ' + (d.cls || 'fn'); el.textContent = d.label;
    bindKey(el, () => namedCmd(d), REPEAT_KEYS.has(d.code), deferred);
  } else if (d.t === 'mod') {
    el.className = 'key mod fn'; el.dataset.mod = d.mod;
    el.innerHTML = `${d.label}<span class="lock"></span>`;
    el.addEventListener('click', () => { feedback(); cycleMod(d.mod); });
  }
  return el;
}
// La commande (caractère + modificateurs collants) est calculée à la 1re frappe de
// l'appui puis renvoyée telle quelle par la répétition : Maj one-shot + maintien
// de « a » donne « AAAA », pas « Aaaa ».
function bindKey(el, build, repeat, deferred) {
  let cmd = null;
  const fire = (first) => { if (first || !cmd) cmd = build(); send(cmd, false); };
  if (repeat) bindRepeat(el, fire, deferred);
  else el.addEventListener('click', () => { flash(el); fire(true); });
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
      for (const d of g.keys) cl.appendChild(makeKey(d, true));   // page qui défile
    }
    grp.appendChild(cl); box.appendChild(grp);
  }
}
// Onglet Bios : modificateurs collants ignorés (un Ctrl armé ailleurs ne doit pas
// s'appliquer en cachette) — les combinaisons sont explicites dans BIOS_PADS.
function buildBios() {
  const box = $('#bios'); box.innerHTML = '';
  for (const p of BIOS_PADS) {
    const pad = document.createElement('div'); pad.className = 'biospad'; pad.id = p.id;
    pad.style.setProperty('--cols', p.cols); pad.style.setProperty('--rows', p.rows);
    for (const d of p.keys) {
      const el = document.createElement('div');
      el.className = ['key', 'bioskey', d.span, d.cls].filter(Boolean).join(' ');
      el.textContent = d.label;
      const fire = () => send(d.cmd, !!d.reliable);
      if (d.rep) bindRepeat(el, fire);
      else el.addEventListener('click', () => { flash(el); fire(); });
      pad.appendChild(el);
    }
    box.appendChild(pad);
  }
}
// Répétition au maintien : 1 frappe, puis une toutes les REP_EVERY_MS après
// REP_DELAY_MS. Des taps successifs (jamais down/up) : aucune touche ne peut rester
// « collée » si un relâchement se perd. fire(true) = 1re frappe de l'appui.
// deferred (page qui défile, Fn/Média) : rien ne part à l'appui — un appui court
// frappe au relâchement, un maintien immobile lance la répétition, un glissement
// (défilement de la page) annule. Sinon la 1re frappe part dès l'appui.
const REP_DELAY_MS = 400, REP_EVERY_MS = 120;
function bindRepeat(el, fire, deferred = false) {
  let t = null, iv = null, pending = false, sx = 0, sy = 0;
  const stop = () => { clearTimeout(t); clearInterval(iv); t = iv = null; pending = false; el.classList.remove('held'); };
  const first = () => { pending = false; flash(el); fire(true); };
  const repeat = () => { iv = setInterval(() => fire(false), REP_EVERY_MS); };
  el.addEventListener('pointerdown', (e) => {
    if (e.button) return;                          // bouton principal / doigt seulement
    stop(); el.classList.add('held'); sx = e.clientX; sy = e.clientY;
    if (deferred) { pending = true; t = setTimeout(() => { first(); repeat(); }, REP_DELAY_MS); }
    else { e.preventDefault(); first(); t = setTimeout(repeat, REP_DELAY_MS); }
  });
  el.addEventListener('pointermove', (e) => {
    if (pending && Math.hypot(e.clientX - sx, e.clientY - sy) > 10) stop();   // défilement
  });
  el.addEventListener('pointerup', () => { if (pending) first(); stop(); });
  el.addEventListener('pointerleave', stop); el.addEventListener('pointercancel', stop);
  el.addEventListener('contextmenu', (e) => e.preventDefault());   // pas de menu d'appui long
}

// ===========================================================================
//  Actions clavier (mode direct) : commande selon les modificateurs collants,
//  les one-shot sont consommés.
// ===========================================================================
function charCmd(d) {
  let ch = d.base;
  if (mods.altgr && d.altgr) ch = d.altgr; else if (mods.shift && d.shift) ch = d.shift;
  const mask = currentMask(false);
  consumeMods();
  return mask ? { t:'char', v:ch, m:mask } : { t:'char', v:ch };
}
function namedCmd(d) {
  if (d.code.startsWith('Media')) return { t:'key', c:d.code, a:'tap' };
  const mask = currentMask(true);
  consumeMods();
  const cmd = { t:'key', c:d.code, a:'tap' }; if (mask) cmd.m = mask;
  return cmd;
}

// ===========================================================================
//  Souris — boutons à maintien (drag) + molette répétée + réglage sensibilité
// ===========================================================================
function initMouse() {
  const pad = $('#trackpad');
  let last = null, accX = 0, accY = 0, timer = null, downT = 0, moved = 0;
  const sens = () => (+$('#mouseSens').value || 15) / 8;
  const flush = () => {
    if (accX || accY) {
      // Repli rotation CSS (paysage logiciel) : le trackpad est pivoté de 90°
      // (transform matrix rotate(90°) → local (u,v) affiché en (-v,u)), mais
      // clientX/clientY restent dans le repère PHYSIQUE de l'écran. On repasse
      // donc les deltas dans le repère perçu : dx = +dYphys, dy = -dXphys.
      const dx = cssRotated ? accY : accX;
      const dy = cssRotated ? -accX : accY;
      sendMouseMove(Math.round(dx), Math.round(dy));   // voie rapide : pas d'ACK, pas de journal
      accX = accY = 0;
    }
    timer = null;
  };
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
    const down = (e) => { e.preventDefault(); feedback(); b.classList.add('held'); send({ t:'mouse', b:btn, a:'down' }); };
    const rel = () => { if (!b.classList.contains('held')) return; b.classList.remove('held'); send({ t:'mouse', b:btn, a:'up' }); };
    b.addEventListener('pointerdown', down); b.addEventListener('pointerup', rel);
    b.addEventListener('pointerleave', rel); b.addEventListener('pointercancel', rel);
  });
  // Molette (flèches) : impulsion + répétition au maintien
  document.querySelectorAll('.mbtn[data-mw]').forEach((b) => {
    const w = +b.dataset.mw; let iv = null;
    const start = (e) => { e.preventDefault(); feedback(); send({ t:'mouse', w }); iv = setInterval(() => send({ t:'mouse', w }), 140); };
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
function stopSeq() { send({ t:'stop' }, true); }   // arrêt hors-file (reconnu exactement par le firmware)

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
const TAB_FLAGS = { azerty:'hid_kb', num:'hid_kb', fn:'hid_kb', bios:'hid_kb',
                    mouse:'hid_ms', gpio:'gpio' };
function onCfg(m) {
  if (m.saved) { toast('Enregistré — le module redémarre, reconnectez'); return; }
  cfg = m;
  setCfgLocked(false);          // config live reçue : le panneau redevient actif
  renderCfg(); applyFlags(cfg);
}
// ---------------------------------------------------------------------------
//  Cohérence hors connexion : sans config live (déconnecté), le panneau Réglages
//  ne reflète RIEN de réel (cases décochées, boutons inertes). On le verrouille
//  donc en lecture seule — contrôles désactivés + bandeau. Il se déverrouille à
//  la réception du cfg (onCfg).
// ---------------------------------------------------------------------------
let cfgLocked = true;
function setCfgLocked(locked) {
  cfgLocked = locked;
  const sec = $('#tab-cfg'); if (!sec) return;
  sec.classList.toggle('locked', locked);
  // .local = préférences de cet appareil (pas du module) : jamais verrouillées.
  sec.querySelectorAll('.cfgpage:not(.local) input, .cfgpage:not(.local) select, .cfgpage:not(.local) button')
     .forEach((el) => { el.disabled = locked; });
  updateCfgOffline();
}
// Bandeau « hors connexion » : visible tant que le panneau est verrouillé (sauf
// sur une page .local, utilisable sans module).
function updateCfgOffline() {
  const el = $('#cfgOffline'); if (!el) return;
  el.classList.toggle('hidden', !cfgLocked || !!document.querySelector('.cfgpage.local.active'));
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
  // Sous-onglet « Appairage » des Réglages : visible seulement si le flag pair est actif.
  const pairSub = document.querySelector('.subtab[data-sub="pair"]');
  if (pairSub) {
    pairSub.classList.toggle('hidden', !c.pair);
    const pairPage = document.querySelector('.cfgpage[data-subpage="pair"]');
    if (!c.pair && pairPage && pairPage.classList.contains('active')) switchCfgSub('module');
  }
  // Bouton « Texte » (dans l'onglet Souris) : c'est une fonction clavier.
  $('#btnTextInput').classList.toggle('hidden', !c.hid_kb);
}
// Sous-onglets du panneau Réglages (évite de scroller : une section à la fois).
function switchCfgSub(name) {
  document.querySelectorAll('.subtab').forEach((t) => t.classList.toggle('active', t.dataset.sub === name));
  document.querySelectorAll('.cfgpage').forEach((p) => p.classList.toggle('active', p.dataset.subpage === name));
  updateCfgOffline();
}
const MAX_SLAVES = 3;
function renderCfg() {
  document.querySelectorAll('input[data-flag]').forEach((i) => { i.checked = !!cfg[i.dataset.flag]; });
  $('#cfgMac').textContent = cfg.mac || '–';
  $('#cfgRole').textContent = ROLE_NAMES[cfg.role] || cfg.role;
  // Modes HID actifs, en tags, sur la ligne Rôle (Clavier / Souris / COM).
  const modes = [];
  if (cfg.hid_kb) modes.push('Clavier');
  if (cfg.hid_ms) modes.push('Souris');
  if (cfg.serial) modes.push('COM');
  const mt = $('#cfgModes');
  if (mt) {
    mt.innerHTML = '';
    modes.forEach((m) => { const s = document.createElement('span'); s.className = 'modetag'; s.textContent = m; mt.appendChild(s); });
  }
  if ($('#cfgName')) $('#cfgName').value = cfg.name || '';
  const slaves = cfg.slaves || [];
  // Sécurité : on ne peut changer la passkey que sans esclave appairé (sinon il
  // faudrait tout ré-appairer). On désactive le bouton et on explicite pourquoi.
  const hasSlaves = cfg.role === 1 && slaves.length > 0;
  if ($('#btnSecPasskey')) $('#btnSecPasskey').disabled = hasSlaves;
  if ($('#secPasskeyHint')) $('#secPasskeyHint').classList.toggle('warn', hasSlaves);
  let peerTxt = '–';
  if (cfg.role === 2)      peerTxt = 'maître ' + (cfg.peer || '?') + ' — je suis id ' + (cfg.self || '?');
  else if (cfg.role === 1) peerTxt = slaves.length + ' esclave(s) sur ' + MAX_SLAVES;
  $('#cfgPeer').textContent = peerTxt;
  const slave = cfg.role === 2, full = slaves.length >= MAX_SLAVES;
  $('#scanBox').classList.toggle('hidden', slave || full);   // esclave : pas de scan ; table pleine : masque
  $('#slaveList').classList.toggle('hidden', cfg.role !== 1);
  renderSlaves();
  renderModuleRename();
  renderSec();
  buildLinkTargets();
  buildGpioModules();
}

// Onglet « Module » : ligne de renommage par carte (maître + esclaves appairés).
// Le libellé du module local devient « Module Maître » quand role=1, sinon « Module ».
function renderModuleRename() {
  const lbl = $('#lblSelfName');
  if (lbl) lbl.textContent = (cfg && cfg.role === 1) ? 'Module Maître' : 'Module';
  const box = $('#moduleSlaveList'); if (!box) return;
  box.innerHTML = '';
  const slaves = (cfg && cfg.slaves) || [];
  for (const s of slaves) {                          // rien si aucun esclave
    const row = document.createElement('div'); row.className = 'renamerow';
    const lab = document.createElement('span'); lab.className = 'renlabel';
    lab.textContent = 'Module esclave ' + s.id;
    const inp = document.createElement('input'); inp.type = 'text'; inp.maxLength = 19;
    inp.value = s.name || ''; inp.placeholder = 'esclave ' + s.id;
    const btn = document.createElement('button'); btn.className = 'btn primary'; btn.textContent = 'Renommer';
    btn.addEventListener('click', () => send({ t:'name', name: inp.value.trim().slice(0, 19) }, true, s.id));
    const st = document.createElement('span'); st.className = 'chip led ' + (s.up ? 'on' : 'off');
    st.textContent = s.up ? 'lié' : 'hs';
    row.appendChild(lab); row.appendChild(inp); row.appendChild(btn); row.appendChild(st);
    box.appendChild(row);
  }
}

// Onglet « Sécurité » : état « défaut / personnalisée » (jamais la valeur) +
// masque ••••• dans le champ quand la valeur est personnalisée. Indicateurs
// pkset/wifiset fournis par le firmware dans la réponse « cfg get ».
function renderSec() {
  const pk = !!(cfg && cfg.pkset), wf = !!(cfg && cfg.wifiset);
  const setState = (el, on, txtOn, txtOff) => {
    if (!el) return;
    el.textContent = on ? txtOn : txtOff;
    el.className = 'chip state ' + (on ? 'custom' : 'def');
  };
  setState($('#secPasskeyState'), pk, 'personnalisée', 'défaut');
  setState($('#secWifiState'),    wf, 'personnalisé',  'défaut');
  const pkIn = $('#secPasskey'); if (pkIn) pkIn.placeholder = pk ? '••••••'   : '000000';
  const wfIn = $('#secWifi');    if (wfIn) wfIn.placeholder = wf ? '••••••••' : '≥ 8 caractères';
}

// Liste des esclaves appairés (maître) : id, MAC, état/RSSI de lien, désappairage par id.
function renderSlaves() {
  const box = $('#slaveList'); if (!box) return;
  box.innerHTML = '';
  if (!cfg || cfg.role !== 1) return;
  const slaves = cfg.slaves || [];
  if (!slaves.length) { box.innerHTML = '<div class="empty">Aucun esclave appairé. Utilisez « Ajouter un esclave ».</div>'; return; }
  for (const s of slaves) {
    const el = document.createElement('div'); el.className = 'scanitem';
    const label = s.name ? esc(s.name) + ' <small>(id ' + s.id + ')</small>' : 'Esclave ' + s.id;
    el.innerHTML = `<b>${label}</b><span class="mac">${esc(s.mac)}</span>` +
      `<span class="chip led ${s.up ? 'on' : 'off'}">${s.up ? 'lié' : 'coupé'}</span>` +
      `<span class="rssi">${s.up ? esc(s.rssi) + ' dBm' : '–'}</span>`;
    const rn = document.createElement('button'); rn.className = 'btn ghost'; rn.textContent = '✏️'; rn.title = 'Renommer';
    rn.addEventListener('click', () => {
      const nm = prompt('Nom de l\'esclave ' + s.id + ' :', s.name || '');
      if (nm !== null) send({ t:'name', name: nm.trim().slice(0, 19) }, true, s.id);
    });
    const b = document.createElement('button'); b.className = 'btn danger'; b.textContent = 'Désappairer';
    b.addEventListener('click', () => {
      if (!confirm('Désappairer ' + (s.name || 'l\'esclave ' + s.id) + ' ? Il revient en mode standard (nom conservé), le maître redémarre.')) return;
      send({ t:'pair', a:'unbind', id:s.id }, true);
    });
    el.appendChild(rn); el.appendChild(b); box.appendChild(el);
  }
}
function saveCfg() {
  const o = { t:'cfg', a:'set' };
  document.querySelectorAll('input[data-flag]').forEach((i) => { o[i.dataset.flag] = i.checked; });
  // Garde-fou : au moins un canal de commande (Wi-Fi, Port COM ou BLE) doit rester
  // actif, sinon on perd tout moyen de piloter le module. Popup bloquante.
  if (!o.wifi && !o.serial && !o.ble) {
    alert('⚠️ Au moins un mode de communication doit rester actif : Wi-Fi, Port COM ou BLE.\n\n'
        + 'Sans ça, vous perdriez le contrôle du module. Réactivez-en un avant d\'enregistrer.');
    return;                               // on N'enregistre PAS
  }
  send(o, true);
}

// ===========================================================================
//  Appairage BLE ↔ BLE (scan / bind / unbind) + état du lien maître→esclave
// ===========================================================================
function onScan(m) {
  const list = $('#scanList');
  if (m.done) {
    $('#btnScan').disabled = false;
    if (!list.children.length) list.innerHTML = '<div class="empty">Aucun module trouvé (cible libre et sous tension ?).</div>';
    return;
  }
  const already = (cfg && cfg.slaves || []).some((s) => s.mac === m.mac);
  const el = document.createElement('div'); el.className = 'scanitem';
  el.innerHTML = `<b>${esc(m.name || '?')}</b><span class="mac">${esc(m.mac)}</span><span class="rssi">${esc(m.rssi)} dBm</span>`;
  const b = document.createElement('button'); b.className = 'btn primary'; b.textContent = already ? 'Déjà appairé' : 'Appairer';
  b.disabled = already;
  b.addEventListener('click', () => {
    if (!confirm('Ajouter ' + (m.name || m.mac) + ' comme ESCLAVE de ce module ?\nLe module redémarre.')) return;
    // On transmet le nom annoncé : le maître le met en cache (affichage + repère
    // au ré-appairage). Un nom « S3-KBD-XXYY » par défaut n'est pas un nom convivial.
    const nm = (m.name && !/^S3-KBD-/.test(m.name)) ? m.name : '';
    b.disabled = true; send({ t:'pair', a:'bind', mac:m.mac, name:nm }, true);
  });
  el.appendChild(b); list.appendChild(el);
}
function startScan() {
  $('#scanList').innerHTML = ''; $('#btnScan').disabled = true;
  send({ t:'pair', a:'scan' }, true);
  setTimeout(() => { $('#btnScan').disabled = false; }, 8000);   // filet si scan done n'arrive pas
}
function onPair(m) { toast(m.ok ? 'Appairage : OK — redémarrage' : 'Appairage : échec'); }

// ===========================================================================
//  Sécurité (LESC) : changer la passkey BLE / le mot de passe Wi-Fi.
//  La saisie de la passkey à l'appairage se fait dans la boîte de l'OS, pas ici.
// ===========================================================================
function onSec(m) {
  if (m.ok) { toast('Sécurité enregistrée — le module redémarre, reconnectez'); return; }
  if (cfg) { cfg.pkset = m.pkset; cfg.wifiset = m.wifiset; }   // indicateurs (jamais les valeurs)
}
function changePasskey() {
  if (cfg && cfg.role === 1 && (cfg.slaves || []).length > 0) {
    toast('Désappairez d\'abord tous les esclaves'); return;
  }
  const v = ($('#secPasskey').value || '').trim();
  if (!/^[0-9]{6}$/.test(v)) { toast('Passkey = 6 chiffres (ex. 000000)'); return; }
  if (!confirm('Changer la passkey en ' + v + ' ?\nLe module efface ses appairages et redémarre.\n' +
               'Pensez à « oublier » le module dans les réglages Bluetooth du téléphone.')) return;
  send({ t:'sec', a:'passkey', pk:v }, true);
  $('#secPasskey').value = '';
}
function changeWifi() {
  const v = $('#secWifi').value || '';
  if (v.length < 8 || v.length > 63) { toast('Mot de passe Wi-Fi : 8 à 63 caractères'); return; }
  if (!confirm('Changer le mot de passe Wi-Fi ?\nLe SoftAP redémarre, reconnectez le téléphone.')) return;
  send({ t:'sec', a:'wifi', psk:v }, true);
  $('#secWifi').value = '';
}

// ===========================================================================
//  Renommage des modules (nom convivial persistant)
// ===========================================================================
function onName(m) {
  if (!cfg) return;
  if (!m.id) { cfg.name = m.name || ''; if ($('#cfgName')) $('#cfgName').value = cfg.name; }
  else { const s = (cfg.slaves || []).find((x) => x.id === m.id); if (s) s.name = m.name || ''; }
  renderSlaves(); buildLinkTargets(); buildGpioModules();
  // Le nom est aussi l'annonce BLE : renommer un module (hors esclave routé) le
  // fait redémarrer pour rafraîchir l'annonce (visible au scan / sélecteur OS).
  toast(m.reboot ? 'Renommé — le module redémarre, reconnectez' : 'Nom enregistré');
}
function renameModule() {
  const nm = ($('#cfgName').value || '').trim().slice(0, 19);
  send({ t:'name', name: nm }, true);
}
// Événement de lien maître→esclave : met à jour l'entrée cfg.slaves[id] et réaffiche.
function onLink(m) {
  if (!cfg || !cfg.slaves) return;
  const s = cfg.slaves.find((x) => x.id === m.id);
  if (!s) return;
  if (m.up !== undefined)   s.up = m.up ? 1 : 0;
  if (m.rssi !== undefined) { s.rssi = m.rssi; s.up = 1; }
  renderSlaves();
}

// ===========================================================================
//  Test de liaison : pings numérotés, RTT min/moy/max, pertes
// ===========================================================================
// Remplit le sélecteur de cible du test de liaison : maître (id 0) + esclaves.
function buildLinkTargets() {
  const sel = $('#linkTarget'); if (!sel) return;
  const prev = sel.value;
  sel.innerHTML = '';
  const add = (id, label) => { const o = document.createElement('option'); o.value = id; o.textContent = label; sel.appendChild(o); };
  add(0, (cfg && cfg.name) ? cfg.name + ' (id 0)' : 'Maître (id 0)');
  for (const s of (cfg && cfg.slaves) || []) add(s.id, (s.name || 'Esclave ' + s.id) + ' (id ' + s.id + ')');
  if (prev && sel.querySelector(`option[value="${prev}"]`)) sel.value = prev;
  renderLinkChart();                 // (ré)affiche le graphe (état vide au départ)
}

const pendingPings = new Map();       // n -> { t0, resolve, timer }
let pingSeq = 0;
function ping(id, timeoutMs = 1500) {
  const n = ++pingSeq;
  return new Promise((resolve) => {
    const t0 = performance.now();
    const timer = setTimeout(() => { pendingPings.delete(n); resolve(null); }, timeoutMs);
    pendingPings.set(n, { t0, resolve, timer });
    send({ t:'ping', n }, false, id);
  });
}
function onPong(m) {
  const p = pendingPings.get(m.n); if (!p) return;
  clearTimeout(p.timer); pendingPings.delete(m.n);
  p.resolve(performance.now() - p.t0);
}
async function linkTest(N = 20, gapMs = 100) {
  const btn = $('#btnLinkTest');
  if (!activeTransport || !activeTransport.connected) { toast('Non connecté'); return; }
  const sel = $('#linkTarget');
  const id = +(sel ? sel.value : 0);
  const name = (sel && sel.selectedOptions[0]) ? sel.selectedOptions[0].textContent : ('id ' + id);
  // (Re)crée la série de cette cible : relancer un test sur le même module l'écrase,
  // tester un autre module AJOUTE une courbe sur le même graphe.
  const s = { name, pts: [] };
  linkSeries.set(id, s);
  renderLinkChart();
  btn.disabled = true;
  for (let i = 0; i < N; i++) {
    s.pts.push(await ping(id));      // RTT (ms) ou null (perdu)
    renderLinkChart();               // tracé LIVE au fil des pings
    await new Promise((res) => setTimeout(res, gapMs));
  }
  btn.disabled = false;
}

// ---------------------------------------------------------------------------
//  Graphe RTT du test de liaison — X = n° de ping, Y = temps (ms).
//  Une série par module (couleur par id) ; plusieurs modules sur le même graphe.
// ---------------------------------------------------------------------------
const linkSeries = new Map();        // id -> { name, pts: [rtt|null, …] }
const LINK_COLORS = { 0:'#2f81f7', 1:'#3fb950', 2:'#d29922', 3:'#a371f7' };
function linkColor(id) { return LINK_COLORS[id] || '#8b949e'; }
function clearLinkChart() { linkSeries.clear(); renderLinkChart(); }
// Arrondi « joli » du max de l'axe Y.
function niceCeil(v) {
  if (v <= 10) return 10;
  const p = Math.pow(10, Math.floor(Math.log10(v))), n = v / p;
  const step = n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10;
  return step * p;
}
function renderLinkChart() {
  const box = $('#linkChart'); if (!box) return;
  const series = [...linkSeries.entries()];
  const W = 480, H = 240, ML = 42, MR = 10, MT = 10, MB = 30;
  const plotW = W - ML - MR, plotH = H - MT - MB;
  let maxN = 1, maxY = 10;
  for (const [, s] of series) {
    maxN = Math.max(maxN, s.pts.length);
    for (const v of s.pts) if (v != null) maxY = Math.max(maxY, v);
  }
  maxY = niceCeil(maxY);
  const xAt = (i) => ML + (maxN <= 1 ? plotW / 2 : (i / (maxN - 1)) * plotW);   // i : index 0-based
  const yAt = (v) => MT + plotH - (v / maxY) * plotH;
  let svg = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="xMidYMid meet" role="img" aria-label="Graphe RTT des pings">`;
  // Grille + graduations Y
  const yTicks = 4;
  for (let t = 0; t <= yTicks; t++) {
    const v = maxY * t / yTicks, y = yAt(v);
    svg += `<line x1="${ML}" y1="${y.toFixed(1)}" x2="${W - MR}" y2="${y.toFixed(1)}" class="grid"/>`;
    svg += `<text x="${ML - 6}" y="${(y + 3).toFixed(1)}" class="ylab">${v.toFixed(0)}</text>`;
  }
  // Graduations X (n° de ping)
  const xTicks = Math.min(maxN, 6);
  for (let t = 0; t < xTicks; t++) {
    const i = xTicks <= 1 ? 0 : Math.round(t * (maxN - 1) / (xTicks - 1));
    svg += `<text x="${xAt(i).toFixed(1)}" y="${MT + plotH + 14}" class="xlab">${i + 1}</text>`;
  }
  // Axes
  svg += `<line x1="${ML}" y1="${MT}" x2="${ML}" y2="${MT + plotH}" class="axis"/>`;
  svg += `<line x1="${ML}" y1="${MT + plotH}" x2="${W - MR}" y2="${MT + plotH}" class="axis"/>`;
  // Séries : courbe (rupture aux pings perdus) + points
  for (const [id, s] of series) {
    const col = linkColor(id);
    let d = '', started = false, dots = '';
    s.pts.forEach((v, i) => {
      if (v == null) { started = false; return; }
      const x = xAt(i).toFixed(1), y = yAt(v).toFixed(1);
      d += (started ? ' L' : ' M') + x + ' ' + y; started = true;
      dots += `<circle cx="${x}" cy="${y}" r="2.2" fill="${col}"/>`;
    });
    if (d) svg += `<path d="${d.trim()}" fill="none" stroke="${col}" stroke-width="1.8"/>`;
    svg += dots;
  }
  // Titres d'axes
  svg += `<text x="${(ML + plotW / 2).toFixed(0)}" y="${H - 4}" class="axtitle" text-anchor="middle">n° de ping</text>`;
  svg += `<text x="11" y="${(MT + plotH / 2).toFixed(0)}" class="axtitle" text-anchor="middle" transform="rotate(-90 11 ${(MT + plotH / 2).toFixed(0)})">RTT (ms)</text>`;
  svg += `</svg>`;
  box.innerHTML = svg;
  renderLinkLegend(series);
}
function renderLinkLegend(series) {
  const box = $('#linkLegend'); if (!box) return;
  box.innerHTML = '';
  if (!series.length) {
    box.innerHTML = '<span class="hint">Lancez un test : chaque module ajoute une courbe (X = n° de ping, Y = RTT en ms). « Effacer le graphe » réinitialise.</span>';
    return;
  }
  for (const [id, s] of series) {
    const recv = s.pts.filter((v) => v != null);
    const n = s.pts.length;
    const loss = n ? Math.round(100 * (n - recv.length) / n) : 0;
    const avg = recv.length ? recv.reduce((a, b) => a + b, 0) / recv.length : 0;
    const el = document.createElement('span'); el.className = 'legitem';
    el.innerHTML = `<span class="sw" style="background:${linkColor(id)}"></span>${esc(s.name)} · moy ${avg.toFixed(0)} ms · pertes ${loss}%`;
    box.appendChild(el);
  }
}

// ===========================================================================
//  GPIO — sorties (bascule) et entrées (voyants). Repères LOGIQUES = table du
//  firmware (gpio_panel.h, GPIO_TABLE) : sorties o1.., entrées i1.., numérotées
//  à partir de 1 dans chaque sens, + BOOT (bouton intégré). `pin` = GPIO physique
//  (câblage, info-bulle). Nouvelle broche : même ligne ici et dans GPIO_TABLE.
// ===========================================================================
const GPIO_OUT = [ { p:'o1', pin:4 }, { p:'o2', pin:5 }, { p:'o3', pin:6 }, { p:'o4', pin:7 } ];
const GPIO_IN  = [ { p:'BOOT', pin:0 }, { p:'i1', pin:8 }, { p:'i2', pin:9 }, { p:'i3', pin:10 }, { p:'i4', pin:11 } ];
// Liste des cartes à afficher : maître (id 0) + esclaves appairés.
function moduleList() {
  const mods = [{ id:0, name:(cfg && cfg.name) || 'Maître (id 0)' }];
  for (const s of (cfg && cfg.slaves) || []) mods.push({ id:s.id, name:(s.name || 'Esclave ' + s.id) });
  return mods;
}
// Une SECTION GPIO par module, toutes affichées ensemble. Les éléments portent
// data-gid (id de la carte) + data-gpio (broche) → clic routé et état ciblé.
function buildGpioModules() {
  const box = $('#gpioModules'); if (!box) return;
  box.innerHTML = '';
  for (const mod of moduleList()) {
    const sec = document.createElement('div'); sec.className = 'gpiomod'; sec.dataset.gid = mod.id;
    const h = document.createElement('h3'); h.innerHTML = `<span class="dot"></span>${esc(mod.name)}`; sec.appendChild(h);
    const mkRow = (label, cells) => {
      const row = document.createElement('div'); row.className = 'gpiorow';
      row.innerHTML = `<span class="gpiolabel">${label}</span>`;
      const cl = document.createElement('div'); cl.className = 'cluster';
      cells.forEach((c) => cl.appendChild(c)); row.appendChild(cl); return row;
    };
    const outs = GPIO_OUT.map(({ p, pin }) => {
      const el = document.createElement('div'); el.className = 'key nav gpio';
      el.dataset.gid = mod.id; el.dataset.gpio = p; el.textContent = p;
      el.title = `${p} = GPIO ${pin} · clic : basculer · appui long : clignotement ou PWM`;
      bindLongPress(el,
        () => openGpioFx(mod, p, el),                                        // appui long
        () => { flash(el); send({ t:'gpio', p, a:'tgl' }, false, mod.id); }); // clic : bascule
      return el;
    });
    const ins = GPIO_IN.map(({ p, pin }) => {
      const el = document.createElement('span'); el.className = 'chip led';
      el.dataset.gid = mod.id; el.dataset.gpio = p; el.textContent = p;
      el.title = p === 'BOOT' ? 'BOOT = bouton intégré (GPIO 0)' : `${p} = GPIO ${pin} (pull-up, actif bas)`;
      return el;
    });
    sec.appendChild(mkRow('Sorties', outs));
    sec.appendChild(mkRow('Entrées', ins));
    box.appendChild(sec);
  }
}
function onGpio(m) {
  const v = (m.v === 1 || m.v === true);
  const fx = !!m.fx && !m.done;           // effet autonome en cours (clignotement / PWM) sur la carte
  document.querySelectorAll(`[data-gid="${m.id}"][data-gpio="${m.p}"]`).forEach((el) => {
    el.classList.toggle('on', v);
    el.classList.toggle('fx', fx);
  });
}
// ---------------------------------------------------------------------------
//  Appui long : clic court = action normale, maintien = action secondaire.
//  Clic droit (ordinateur) = maintien. Un léger glissement (défilement de la
//  page) annule le maintien. Classe .pressing = jauge de progression (CSS).
// ---------------------------------------------------------------------------
const LONG_PRESS_MS = 450;
function bindLongPress(el, onLong, onShort) {
  let timer = null, fired = false, sx = 0, sy = 0;
  const cancel = () => { clearTimeout(timer); timer = null; el.classList.remove('pressing'); };
  const fire = () => {
    cancel(); fired = true;
    // Retour haptique (téléphone) — seulement après un vrai geste, sinon Chrome le bloque.
    const act = navigator.userActivation;
    if (navigator.vibrate && (!act || act.hasBeenActive)) { try { navigator.vibrate(15); } catch (e) {} }
    onLong();
  };
  el.addEventListener('pointerdown', (e) => {
    if (e.button) return;                          // bouton principal / doigt seulement
    fired = false; sx = e.clientX; sy = e.clientY;
    cancel(); el.classList.add('pressing');
    timer = setTimeout(fire, LONG_PRESS_MS);
  });
  el.addEventListener('pointermove', (e) => {
    if (timer && Math.hypot(e.clientX - sx, e.clientY - sy) > 10) cancel();
  });
  el.addEventListener('pointerup', cancel);
  el.addEventListener('pointerleave', cancel);
  el.addEventListener('pointercancel', cancel);
  el.addEventListener('contextmenu', (e) => { e.preventDefault(); if (!fired) fire(); });
  el.addEventListener('click', () => { if (fired) { fired = false; return; } onShort(); });
}

// ---------------------------------------------------------------------------
//  Popup « effet » d'une sortie (appui long) : clignotement (loop) ou PWM,
//  toujours en durée infinie — l'effet tourne sur la carte jusqu'à une nouvelle
//  commande sur cette sortie (clic = bascule, ou « Arrêter »).
// ---------------------------------------------------------------------------
const FX_PRESETS = { lent:[1000, 1000], normal:[500, 500], rapide:[100, 100], eclat:[50, 950] };
const fxLast = {};                                  // derniers réglages par carte:sortie
let fxTarget = null;                                // { mod, p }

function fxParams(key) {
  return fxLast[key] || (fxLast[key] = { mode:'loop', tSet:500, tClr:500, duty:50, hz:1000 });
}
function openGpioFx(mod, p, el) {
  fxTarget = { mod, p };
  const s = fxParams(mod.id + ':' + p);
  $('#fxTitle').textContent = `Sortie ${p} · ${mod.name}`;
  $('#fxRunning').classList.toggle('hidden', !(el && el.classList.contains('fx')));
  $('#fxSet').value = s.tSet; $('#fxClr').value = s.tClr; $('#fxDuty').value = s.duty;
  document.querySelectorAll('#fxHz .btn').forEach((b) => b.classList.toggle('active', +b.dataset.hz === s.hz));
  setFxMode(s.mode);
  renderFx();
  $('#gpioFx').classList.remove('hidden');
}
function closeGpioFx() { $('#gpioFx').classList.add('hidden'); fxTarget = null; }
function setFxMode(mode) {
  document.querySelectorAll('#fxModes .btn').forEach((b) => b.classList.toggle('active', b.dataset.mode === mode));
  document.querySelectorAll('#gpioFx .fxpane').forEach((pn) => pn.classList.toggle('hidden', pn.dataset.pane !== mode));
  if (fxTarget) fxParams(fxTarget.mod.id + ':' + fxTarget.p).mode = mode;
}
// Valeurs affichées + mémorisation des réglages de la sortie.
function renderFx() {
  if (!fxTarget) return;
  const s = fxParams(fxTarget.mod.id + ':' + fxTarget.p);
  s.tSet = +$('#fxSet').value; s.tClr = +$('#fxClr').value; s.duty = +$('#fxDuty').value;
  $('#fxSetVal').textContent = s.tSet + ' ms';
  $('#fxClrVal').textContent = s.tClr + ' ms';
  $('#fxDutyVal').textContent = s.duty + ' %';
  const cycle = s.tSet + s.tClr, hz = 1000 / cycle;
  const num = (x) => x.toLocaleString('fr-FR', { maximumFractionDigits:1 });
  $('#fxLoopInfo').textContent = `Cycle ${cycle} ms · ${hz >= 1 ? num(hz) + ' Hz' : 'un éclat toutes les ' + num(cycle / 1000) + ' s'}` +
    ` · allumé ${Math.round(100 * s.tSet / cycle)} % du temps`;
  $('#fxPwmInfo').textContent = s.duty === 0 ? 'Sortie maintenue à 0' : s.duty === 100 ? 'Sortie maintenue à 1'
    : `Niveau moyen ${s.duty} % · ${s.hz >= 1000 ? s.hz / 1000 + ' kHz' : s.hz + ' Hz'}`;
}
function runGpioFx() {
  if (!fxTarget) return;
  const { mod, p } = fxTarget, s = fxParams(mod.id + ':' + p);
  // Durée infinie : ni nb (loop) ni t_pwm (pwm).
  const cmd = s.mode === 'pwm' ? { t:'gpio', p, a:'pwm', duty:s.duty, hz:s.hz }
                               : { t:'gpio', p, a:'loop', t_set:s.tSet, t_clr:s.tClr };
  send(cmd, true, mod.id);
  closeGpioFx();
  toast(s.mode === 'pwm' ? `PWM ${s.duty} % lancé sur la sortie ${p}` : `Clignotement lancé sur la sortie ${p}`);
}
function stopGpioFx() {
  if (!fxTarget) return;
  const { mod, p } = fxTarget;
  send({ t:'gpio', p, a:'clr' }, true, mod.id);    // toute écriture interrompt l'effet
  closeGpioFx();
  toast(`Sortie ${p} arrêtée`);
}
function initGpioFx() {
  $('#btnFxClose').addEventListener('click', closeGpioFx);
  $('#gpioFx').addEventListener('click', (e) => { if (e.target.id === 'gpioFx') closeGpioFx(); });
  $('#fxModes').addEventListener('click', (e) => { const b = e.target.closest('.btn'); if (b) { setFxMode(b.dataset.mode); renderFx(); } });
  $('#fxPresets').addEventListener('click', (e) => {
    const b = e.target.closest('.btn'); if (!b) return;
    const [on, off] = FX_PRESETS[b.dataset.preset]; $('#fxSet').value = on; $('#fxClr').value = off; renderFx();
  });
  $('#fxHz').addEventListener('click', (e) => {
    const b = e.target.closest('.btn'); if (!b || !fxTarget) return;
    fxParams(fxTarget.mod.id + ':' + fxTarget.p).hz = +b.dataset.hz;
    document.querySelectorAll('#fxHz .btn').forEach((x) => x.classList.toggle('active', x === b));
    renderFx();
  });
  ['#fxSet', '#fxClr', '#fxDuty'].forEach((id) => $(id).addEventListener('input', renderFx));
  $('#btnFxRun').addEventListener('click', runGpioFx);
  $('#btnFxStop').addEventListener('click', stopGpioFx);
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && fxTarget) closeGpioFx(); });
}

// Relit toutes les cartes (maître + esclaves).
function readGpio() {
  if (!activeTransport || !activeTransport.connected) return;
  for (const mod of moduleList()) send({ t:'gpio', a:'read' }, false, mod.id);
}

// ===========================================================================
//  toast / util
// ===========================================================================
function esc(s) { return String(s).replace(/[&<>"]/g, (c2) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c2])); }
// logLine : neutralisé (l'onglet Journal a été retiré). Conservé en no-op pour
// ne pas toucher aux nombreux appels internes.
function logLine(/* kind, text */) {}
let toastTimer = null;
function toast(msg) {
  const el = $('#toast'); el.textContent = msg; el.classList.remove('hidden');
  clearTimeout(toastTimer); toastTimer = setTimeout(() => el.classList.add('hidden'), 1800);
}
// Retour visuel + retour au toucher (vibration / clic) d'une frappe.
function flash(el) { feedback(); el.classList.add('flash'); setTimeout(() => el.classList.remove('flash'), 90); }

// ---------------------------------------------------------------------------
//  Retour au toucher : préférence de CET appareil (localStorage, pas la NVS).
//  Vibration = Android/Chrome ; Safari (iPhone, transport Wi-Fi) n'a pas
//  navigator.vibrate -> clic sonore synthétisé (Web Audio, sans fichier).
// ---------------------------------------------------------------------------
const FB_KEY = 's3kbd.feedback', FB_VIBRATE_MS = 15;
const canVibrate = typeof navigator.vibrate === 'function';
let fbMode = canVibrate ? 'vibrate' : 'click';      // vibrate | click | both | off
try { fbMode = localStorage.getItem(FB_KEY) || fbMode; } catch (e) {}
let audioCtx = null;
function audioReady() {
  if (!audioCtx) {
    const AC = window.AudioContext || window.webkitAudioContext; if (!AC) return null;
    audioCtx = new AC();
  }
  if (audioCtx.state === 'suspended') audioCtx.resume().catch(() => {});   // déverrouillé par un geste
  return audioCtx;
}
function clickSound() {
  try {
    const ctx = audioReady(); if (!ctx) return;
    const t = ctx.currentTime, o = ctx.createOscillator(), g = ctx.createGain();
    o.type = 'square'; o.frequency.value = 1600;
    g.gain.setValueAtTime(0.06, t); g.gain.exponentialRampToValueAtTime(0.0001, t + 0.025);
    o.connect(g); g.connect(ctx.destination); o.start(t); o.stop(t + 0.03);
  } catch (e) {}
}
function feedback() {
  if (canVibrate && (fbMode === 'vibrate' || fbMode === 'both')) {
    const act = navigator.userActivation;   // Chrome refuse vibrate avant le 1er geste
    if (!act || act.hasBeenActive) { try { navigator.vibrate(FB_VIBRATE_MS); } catch (e) {} }
  }
  if (fbMode === 'click' || fbMode === 'both') clickSound();
}
function setFeedback(mode) {
  fbMode = mode; try { localStorage.setItem(FB_KEY, mode); } catch (e) {}
  renderFeedback(); feedback();             // essai immédiat du mode choisi
}
function renderFeedback() {
  document.querySelectorAll('[data-fb]').forEach((b) => {
    const on = b.dataset.fb === fbMode;
    b.classList.toggle('primary', on); b.classList.toggle('ghost', !on);
    if (b.dataset.fb === 'vibrate' || b.dataset.fb === 'both') b.disabled = !canVibrate;
  });
  const h = $('#fbNoVibrate'); if (h) h.classList.toggle('hidden', canVibrate);
}

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
let cssRotated = false;   // repli : rotation CSS forcée (paysage simulé par transform)
function updateEnv() {
  const coarse = matchMedia('(pointer:coarse)').matches;
  const physLandscape = matchMedia('(orientation:landscape)').matches;
  const isPhone = coarse && Math.min(screen.width, screen.height) <= 560;
  // Si l'appareil passe PHYSIQUEMENT en paysage, la rotation CSS forcée n'a plus
  // lieu d'être : on la retire pour ne pas doubler la rotation.
  if (cssRotated && physLandscape) { cssRotated = false; document.body.classList.remove('force-rotate'); }
  const landscape = physLandscape || cssRotated;   // la rotation CSS simule le paysage
  document.body.classList.toggle('is-phone', isPhone);
  document.body.classList.toggle('landscape', landscape);
  document.body.classList.toggle('portrait', !landscape);
  const rot = $('#btnRotate'); if (rot) rot.classList.toggle('active', cssRotated);
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
// Force la rotation en PAYSAGE. Deux niveaux :
//   1) natif (Android/Chrome) : plein écran + screen.orientation.lock ;
//   2) REPLI CSS : quand le natif est indisponible ou échoue — cas de l'iPhone
//      (pas de Web Bluetooth donc transport Wi-Fi, et son WebView/portail captif
//      n'expose ni Fullscreen ni orientation.lock) — on pivote toute la page de
//      90° via transform (voir .force-rotate dans style.css). Un 2ᵉ appui annule.
function setCssRotation(on) {
  cssRotated = on;
  document.body.classList.toggle('force-rotate', on);
  updateEnv();                       // recalcule les classes paysage/portrait + état du bouton
}
async function toggleRotation() {
  if (cssRotated) { setCssRotation(false); return; }         // déjà forcé : on annule
  const so = screen.orientation;
  // 1) Essai natif : ne le tente que si TOUTES les briques existent.
  if (so && so.lock && document.documentElement.requestFullscreen) {
    try {
      if (!document.fullscreenElement) await document.documentElement.requestFullscreen();
      const next = (so.type || '').startsWith('landscape') ? 'portrait' : 'landscape';
      await so.lock(next);
      return;                        // verrouillage natif OK
    } catch (e) { /* indisponible/refusé → repli CSS ci-dessous */ }
  }
  // 2) Repli CSS (iPhone, portail captif Wi-Fi, plein écran refusé…).
  setCssRotation(true);
  toast('Rotation paysage forcée');
}

// ===========================================================================
//  Câblage
// ===========================================================================
function wireUI() {
  $('#btnConn').addEventListener('click', onConnClick);
  $('#btnFull').addEventListener('click', toggleFullscreen);
  $('#btnRotate').addEventListener('click', toggleRotation);
  $('#btnCfg').addEventListener('click', toggleCfg);

  document.querySelectorAll('.tab').forEach((tab) => tab.addEventListener('click', () => switchTab(tab.dataset.tab)));
  // Sous-onglets du panneau Réglages
  document.querySelectorAll('.subtab').forEach((s) => s.addEventListener('click', () => switchCfgSub(s.dataset.sub)));
  // Retour au toucher (sous-onglet Appareil) + déverrouillage audio : iOS/Safari ne
  // l'autorise que dans un vrai geste (touchend/click), pas au pointerdown.
  document.querySelectorAll('[data-fb]').forEach((b) => b.addEventListener('click', () => setFeedback(b.dataset.fb)));
  renderFeedback();
  const unlockAudio = () => { if ((fbMode === 'click' || fbMode === 'both') && (!audioCtx || audioCtx.state !== 'running')) audioReady(); };
  document.addEventListener('touchend', unlockAudio, { passive:true });
  document.addEventListener('click', unlockAudio, true);

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
  $('#btnLinkTest').addEventListener('click', () => linkTest());
  $('#btnLinkClear').addEventListener('click', clearLinkChart);
  $('#btnGpioRead').addEventListener('click', readGpio);

  // Nom du module + sécurité (passkey BLE / mot de passe Wi-Fi)
  $('#btnRenameSelf').addEventListener('click', renameModule);
  $('#btnSecPasskey').addEventListener('click', changePasskey);
  $('#btnSecWifi').addEventListener('click', changeWifi);

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

  // Étapes de rendu isolées : si l'une échoue (ex. DOM dépareillé par un cache SW
  // périmé), on N'empêche PAS le câblage du bouton Connecter et le choix du transport.
  try {
    buildKeyboard('#kb-azerty', KB_AZERTY);
    buildKeyboard('#kb-num', KB_NUM);
    buildFn();
    buildBios();
    buildGpioModules();
    renderSteps();
    refreshMods();
    initMouse();
    initTextPass();
    initGpioFx();
  } catch (e) { logLine('err', 'init UI (partiel) : ' + e.message); }

  try { wireUI(); } catch (e) { logLine('err', 'wireUI : ' + e.message); }
  try { updateEnv(); } catch (e) { logLine('err', 'updateEnv : ' + e.message); }
  try { setCfgLocked(true); } catch (e) { logLine('err', 'setCfgLocked : ' + e.message); }   // déconnecté au démarrage

  logLine('in', '=== app.js v22 (répétition + retour au toucher) chargé ===');
  logLine('in', 'Page: ' + location.protocol + '//' + location.host + '  (sécurisé=' + window.isSecureContext + ')');
  selectTransport();
  logLine('in', 'prêt.');
}
document.addEventListener('DOMContentLoaded', init);
