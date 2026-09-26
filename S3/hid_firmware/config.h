// ===========================================================================
//  config.h  —  Paramètres persistants en flash (NVS via Preferences)
// ---------------------------------------------------------------------------
//  Un seul enregistrement `cfg_t`, lu au démarrage (cfgLoad) et écrit sur
//  demande (cfgSave). Tout changement d'un flag USB (kb/ms/serial) impose un
//  redémarrage : le descripteur USB est figé à USB.begin().
//
//  Clés NVS (namespace "s3kbd") : kb, ms, ser, gpio, pair (uchar 0/1),
//  role (uchar : 0 standard, 1 maître, 2 esclave), peer (6 octets = MAC BLE
//  du maître, sur un esclave), self (uchar = id attribué à l'esclave par son
//  maître), slaves (blob = table de routage id↔MAC + nom du maître),
//  pk (uint32 = passkey LESC statique, 0 = 000000), wpsk (chaîne = clé WPA2 du
//  SoftAP), nm (chaîne = nom convivial du module).
//
//  SÉCURITÉ (phase LESC). La passkey statique `passkey` sert :
//    - à l'appairage téléphone↔module (l'utilisateur la saisit) ;
//    - au lien maître↔esclave (partagée : le maître la pousse à l'esclave au bind).
//  Elle NE circule jamais en clair (bind = lien chiffré Just Works, cf. ble_link.h).
//  Le mot de passe Wi-Fi `apPsk` sort du code en dur (wifi_portal.h). Changer la
//  passkey impose de désappairer tous les esclaves (règle) et efface les bonds.
//
//  Étoile multi-esclaves : le MAÎTRE (id 0) route par `id` vers ses esclaves
//  (id 1..MAX_SLAVES). Table `slaves[]` = (id, MAC) persistée. Un esclave garde
//  son `selfId` pour se signaler toujours avec le même id. `peer[]` ne sert plus
//  qu'à l'esclave (MAC de son maître, whitelist).
// ===========================================================================
#pragma once
#include <Preferences.h>
#include <ArduinoJson.h>

enum : uint8_t { ROLE_STD = 0, ROLE_MASTER = 1, ROLE_SLAVE = 2 };

// Nombre max d'esclaves en étoile. Plafond DUR du coeur : le total des
// connexions BLE simultanées (téléphone + esclaves) est limité par
// CONFIG_BT_NIMBLE_MAX_CONNECTIONS = 3. L'hôte pilote le maître par COM/Wi-Fi
// (ne consomment pas de connexion BLE) -> les 3 connexions vont aux esclaves.
#define MAX_SLAVES 3

// Longueur max d'un nom convivial (octets, \0 compris). Cap volontairement bas
// pour tenir dans STATUS_MAX quand `cfg get` sérialise le nom du module + celui
// de chaque esclave (cf. hid_firmware.ino, budget STATUS_MAX).
#define NAME_MAX 20

// Clé WPA2 par défaut du SoftAP (module neuf). >= 8 caractères (contrainte WPA2).
// Sert aussi de référence pour l'indicateur « Wi-Fi personnalisé » (statusCfg).
#define AP_PSK_DEFAULT "12345678"

struct slave_nv_t {
  uint8_t id;             // 1..MAX_SLAVES ; 0 = slot libre
  uint8_t mac[6];         // MAC BLE de l'esclave
  char    name[NAME_MAX]; // nom convivial de l'esclave (vu du maître), "" si aucun
};

struct cfg_t {
  bool     hidKb;    // clavier HID (+ Consumer Control)
  bool     hidMs;    // souris HID
  bool     serial;   // 2e port COM (CDC) parlant le protocole
  bool     gpio;     // panneau GPIO
  bool     pair;     // appairage autorisé (scan / bind / provisioning)
  bool     ble;      // serveur BLE : accepte les connexions d'un contrôleur (smartphone/OS)
  bool     wifi;     // point d'accès Wi-Fi (SoftAP) + télécommande web
  bool     boot5;    // BOOT ×5 rapides = désappairage physique (escape hatch)
  bool     bootRst;  // BOOT maintenu 20 s = reset d'usine (config + bonds, logiciel conservé)
  uint8_t  role;     // ROLE_*
  uint8_t  peer[6];  // esclave : MAC de son maître (whitelist). Inutilisé ailleurs.
  uint8_t  selfId;   // esclave : id attribué par le maître (0 sinon)
  uint32_t passkey;  // passkey LESC statique (0 = 000000 par défaut)
  char     apPsk[64];      // clé WPA2 du SoftAP (8..63 car.)
  char     name[NAME_MAX]; // nom convivial de CE module ("" = défaut S3-KBD-XXYY)
  slave_nv_t slaves[MAX_SLAVES];   // maître : table de routage id↔MAC(+nom)
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
  g_cfg.ble    = p.getUChar("ble",  1) != 0;             // par défaut : BLE actif
  g_cfg.wifi   = p.getUChar("wifi", 1) != 0;             // par défaut : Wi-Fi actif
  g_cfg.boot5  = p.getUChar("bt5",  1) != 0;             // par défaut : BOOT ×5 actif
  g_cfg.bootRst= p.getUChar("bt20", 1) != 0;             // par défaut : BOOT 20 s actif
  g_cfg.role   = p.getUChar("role", ROLE_STD);
  g_cfg.selfId = p.getUChar("self", 0);
  g_cfg.passkey = p.getUInt("pk", 0);                    // 0 = 000000 par défaut
  memset(g_cfg.apPsk, 0, sizeof(g_cfg.apPsk));
  if (p.getString("wpsk", g_cfg.apPsk, sizeof(g_cfg.apPsk)) == 0)
    strlcpy(g_cfg.apPsk, AP_PSK_DEFAULT, sizeof(g_cfg.apPsk));   // défaut module neuf
  memset(g_cfg.name, 0, sizeof(g_cfg.name));
  p.getString("nm", g_cfg.name, sizeof(g_cfg.name));     // "" si absent
  memset(g_cfg.peer, 0, 6);
  if (p.getBytesLength("peer") == 6) p.getBytes("peer", g_cfg.peer, 6);
  memset(g_cfg.slaves, 0, sizeof(g_cfg.slaves));
  // NB : ajouter un champ à slave_nv_t change sizeof(slaves) -> l'ancien blob
  // (firmware antérieur) est ignoré au 1er boot, la table repart vide. Sans
  // conséquence : un changement de sécurité impose de toute façon un ré-appairage.
  if (p.getBytesLength("slaves") == sizeof(g_cfg.slaves))
    p.getBytes("slaves", g_cfg.slaves, sizeof(g_cfg.slaves));
  p.end();
  if (g_cfg.role > ROLE_SLAVE) g_cfg.role = ROLE_STD;
  if (g_cfg.passkey > 999999)  g_cfg.passkey = 0;        // garde-fou (6 chiffres)
}

static void cfgSave() {
  Preferences p;
  p.begin(CFG_NS, false);
  p.putUChar("kb",   g_cfg.hidKb  ? 1 : 0);
  p.putUChar("ms",   g_cfg.hidMs  ? 1 : 0);
  p.putUChar("ser",  g_cfg.serial ? 1 : 0);
  p.putUChar("gpio", g_cfg.gpio   ? 1 : 0);
  p.putUChar("pair", g_cfg.pair   ? 1 : 0);
  p.putUChar("ble",  g_cfg.ble    ? 1 : 0);
  p.putUChar("wifi", g_cfg.wifi   ? 1 : 0);
  p.putUChar("bt5",  g_cfg.boot5  ? 1 : 0);
  p.putUChar("bt20", g_cfg.bootRst? 1 : 0);
  p.putUChar("role", g_cfg.role);
  p.putUChar("self", g_cfg.selfId);
  p.putUInt("pk", g_cfg.passkey);
  p.putString("wpsk", g_cfg.apPsk);
  p.putString("nm", g_cfg.name);
  p.putBytes("peer", g_cfg.peer, 6);
  p.putBytes("slaves", g_cfg.slaves, sizeof(g_cfg.slaves));
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

// ---------------------------------------------------------------------------
//  Table de routage du maître : slaves[] indexée par slot, id 1..MAX_SLAVES.
// ---------------------------------------------------------------------------
// Nombre d'esclaves occupant un slot.
static uint8_t slaveCount() {
  uint8_t n = 0;
  for (int i = 0; i < MAX_SLAVES; i++) if (g_cfg.slaves[i].id) n++;
  return n;
}

// Index (dans slaves[]) de l'esclave d'id `id`, -1 si absent.
static int slaveIndexById(uint8_t id) {
  if (!id) return -1;
  for (int i = 0; i < MAX_SLAVES; i++) if (g_cfg.slaves[i].id == id) return i;
  return -1;
}

// Plus petit id libre dans 1..MAX_SLAVES, 0 si la table est pleine.
static uint8_t slaveFreeId() {
  for (uint8_t id = 1; id <= MAX_SLAVES; id++) if (slaveIndexById(id) < 0) return id;
  return 0;
}

// Ajoute (id, mac) dans le 1er slot libre. false si plus de place.
static bool slaveAdd(uint8_t id, const uint8_t* mac) {
  if (!id) return false;
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (!g_cfg.slaves[i].id) {
      g_cfg.slaves[i].id = id;
      memcpy(g_cfg.slaves[i].mac, mac, 6);
      return true;
    }
  }
  return false;
}

// Retire l'esclave d'id `id`. false s'il n'existait pas.
static bool slaveRemove(uint8_t id) {
  int i = slaveIndexById(id);
  if (i < 0) return false;
  memset(&g_cfg.slaves[i], 0, sizeof(slave_nv_t));
  return true;
}

// Enregistre (dans la table du maître) le nom convivial de l'esclave `id`.
// Ne persiste pas ici : l'appelant fait cfgSave(). false si id inconnu.
static bool slaveSetName(uint8_t id, const char* name) {
  int i = slaveIndexById(id);
  if (i < 0) return false;
  strlcpy(g_cfg.slaves[i].name, name ? name : "", NAME_MAX);
  return true;
}

// ---------------------------------------------------------------------------
//  Sécurité (phase LESC) — helpers de validation
// ---------------------------------------------------------------------------
// Passkey BLE : entier 6 chiffres (000000..999999).
static bool secPasskeyValid(uint32_t pk) { return pk <= 999999; }
// Clé WPA2 : 8..63 caractères ASCII imprimables.
static bool secWifiPskValid(const char* s) {
  if (!s) return false;
  size_t n = strlen(s);
  return n >= 8 && n <= 63;
}

// NB : la sérialisation JSON de la config (réponse à cfg get) est dans
// hid_firmware.ino (statusCfg), car elle lit aussi l'état de lien par esclave
// (runtime g_link[] de ble_link.h).
