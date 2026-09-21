// ===========================================================================
//  config.h  —  Paramètres persistants en flash (NVS via Preferences)
// ---------------------------------------------------------------------------
//  Un seul enregistrement `cfg_t`, lu au démarrage (cfgLoad) et écrit sur
//  demande (cfgSave). Tout changement d'un flag USB (kb/ms/serial) impose un
//  redémarrage : le descripteur USB est figé à USB.begin().
//
//  Clés NVS (namespace "s3kbd") : kb, ms, ser, gpio, pair (uchar 0/1),
//  role (uchar : 0 standard, 1 maître, 2 esclave), peer (6 octets = MAC BLE
//  du module appairé ; tout à zéro = aucun).
// ===========================================================================
#pragma once
#include <Preferences.h>
#include <ArduinoJson.h>

enum : uint8_t { ROLE_STD = 0, ROLE_MASTER = 1, ROLE_SLAVE = 2 };

struct cfg_t {
  bool    hidKb;     // clavier HID (+ Consumer Control)
  bool    hidMs;     // souris HID
  bool    serial;    // 2e port COM (CDC) parlant le protocole
  bool    gpio;      // panneau GPIO
  bool    pair;      // appairage autorisé (scan / bind / slave)
  uint8_t role;      // ROLE_*
  uint8_t peer[6];   // MAC du module appairé (maître si esclave, esclave si maître)
};

static cfg_t g_cfg;

static const char* CFG_NS = "s3kbd";

static void cfgLoad() {
  Preferences p;
  p.begin(CFG_NS, false);                      // lecture/écriture : crée le namespace au 1er boot
  g_cfg.hidKb  = p.getUChar("kb",   1) != 0;
  g_cfg.hidMs  = p.getUChar("ms",   1) != 0;
  g_cfg.serial = p.getUChar("ser",  0) != 0;
  g_cfg.gpio   = p.getUChar("gpio", 1) != 0;
  g_cfg.pair   = p.getUChar("pair", 1) != 0;
  g_cfg.role   = p.getUChar("role", ROLE_STD);
  memset(g_cfg.peer, 0, 6);
  if (p.getBytesLength("peer") == 6) p.getBytes("peer", g_cfg.peer, 6);
  p.end();
  if (g_cfg.role > ROLE_SLAVE) g_cfg.role = ROLE_STD;
}

static void cfgSave() {
  Preferences p;
  p.begin(CFG_NS, false);
  p.putUChar("kb",   g_cfg.hidKb  ? 1 : 0);
  p.putUChar("ms",   g_cfg.hidMs  ? 1 : 0);
  p.putUChar("ser",  g_cfg.serial ? 1 : 0);
  p.putUChar("gpio", g_cfg.gpio   ? 1 : 0);
  p.putUChar("pair", g_cfg.pair   ? 1 : 0);
  p.putUChar("role", g_cfg.role);
  p.putBytes("peer", g_cfg.peer, 6);
  p.end();
}

// MAC binaire -> "aa:bb:cc:dd:ee:ff" (out : 18 octets mini)
static void cfgMacStr(const uint8_t* m, char* out) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// "aa:bb:cc:dd:ee:ff" (séparateur ':' ou '-') -> 6 octets. false si mal formée.
static bool cfgParseMac(const char* s, uint8_t* out) {
  if (!s || strlen(s) != 17) return false;
  for (int i = 0; i < 6; i++) {
    const char* h = s + i * 3;
    char* end = nullptr;
    char tmp[3] = { h[0], h[1], 0 };
    long v = strtol(tmp, &end, 16);
    if (end != tmp + 2) return false;
    if (i < 5 && h[2] != ':' && h[2] != '-') return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

static bool cfgPeerValid() {
  for (int i = 0; i < 6; i++) if (g_cfg.peer[i]) return true;
  return false;
}

// Sérialise la config (+ état du lien) : réponse à {"t":"cfg","a":"get"}.
static void cfgToJson(char* out, size_t n, const char* mac, bool link, int rssi) {
  JsonDocument d;
  d["hid_kb"] = g_cfg.hidKb ? 1 : 0;
  d["hid_ms"] = g_cfg.hidMs ? 1 : 0;
  d["serial"] = g_cfg.serial ? 1 : 0;
  d["gpio"]   = g_cfg.gpio ? 1 : 0;
  d["pair"]   = g_cfg.pair ? 1 : 0;
  d["role"]   = g_cfg.role;
  d["mac"]    = mac;
  char peer[18] = "";
  if (cfgPeerValid()) cfgMacStr(g_cfg.peer, peer);
  d["peer"]   = peer;
  d["link"]   = link ? 1 : 0;
  d["rssi"]   = rssi;
  serializeJson(d, out, n);
}
