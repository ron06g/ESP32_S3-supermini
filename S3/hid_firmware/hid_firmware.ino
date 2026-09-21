// ===========================================================================
//  hid_firmware.ino  —  Lot S3 « HID-Bridge » (MOCK / POC, sans securite)
// ---------------------------------------------------------------------------
//  ESP32-S3 (SuperMini) qui est SIMULTANEMENT :
//    - un clavier USB HID (+ Consumer Control) et une souris, activables
//      independamment (parametres en flash, config.h) ;
//    - un port COM USB optionnel (le CDC unique bascule en mode protocole) ;
//    - un serveur BLE GATT qui recoit les commandes du site web ;
//    - un point d'acces Wi-Fi servant l'app + WebSocket (wifi_portal.h) ;
//    - un traducteur commande -> frappe (disposition AZERTY, sequences) ;
//    - quelques GPIO pilotables (gpio_panel.h) ;
//    - MAITRE ou ESCLAVE d'un second module identique (ble_link.h) : le
//      maitre transfere toutes ses sorties a l'esclave via BLE, avec le
//      MEME protocole JSON.
//
//  Contrat GATT : socle §5 + extensions (cfg / pair / ping / gpio, cf. README).
//  Table AZERTY : keymap_azerty.h (spec S3 §6).
//
//  Dependances :
//    - Coeur Arduino-ESP32 >= 3.x (TinyUSB natif, 2 ports CDC, Preferences).
//    - Bibliotheques ArduinoJson v7 et WebSockets (Links2004).
//
//  Reglages Arduino IDE (Outils) : Board "ESP32S3 Dev Module", USB Mode =
//  "USB-OTG (TinyUSB)" (OBLIGATOIRE pour le HID), USB CDC On Boot = **Disabled**
//  (voir ci-dessous), Flash 4 MB, Partition "Huge APP", PSRAM = QSPI.
//
//  POURQUOI « CDC On Boot = Disabled » : avec cette option, le coeur appelle
//  USB.begin() dans app_main(), AVANT setup(). Or les interfaces USB (HID
//  clavier/souris, 2e CDC) sont choisies d'apres la config NVS lue dans setup()
//  et doivent etre construites AVANT USB.begin() (le descripteur est fige a ce
//  moment). Le sketch possede donc lui-meme la console USB (`Console`, CDC 0)
//  et l'appel a USB.begin() (usbBegin()).
//
//  ATTENTION (socle §7 BadUSB) : ce peripherique EST un injecteur de frappes.
//  Tant que la securite n'est pas en place, ne le brancher que sur des machines
//  de confiance.
// ===========================================================================

#include "USB.h"
#include "USBCDC.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDMouse.h"
#include <ArduinoJson.h>

// Console debug/flash = USB CDC interface 0, possedee par le sketch (CDC On Boot
// desactive). Construite a l'init statique -> enregistree avant USB.begin().
static USBCDC Console(0);

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <host/ble_gap.h>               // ble_gap_conn_desc (pile NimBLE du coeur 3.x)
#include "esp_mac.h"
#include "esp_heap_caps.h"              // mesures de heap (interne / PSRAM)

#include "keymap_azerty.h"

// ---------------------------------------------------------------------------
//  Contrat d'interface — UUIDs du service HID-Bridge (socle §5.1)
// ---------------------------------------------------------------------------
#define SERVICE_UUID  "9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define CMD_UUID      "9f1d0001-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define STATUS_UUID   "9f1d0002-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define BLE_NAME      "S3-KBD"          // annonce : S3-KBD-XXYY (2 derniers octets MAC)

// ---------------------------------------------------------------------------
//  Reglages
// ---------------------------------------------------------------------------
#define STATUS_MAX 200                              // taille max d'une frame STATUS (cfg:{...})
static const uint16_t KBD_VID = 0x303A;             // Espressif (dev)
static const uint16_t KBD_PID = 0x8161;             // PID de dev, stable
static const char*    KBD_PRODUCT = "S3-KBD (mock)";

static const uint32_t KEY_PRESS_MS   = 5;   // duree appui d'une touche
static const uint32_t KEY_GAP_MS     = 5;   // repos entre deux frappes
static const uint32_t TXT_INTERKEY_MS= 6;   // cadence inter-caractere du mode macro
static const size_t   CMD_QUEUE_LEN  = 16;  // profondeur de la file de commandes
static const size_t   CMD_MAX_BYTES  = 600; // garde-fou taille d'une commande

// Desappairage physique : N appuis sur le bouton BOOT (GPIO0) en moins de W ms.
// Escape hatch pour un esclave (plus de Wi-Fi ni d'acces web) : revient en mode
// standard. Fonctionne quel que soit le role et meme si le panneau GPIO est off.
static const uint8_t  RESET_BTN_PIN          = 0;
static const uint8_t  BOOT_RESET_TAPS   = 5;
static const uint32_t BOOT_RESET_WIN_MS = 3000;

// --- Declarations avancees partagees par les modules (.h) ---
static void enqueueCommand(const uint8_t* data, size_t len);
static void releaseAll();
static void notifyStatus(const char* s);
static void rebootWithStatus(const char* s);

#include "config.h"                    // parametres NVS (g_cfg)

// Logs de debug sur la console USB. TUS quand le port serie protocole est actif :
// le CDC unique porte alors le protocole, on le garde propre. (g_cfg vient de config.h)
#define DBG(...)  do { if (!g_cfg.serial) Console.printf(__VA_ARGS__); } while (0)
#define DBGLN(s)  do { if (!g_cfg.serial) Console.println(s); } while (0)
#include "status_led.h"
#include "web_assets.h"                // app WEB/Keyboard/ embarquee (genere)
#include "wifi_portal.h"               // SoftAP + portail captif + WebSocket
#include "com_port.h"                  // 2e port CDC (protocole sur COM)
#include "gpio_panel.h"                // panneau GPIO
#include "ble_link.h"                  // appairage maitre / esclave

// ---------------------------------------------------------------------------
//  Objets USB — instancies dynamiquement selon la config (le constructeur
//  enregistre l'interface dans le descripteur : il faut donc ne PAS construire
//  ce qui est desactive).
// ---------------------------------------------------------------------------
static USBHIDKeyboard*        g_kb    = nullptr;
static USBHIDConsumerControl* g_cc    = nullptr;
static USBHIDMouse*           g_mouse = nullptr;

// ---------------------------------------------------------------------------
//  Etat global
// ---------------------------------------------------------------------------
BLECharacteristic* g_cmdChar    = nullptr;
BLECharacteristic* g_statusChar = nullptr;
BLEServer*         g_server     = nullptr;
static char        g_bleMac[18] = "";

volatile bool g_connected = false;   // un client BLE (telephone, ou le maitre si esclave)
volatile bool g_stop      = false;   // demande d'arret de sequence
QueueHandle_t g_cmdQueue  = nullptr; // file de char* (JSON \0-termine, malloc)
SemaphoreHandle_t g_stopMux = nullptr; // serialise le fast-path STOP entre BLE et Wi-Fi

uint8_t g_heldMods    = 0;           // modificateurs maintenus (Ctrl/Shift/...)
uint8_t g_heldKeys[6] = {0};         // touches non-modif maintenues (down/up)

// Etat LED « de fond » selon le role et les connexions.
static LedMode ledBaseMode() {
  if (g_cfg.role == ROLE_SLAVE)  return g_connected ? LST_SLAVE_LINKED : LST_SLAVE_WAIT;
  if (g_cfg.role == ROLE_MASTER) return g_linkUp    ? LST_CONNECTED    : LST_IDLE;
  return g_connected ? LST_CONNECTED : LST_IDLE;
}

// ===========================================================================
//  Bas niveau : emission des rapports HID clavier
// ===========================================================================
static void sendHID() {
  if (!g_kb) return;
  KeyReport kr = {0};
  kr.modifiers = g_heldMods;
  for (int i = 0; i < 6; i++) kr.keys[i] = g_heldKeys[i];
  g_kb->sendReport(&kr);
}

static void holdKey(uint8_t k) {
  if (!k) return;
  for (int i = 0; i < 6; i++) if (g_heldKeys[i] == k) return;    // deja tenue
  for (int i = 0; i < 6; i++) if (g_heldKeys[i] == 0) { g_heldKeys[i] = k; break; }
  sendHID();
}

static void unholdKey(uint8_t k) {
  for (int i = 0; i < 6; i++) if (g_heldKeys[i] == k) g_heldKeys[i] = 0;
  sendHID();
}

// Appui + relache d'une touche, en ajoutant temporairement extraMods.
static void tapKey(uint8_t k, uint8_t extraMods) {
  ledPulse(C_GREEN, 55);              // impulsion verte par frappe (repetitions gerees en file)
  uint8_t saved = g_heldMods;
  g_heldMods |= extraMods;
  holdKey(k);
  vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS));
  unholdKey(k);
  g_heldMods = saved;
  sendHID();                          // revient a l'etat des modificateurs tenus
  vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
}

// Relachement de surete : tout relacher (socle : ne pas laisser de touche collee).
static void releaseAll() {
  g_heldMods = 0;
  for (int i = 0; i < 6; i++) g_heldKeys[i] = 0;
  if (g_kb) g_kb->releaseAll();
  sendHID();
}

// ===========================================================================
//  STATUS (socle §5 / spec S3 §4) — diffuse vers BLE, Wi-Fi, COM, console
// ===========================================================================
static void notifyStatus(const char* s) {
  // Reflet LED (point central) : busy/ready pilotent le fond, err:* fait une
  // impulsion orange transitoire sans figer la LED.
  if      (!strncmp(s, "err", 3)) ledPulse(C_ORANGE, 160);
  else if (!strcmp(s, "busy"))    ledSetMode(LST_BUSY);
  else if (!strcmp(s, "ready"))   ledSetMode(ledBaseMode());

  if (g_statusChar) {
    g_statusChar->setValue((uint8_t*)s, strlen(s));
    if (g_connected) g_statusChar->notify();   // garde g_connected : specifique BLE
  }
  wifiQueueStatus(s);                          // diffusion Wi-Fi (drainee par la tache reseau)
  comQueueStatus(s);                           // diffusion port COM (drainee par la tache com)
  DBG("[STATUS] %s\n", s);
}

// Emet un statut, laisse le temps aux transports de l'ecouler, redemarre.
static void rebootWithStatus(const char* s) {
  notifyStatus(s);
  vTaskDelay(pdMS_TO_TICKS(300));
  Console.flush();
  ESP.restart();
}

// ===========================================================================
//  Conversion du masque web (bits) -> octet de modificateurs HID
//  bit0=Ctrl,1=Shift,2=Alt,3=GUI,4=AltGr   (socle §5.2)
// ===========================================================================
static uint8_t maskToHid(int m) {
  uint8_t h = 0;
  if (m & 1)  h |= KM_CTRL;
  if (m & 2)  h |= KM_SHIFT;
  if (m & 4)  h |= KM_ALT;
  if (m & 8)  h |= KM_GUI;
  if (m & 16) h |= KM_ALTGR;
  return h;
}

// ===========================================================================
//  Decodeur UTF-8 (renvoie le point de code, avance *idx)
// ===========================================================================
static uint32_t utf8Decode(const char* s, size_t len, size_t* idx) {
  size_t i = *idx;
  if (i >= len) { *idx = i; return 0; }
  uint8_t c = (uint8_t)s[i];
  uint32_t cp; int extra;
  if      (c < 0x80)        { cp = c;         extra = 0; }
  else if ((c & 0xE0)==0xC0){ cp = c & 0x1F;  extra = 1; }
  else if ((c & 0xF0)==0xE0){ cp = c & 0x0F;  extra = 2; }
  else if ((c & 0xF8)==0xF0){ cp = c & 0x07;  extra = 3; }
  else                      { cp = c;         extra = 0; }
  i++;
  for (int k = 0; k < extra && i < len; k++) {
    if (((uint8_t)s[i] & 0xC0) != 0x80) break;
    cp = (cp << 6) | ((uint8_t)s[i] & 0x3F);
    i++;
  }
  *idx = i;
  return cp;
}

// ===========================================================================
//  Frappe d'un caractere via la table AZERTY
// ===========================================================================
static bool typeChar(uint32_t cp, uint8_t extraMods) {
  km_stroke_t st[2]; uint8_t n;
  if (!km_lookup(cp, st, &n)) return false;      // non mappe
  for (uint8_t i = 0; i < n; i++) tapKey(st[i].key, st[i].mod | extraMods);
  return true;
}

// Frappe d'une chaine UTF-8 (mode macro)
static void typeString(const char* v) {
  size_t len = strlen(v), i = 0;
  while (i < len && !g_stop) {
    uint32_t cp = utf8Decode(v, len, &i);
    if (cp == '\n')      tapKey(0x28, 0);         // Entree
    else if (cp == '\t') tapKey(0x2B, 0);         // Tab
    else if (cp == '\r') { /* ignore */ }
    else if (!typeChar(cp, 0)) {
      char e[24]; snprintf(e, sizeof(e), "err:unmapped:U+%04X", (unsigned)cp);
      notifyStatus(e);
    }
    vTaskDelay(pdMS_TO_TICKS(TXT_INTERKEY_MS));
  }
}

// ===========================================================================
//  Touches nommees (socle §5.2)
// ===========================================================================
// Renvoie l'usage HID d'une touche nommee, 0 si inconnue / traitee ailleurs.
static uint8_t nameToHid(const char* c) {
  struct { const char* n; uint8_t k; } T[] = {
    {"Enter",0x28},{"Escape",0x29},{"Backspace",0x2A},{"Tab",0x2B},
    {"Space",0x2C},{"Delete",0x4C},{"Insert",0x49},{"CapsLock",0x39},
    {"ArrowRight",0x4F},{"ArrowLeft",0x50},{"ArrowDown",0x51},{"ArrowUp",0x52},
    {"Home",0x4A},{"End",0x4D},{"PageUp",0x4B},{"PageDown",0x4E},
    {"PrintScreen",0x46},{"ScrollLock",0x47},{"Pause",0x48},
    {"ContextMenu",0x65},{"NumLock",0x53},
    {"F1",0x3A},{"F2",0x3B},{"F3",0x3C},{"F4",0x3D},{"F5",0x3E},{"F6",0x3F},
    {"F7",0x40},{"F8",0x41},{"F9",0x42},{"F10",0x43},{"F11",0x44},{"F12",0x45},
  };
  for (auto& e : T) if (strcmp(e.n, c) == 0) return e.k;
  return 0;
}

// Bit de modificateur d'un nom de modificateur seul, 0 sinon.
static uint8_t modNameToBit(const char* c) {
  if (!strcmp(c,"ControlLeft"))  return KM_CTRL;
  if (!strcmp(c,"ShiftLeft"))    return KM_SHIFT;
  if (!strcmp(c,"AltLeft"))      return KM_ALT;
  if (!strcmp(c,"MetaLeft"))     return KM_GUI;
  if (!strcmp(c,"AltRight"))     return KM_ALTGR;
  if (!strcmp(c,"ControlRight")) return 0x10;
  if (!strcmp(c,"ShiftRight"))   return 0x20;
  if (!strcmp(c,"MetaRight"))    return 0x80;
  return 0;
}

// Usage Consumer Control d'une touche media, 0 sinon (socle §5.2).
static uint16_t mediaNameToUsage(const char* c) {
  if (!strcmp(c,"MediaPlayPause"))  return 0x00CD;
  if (!strcmp(c,"MediaVolumeUp"))   return 0x00E9;
  if (!strcmp(c,"MediaVolumeDown")) return 0x00EA;
  if (!strcmp(c,"MediaMute"))       return 0x00E2;
  if (!strcmp(c,"MediaNext"))       return 0x00B5;
  if (!strcmp(c,"MediaPrevious"))   return 0x00B6;
  if (!strcmp(c,"MediaHome"))       return 0x0223;  // AC Home
  if (!strcmp(c,"MediaBack"))       return 0x0224;  // AC Back
  return 0;
}

static void mediaTap(uint16_t usage) {
  if (!g_cc) { notifyStatus("err:nohid"); return; }
  ledPulse(C_CYAN, 90);              // impulsion cyan pour la telecommande media
  g_cc->press(usage);
  vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS));
  g_cc->release();
  vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
}

// ===========================================================================
//  Souris (USB HID). Protocole web (socle, extension) :
//    {"t":"mouse","dx":<int>,"dy":<int>}          deplacement relatif
//    {"t":"mouse","w":<int>}                       molette (vertical)
//    {"t":"mouse","b":"left|right|middle","a":...}  clic / down / up / dbl
// ===========================================================================
static uint8_t mouseBtn(const char* b) {
  if (!strcmp(b, "left"))   return MOUSE_LEFT;
  if (!strcmp(b, "right"))  return MOUSE_RIGHT;
  if (!strcmp(b, "middle")) return MOUSE_MIDDLE;
  return 0;
}

// Un rapport HID souris ne porte que des deltas [-127..127] : on decoupe.
static void mouseMove(int dx, int dy, int w) {
  while (dx || dy || w) {
    int sx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
    int sy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
    int sw = w  > 127 ? 127 : (w  < -127 ? -127 : w);
    g_mouse->move((int8_t)sx, (int8_t)sy, (int8_t)sw);
    dx -= sx; dy -= sy; w -= sw;
  }
}

static void handleMouse(JsonDocument& doc) {
  if (!g_mouse) { notifyStatus("err:nohid"); return; }
  // Deplacement / molette
  if (doc["dx"].is<int>() || doc["dy"].is<int>() || doc["w"].is<int>()) {
    int w = doc["w"] | 0;
    mouseMove(doc["dx"] | 0, doc["dy"] | 0, w);
    if (w) ledPulse(C_GREEN, 40);
    return;
  }
  // Bouton
  const char* b = doc["b"] | "";
  if (*b) {
    uint8_t btn = mouseBtn(b);
    if (!btn) { notifyStatus("err:mouse"); return; }
    const char* a = doc["a"] | "click";
    if      (!strcmp(a, "down")) g_mouse->press(btn);
    else if (!strcmp(a, "up"))   g_mouse->release(btn);
    else if (!strcmp(a, "dbl"))  { g_mouse->click(btn); vTaskDelay(pdMS_TO_TICKS(40)); g_mouse->click(btn); }
    else                         g_mouse->click(btn);
    ledPulse(C_GREEN, 50);
    return;
  }
  notifyStatus("err:mouse");
}

// Traite une commande "key"
static void handleKey(const char* c, const char* a, int mask) {
  if (!c || !*c) return;

  // 1) Touche media -> Consumer Control
  uint16_t mu = mediaNameToUsage(c);
  if (mu) { mediaTap(mu); return; }

  // 2) Modificateur seul (down/up pour composer un raccourci touche-a-touche)
  uint8_t mb = modNameToBit(c);
  if (mb) {
    if (!strcmp(a,"down"))      { g_heldMods |= mb;  sendHID(); }
    else if (!strcmp(a,"up"))   { g_heldMods &= ~mb; sendHID(); }
    else { g_heldMods |= mb; sendHID(); vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS));
           g_heldMods &= ~mb; sendHID(); }
    return;
  }

  // 3) Touche fonctionnelle nommee
  uint8_t k = nameToHid(c);
  if (!k) { notifyStatus("err:unmapped"); return; }
  uint8_t em = maskToHid(mask);
  if (!strcmp(a,"down"))      { g_heldMods |= em; holdKey(k); }
  else if (!strcmp(a,"up"))   { unholdKey(k); g_heldMods &= ~em; sendHID(); }
  else                        { tapKey(k, em); }   // tap (defaut)
}

// ===========================================================================
//  Sequences (socle §5.3, spec S3 §5.3) — executees dans la tache worker,
//  horloge locale : la latence BLE n'influe pas sur le tempo.
// ===========================================================================
static void tapCombo(uint8_t mods, uint8_t key) { tapKey(key, mods); }

static bool runNamedSeq(const char* n) {
  if      (!strcmp(n,"ctrl_alt_del")) tapCombo(KM_CTRL|KM_ALT, 0x4C);
  else if (!strcmp(n,"ctrl_esc"))     tapCombo(KM_CTRL,        0x29);
  else if (!strcmp(n,"alt_tab"))      tapCombo(KM_ALT,         0x2B);
  else if (!strcmp(n,"alt_f4"))       tapCombo(KM_ALT,         0x3D);
  else if (!strcmp(n,"win_d"))        tapCombo(KM_GUI,         0x07);
  else return false;
  return true;
}

// Attente interruptible (verifie g_stop par tranches).
static void waitMs(uint32_t ms) {
  uint32_t step = 20;
  while (ms > 0 && !g_stop) {
    uint32_t d = ms < step ? ms : step;
    vTaskDelay(pdMS_TO_TICKS(d));
    ms -= d;
  }
}

// Frappe d'une etape "tap" : 1 seul point de code -> char, sinon touche nommee.
static void stepTap(const char* val, int mask) {
  if (!val || !*val) return;
  size_t len = strlen(val), i = 0;
  uint32_t cp = utf8Decode(val, len, &i);
  if (i >= len) {                       // un seul point de code -> caractere
    if (!typeChar(cp, maskToHid(mask))) notifyStatus("err:unmapped");
  } else {                              // plusieurs -> nom de touche (Enter, F1...)
    handleKey(val, "tap", mask);
  }
}

static void runCustomSeq(JsonArray steps) {
  for (JsonObject st : steps) {
    if (g_stop) break;
    if (st["wait"].is<long>()) {
      waitMs((uint32_t) st["wait"].as<long>());
    } else if (st["rep"].is<int>()) {
      int rep = st["rep"].as<int>();
      uint32_t every = st["every"].is<long>() ? (uint32_t) st["every"].as<long>() : 0;
      const char* tap = st["tap"] | "";
      int m = st["m"] | 0;
      for (int r = 0; r < rep && !g_stop; r++) {
        stepTap(tap, m);
        if (r < rep - 1) waitMs(every);
      }
    } else if (st["tap"].is<const char*>() || st["tap"].is<int>()) {
      char tmp[16];
      const char* tap;
      if (st["tap"].is<int>()) { snprintf(tmp,sizeof(tmp),"%d",st["tap"].as<int>()); tap = tmp; }
      else tap = st["tap"] | "";
      stepTap(tap, st["m"] | 0);
    }
  }
}

// ===========================================================================
//  Commandes locales : cfg (parametres) et pair (appairage)
// ===========================================================================
static bool jsonFlag(JsonVariant v, bool cur) {
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<int>())  return v.as<int>() != 0;
  return cur;
}

static void handleCfg(JsonDocument& doc) {
  const char* a = doc["a"] | "get";
  if (!strcmp(a, "set")) {
    g_cfg.hidKb  = jsonFlag(doc["hid_kb"], g_cfg.hidKb);
    g_cfg.hidMs  = jsonFlag(doc["hid_ms"], g_cfg.hidMs);
    g_cfg.serial = jsonFlag(doc["serial"], g_cfg.serial);
    g_cfg.gpio   = jsonFlag(doc["gpio"],   g_cfg.gpio);
    g_cfg.pair   = jsonFlag(doc["pair"],   g_cfg.pair);
    cfgSave();
    DBGLN("[CFG] sauvegarde, redemarrage");
    rebootWithStatus("cfg:saved");
    return;
  }
  char js[STATUS_MAX - 4];
  cfgToJson(js, sizeof(js), g_bleMac, g_linkUp, g_linkRssi);
  char out[STATUS_MAX];
  snprintf(out, sizeof(out), "cfg:%s", js);
  notifyStatus(out);
}

static void handlePair(JsonDocument& doc) {
  const char* a = doc["a"] | "";
  if (!strcmp(a, "scan"))   { linkScan(); return; }
  if (!strcmp(a, "bind"))   { linkBind(doc["mac"] | ""); return; }
  if (!strcmp(a, "unbind")) { linkUnbind(); return; }
  if (!strcmp(a, "slave")) {                       // ordre recu du futur maitre
    uint8_t m[6];
    if (!g_cfg.pair || g_cfg.role == ROLE_MASTER || !cfgParseMac(doc["mac"] | "", m)) { notifyStatus("err:pair"); return; }
    memcpy(g_cfg.peer, m, 6);
    g_cfg.role = ROLE_SLAVE;
    cfgSave();
    DBG("[PAIR] esclave de %s, redemarrage\n", (const char*)(doc["mac"] | ""));
    rebootWithStatus("pair:ok");
    return;
  }
  if (!strcmp(a, "reset")) {                       // retour au mode standard (aussi via COM)
    g_cfg.role = ROLE_STD;
    memset(g_cfg.peer, 0, 6);
    cfgSave();
    DBGLN("[PAIR] reset -> mode standard, redemarrage");
    rebootWithStatus("pair:ok");
    return;
  }
  notifyStatus("err:pair");
}

// ===========================================================================
//  Dispatch d'une commande JSON (execute dans la tache worker)
// ===========================================================================
static void processCommand(const char* json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) { notifyStatus("err:json"); return; }

  const char* t = doc["t"] | "";

  // --- Toujours locales : cfg / pair ---
  if (!strcmp(t, "cfg"))  { handleCfg(doc);  return; }
  if (!strcmp(t, "pair")) { handlePair(doc); return; }

  // --- MAITRE : tout le reste est transfere tel quel a l'esclave ---
  if (g_cfg.role == ROLE_MASTER) {
    if (!g_linkUp) { notifyStatus("err:nolink"); return; }
    linkForward(json, !strcmp(t, "txt") || !strcmp(t, "seq"));
    return;
  }

  if (!strcmp(t, "ping")) {
    char s[24]; snprintf(s, sizeof(s), "pong:%ld", (long)(doc["n"] | 0L));
    notifyStatus(s);

  } else if (!strcmp(t, "gpio")) {
    if (!g_cfg.gpio) { notifyStatus("err:gpio"); return; }
    gpioHandle(doc);

  } else if (!strcmp(t, "char")) {
    if (!g_kb) { notifyStatus("err:nohid"); return; }
    const char* v = doc["v"] | "";
    if (!*v) { notifyStatus("err:empty"); return; }
    size_t len = strlen(v), i = 0;
    uint32_t cp = utf8Decode(v, len, &i);
    if (!typeChar(cp, maskToHid(doc["m"] | 0))) notifyStatus("err:unmapped");

  } else if (!strcmp(t, "txt")) {
    if (!g_kb) { notifyStatus("err:nohid"); return; }
    const char* v = doc["v"] | "";
    notifyStatus("busy");
    typeString(v);
    notifyStatus("ready");

  } else if (!strcmp(t, "key")) {
    if (!g_kb) { notifyStatus("err:nohid"); return; }
    handleKey(doc["c"] | "", doc["a"] | "tap", doc["m"] | 0);

  } else if (!strcmp(t, "seq")) {
    const char* n = doc["n"] | "";
    if (!strcmp(n, "stop")) { releaseAll(); notifyStatus("ready"); return; }
    if (!g_kb) { notifyStatus("err:nohid"); return; }
    notifyStatus("busy");
    if (*n) {
      if (!runNamedSeq(n)) notifyStatus("err:seq");
    } else if (doc["s"].is<JsonArray>()) {
      runCustomSeq(doc["s"].as<JsonArray>());
    } else {
      notifyStatus("err:seq");
    }
    releaseAll();                        // surete de fin de sequence
    notifyStatus("ready");

  } else if (!strcmp(t, "mouse")) {
    handleMouse(doc);

  } else {
    notifyStatus("err:type");
  }
}

// ===========================================================================
//  Enfilage d'une commande brute — PARTAGE par BLE, Wi-Fi et COM.
//  Garde-fou taille + STOP prioritaire hors-file. Les transports ne font que
//  l'appeler ; l'execution (USB HID) reste au seul worker.
// ===========================================================================
static void enqueueCommand(const uint8_t* data, size_t len) {
  if (!data || !len) return;
  if (len > CMD_MAX_BYTES) { notifyStatus("err:toolong"); return; }

  // Detection d'un STOP prioritaire : l'appliquer tout de suite (S3 §5.3), sans
  // le mettre en file derriere une sequence en cours. Serialise BLE vs Wi-Fi.
  bool isStop = false;
  for (size_t i = 0; i + 4 <= len; i++)
    if (memcmp(data + i, "stop", 4) == 0) { isStop = true; break; }
  if (isStop) {
    if (g_stopMux) xSemaphoreTake(g_stopMux, portMAX_DELAY);
    g_stop = true;
    char* p;
    while (xQueueReceive(g_cmdQueue, &p, 0) == pdTRUE) free(p);
    releaseAll();
    if (g_stopMux) xSemaphoreGive(g_stopMux);
    if (g_cfg.role == ROLE_MASTER) {           // STOP transfere en priorite a l'esclave
      linkPurgeTx();
      linkForward("{\"t\":\"seq\",\"n\":\"stop\"}", true);
    }
    notifyStatus("ready");
    return;
  }

  // Copie \0-terminee, mise en file (xQueueSend est thread-safe).
  char* buf = (char*)malloc(len + 1);
  if (!buf) { notifyStatus("err:mem"); return; }
  memcpy(buf, data, len);
  buf[len] = 0;
  if (xQueueSend(g_cmdQueue, &buf, 0) != pdTRUE) {
    free(buf);
    notifyStatus("err:busy");                  // file pleine
  }
}

// ===========================================================================
//  Tache worker : consomme la file, execute. Seul thread qui touche l'USB HID.
// ===========================================================================
static void workerTask(void* arg) {
  for (;;) {
    char* buf = nullptr;
    if (xQueueReceive(g_cmdQueue, &buf, portMAX_DELAY) == pdTRUE && buf) {
      g_stop = false;
      processCommand(buf);
      free(buf);
    }
  }
}

// ===========================================================================
//  Desappairage physique : 5 appuis sur BOOT (GPIO0) en < 3 s
// ---------------------------------------------------------------------------
//  Tache autonome (toujours active, independante du role et du flag GPIO).
//  Chaque appui = impulsion violette ; au 5e :
//    - MAITRE  -> {"t":"pair","a":"unbind"} (libere l'esclave puis se reset)
//    - ESCLAVE -> {"t":"pair","a":"reset"}  (revient en standard)
//    - standard: rien (deja libre) -> simple accuse orange.
//  On ne fait qu'ENFILER dans g_cmdQueue : c'est le worker qui execute (les
//  handlers cfg/pair y sont locaux, meme en mode maitre).
// ===========================================================================
static void bootResetTask(void*) {
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);            // BOOT : bouton vers GND (actif bas)
  uint8_t  taps = 0;
  uint32_t firstMs = 0, lastEdge = 0;
  bool pressed = false;
  for (;;) {
    bool now = (digitalRead(RESET_BTN_PIN) == LOW);
    uint32_t t = millis();
    if (now != pressed && (t - lastEdge) > 30) {   // anti-rebond 30 ms
      pressed = now; lastEdge = t;
      if (pressed) {                                // front descendant = 1 appui
        if (taps == 0 || (t - firstMs) > BOOT_RESET_WIN_MS) { taps = 0; firstMs = t; }
        taps++;
        ledPulse(C_VIOLET, 60);                     // retour visuel par appui
        if (taps >= BOOT_RESET_TAPS) {
          taps = 0;
          if (g_cfg.role == ROLE_MASTER) {
            const char* j = "{\"t\":\"pair\",\"a\":\"unbind\"}";
            enqueueCommand((const uint8_t*)j, strlen(j));
          } else if (g_cfg.role == ROLE_SLAVE) {
            const char* j = "{\"t\":\"pair\",\"a\":\"reset\"}";
            enqueueCommand((const uint8_t*)j, strlen(j));
          } else {
            ledPulse(C_ORANGE, 200);                // deja standard : rien a defaire
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ===========================================================================
//  Callbacks BLE (serveur)
// ===========================================================================
// NB : le coeur esp32 3.x construit la bibliotheque BLE sur NimBLE (sdkconfig
// CONFIG_NIMBLE_ENABLED) ; les surcharges « avec descripteur » sont donc celles
// de NimBLE (ble_gap_conn_desc), pas celles de Bluedroid.
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s, ble_gap_conn_desc* d) override {
    BLEAddress remote(d->peer_ota_addr);
    // ESCLAVE : seul le maitre appaire est accepte (la whitelist filtre deja
    // a l'annonce ; ceci est la ceinture + bretelles).
    if (g_cfg.role == ROLE_SLAVE && !remote.equals(BLEAddress(g_cfg.peer))) {
      DBG("[BLE] client refuse (%s n'est pas le maitre)\n", remote.toString().c_str());
      s->disconnect(d->conn_handle);
      return;
    }
    g_connected = true;
    DBG("[BLE] client connecte %s\n", remote.toString().c_str());
    ledSetMode(ledBaseMode());
    ledPulse(C_GREEN, 90);             // confirmation de connexion
    notifyStatus("ready");
  }
  void onDisconnect(BLEServer* s) override {
    g_connected = false;
    DBGLN("[BLE] client deconnecte");
    ledSetMode(ledBaseMode());
    ledPulse(C_CYAN, 140);             // marque la deconnexion, retour au repos
    g_stop = true;
    // Vide la file (et libere les buffers) pour ne rien taper apres coup.
    if (g_stopMux) xSemaphoreTake(g_stopMux, portMAX_DELAY);
    char* p;
    while (xQueueReceive(g_cmdQueue, &p, 0) == pdTRUE) free(p);
    releaseAll();                        // relachement de surete (socle §5.1)
    if (g_stopMux) xSemaphoreGive(g_stopMux);
    BLEDevice::startAdvertising();       // re-annonce
  }
  void onMtuChanged(BLEServer* s, ble_gap_conn_desc* d, uint16_t mtu) override {
    DBG("[BLE] MTU=%u\n", mtu);      // cfg:{...} (~140 o) exige MTU >= 150
  }
};

class CmdCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    enqueueCommand(c->getData(), c->getLength());   // meme point d'entree que le Wi-Fi / COM
  }
};

// ===========================================================================
//  Init USB : interfaces selon la config (construites AVANT USB.begin()).
//  Exige « USB CDC On Boot = Disabled » (sinon le coeur a deja appele
//  USB.begin() dans app_main et rien de ce qui suit n'entre au descripteur).
//  Numero de serie derive des flags -> Windows re-enumere proprement chaque
//  combinaison au lieu de reutiliser un descripteur mis en cache.
// ===========================================================================
static void usbBegin() {
  static char serial[16];
  snprintf(serial, sizeof(serial), "S3KBD-%c%c%c",
           g_cfg.hidKb ? 'K' : 'x', g_cfg.hidMs ? 'M' : 'x', g_cfg.serial ? 'S' : 'x');
  USB.VID(KBD_VID);
  USB.PID(KBD_PID);
  USB.productName(KBD_PRODUCT);
  USB.manufacturerName("POC");
  USB.serialNumber(serial);
  if (g_cfg.hidKb) {                     // clavier construit en premier : protocole boot de l'interface HID
    g_kb = new USBHIDKeyboard();
    g_cc = new USBHIDConsumerControl();
    g_kb->begin();
    g_cc->begin();
  }
  if (g_cfg.hidMs) { g_mouse = new USBHIDMouse(); g_mouse->begin(); }
  if (g_cfg.serial) comBegin();
  USB.begin();
  DBG("[USB] serie %s : clavier=%d souris=%d COM(protocole)=%d\n", serial, g_cfg.hidKb, g_cfg.hidMs, g_cfg.serial);
}

// ===========================================================================
//  Init BLE serveur (service HID-Bridge). Esclave : whitelist du maitre.
// ===========================================================================
static bool bleBegin() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  char name[24];
  snprintf(name, sizeof(name), "%s-%02X%02X", BLE_NAME, mac[4], mac[5]);

  BLEDevice::init(name);
  BLEDevice::setMTU(517);                // negociation MTU eleve (socle §5.4)
  strncpy(g_bleMac, BLEDevice::getAddress().toString().c_str(), sizeof(g_bleMac) - 1);

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCB());

  BLEService* svc = g_server->createService(SERVICE_UUID);

  g_cmdChar = svc->createCharacteristic(
      CMD_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  g_cmdChar->setCallbacks(new CmdCB());

  g_statusChar = svc->createCharacteristic(
      STATUS_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  g_statusChar->addDescriptor(new BLE2902());
  g_statusChar->setValue("ready");

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);     // filtrage requestDevice sur le service
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  if (g_cfg.role == ROLE_SLAVE) {        // seul le maitre peut se connecter
    // BLEDevice::whiteListAdd() ne linke pas sur le coeur 3.3.x (m_whiteList
    // declare sans definition) : appel direct de la whitelist NimBLE.
    // ble_addr_t.val est en ordre inverse de la notation aa:bb:cc:dd:ee:ff.
    ble_addr_t wl; wl.type = BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; i++) wl.val[i] = g_cfg.peer[5 - i];
    int rc = ble_gap_wl_set(&wl, 1);
    if (rc) DBG("[BLE] whitelist : erreur rc=%d\n", rc);
    adv->setScanFilter(false, true);     // scan-requests de tous, connexions whitelist seulement
    char pm[18]; cfgMacStr(g_cfg.peer, pm);
    DBG("[BLE] ESCLAVE : connexions limitees au maitre %s\n", pm);
  }
  BLEDevice::startAdvertising();
  DBG("[BLE] annonce '%s' (%s) — service %s\n", name, g_bleMac, SERVICE_UUID);

  return g_server && svc && g_cmdChar && g_statusChar;
}

// ===========================================================================
//  setup / loop
// ===========================================================================
void setup() {
  Console.begin(115200);                         // CDC 0 : console (le port n'existe qu'apres USB.begin)

  // --- LED d'etat (tache dediee) : auto-test puis fond BOOT ---
  ledBegin();

  // --- Parametres persistants (AVANT l'USB : ils choisissent les interfaces) ---
  cfgLoad();

  // --- File + tache worker (avant l'USB, pour ne rien perdre) ---
  g_cmdQueue = xQueueCreate(CMD_QUEUE_LEN, sizeof(char*));
  g_stopMux  = xSemaphoreCreateMutex();          // avant l'USB/BLE (les callbacks l'utilisent)
  xTaskCreatePinnedToCore(workerTask, "worker", 8192, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(bootResetTask, "boot", 2560, nullptr, 2, nullptr, 0);  // 5 appuis BOOT = desappairage

  // --- USB (console + HID clavier / souris + COM selon config), puis USB.begin() ---
  usbBegin();
  delay(300);                                    // laisse l'hote enumerer avant les 1res traces
  DBGLN("\n[S3-KBD] demarrage (mock)");
  {
    char pm[18] = "-"; if (cfgPeerValid()) cfgMacStr(g_cfg.peer, pm);
    DBG("[CFG] kb=%d ms=%d serial=%d gpio=%d pair=%d role=%s peer=%s\n",
                  g_cfg.hidKb, g_cfg.hidMs, g_cfg.serial, g_cfg.gpio, g_cfg.pair,
                  g_cfg.role == ROLE_MASTER ? "MAITRE" : g_cfg.role == ROLE_SLAVE ? "ESCLAVE" : "standard", pm);
  }

  // --- BLE serveur GATT ---
  bool bleOk = bleBegin();

  // --- GPIO (pas sur le maitre : les GPIO pilotes sont ceux de l'esclave) ---
  if (g_cfg.gpio && g_cfg.role != ROLE_MASTER) gpioBegin();

  // --- Transport Wi-Fi : jamais sur un esclave ---
  if (g_cfg.role != ROLE_SLAVE) wifiPortalBegin();
  else DBGLN("[WiFi] desactive (esclave)");

  // --- Lien vers l'esclave (maitre) ---
  if (g_cfg.role == ROLE_MASTER) linkBegin();

  // --- Etat final : repos ou erreur si le GATT a echoue ---
  if (!bleOk) {
    DBGLN("[BLE] ERREUR init GATT");
    ledSetError(1);                    // 1 clignotement rouge = echec BLE
  } else {
    ledSetMode(ledBaseMode());
  }

  DBG("[HEAP] free=%u internal=%u psram=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  DBGLN("[S3-KBD] pret.");
}

void loop() {
  // Tout se passe dans les taches ; ici : mesure de heap periodique (§ risques).
  vTaskDelay(pdMS_TO_TICKS(10000));
  DBG("[HEAP] free=%u internal=%u minint=%u psram=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
