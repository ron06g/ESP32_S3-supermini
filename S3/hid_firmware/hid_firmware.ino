// ===========================================================================
//  hid_firmware.ino  —  Lot S3 « HID-Bridge » (clavier + souris HID sans fil)
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
//  clavier/souris, CDC protocole) sont choisies d'apres la config NVS lue dans
//  setup() et doivent etre construites AVANT USB.begin() (le descripteur est fige
//  a ce moment). Le sketch appelle donc lui-meme USB.begin() (usbBegin()).
//
//  Le peripherique agit comme un clavier/souris USB : il envoie de vraies frappes
//  a la machine cible. Comme tout clavier, ne le connecter qu'a des machines de
//  confiance. CMD exige un lien BLE chiffre + authentifie (garde-fou).
// ===========================================================================

#include "USB.h"
#include "USBCDC.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDMouse.h"
#include <ArduinoJson.h>
#include <nvs_flash.h>                 // nvs_flash_erase() : reset d'usine (BOOT 20 s)

// Console de DEBUG supprimee : un module neuf n'expose que le HID (clavier/souris).
// L'unique CDC (interface 0) n'est cree QUE si le flag `serial` est actif, et sert
// alors UNIQUEMENT le protocole (port COM, cf. com_port.h). Aucune sortie de debug.

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>                // LESC : passkey statique, iocap, bonding (NimBLE)
#include <host/ble_gap.h>               // ble_gap_conn_desc, ble_gap_unpair (pile NimBLE du coeur 3.x)
#include <host/ble_store.h>             // ble_store_clear (effacement des bonds)
#include "esp_mac.h"
#include "esp_heap_caps.h"              // mesures de heap (interne / PSRAM)

#include "keymap_azerty.h"

// ---------------------------------------------------------------------------
//  Contrat d'interface — UUIDs du service HID-Bridge (socle §5.1)
// ---------------------------------------------------------------------------
#define SERVICE_UUID  "9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define CMD_UUID      "9f1d0001-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define STATUS_UUID   "9f1d0002-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define PROV_UUID     "9f1d0003-5b8e-4a4a-9c2a-2b7f3e6a1001"   // provisioning d'un esclave (bind)
#define BLE_NAME      "S3-KBD"          // annonce : nom convivial, sinon S3-KBD-XXYY (2 derniers octets MAC)

// ---------------------------------------------------------------------------
//  Reglages
// ---------------------------------------------------------------------------
#define STATUS_MAX 512                              // taille max d'une frame STATUS JSON (cfg + slaves[] + noms). MTU 517 -> tient en 1 PDU
static const uint16_t KBD_VID = 0x303A;             // Espressif
static const uint16_t KBD_PID = 0x8161;             // PID stable
static const char*    KBD_PRODUCT = "S3-KBD";

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
static const uint32_t BOOT_FACTORY_MS   = 20000;   // BOOT maintenu 20 s = reset d'usine

// --- Declarations avancees partagees par les modules (.h) ---
static void enqueueCommand(const uint8_t* data, size_t len);
static void releaseAll();
static void notifyStatus(const char* s);   // adaptateur : texte legacy OU JSON -> diffusion
static void statusRaw(const char* s);       // diffusion brute d'une frame deja construite (JSON)
static void rebootWithStatus(const char* s);
static void secClearBonds();                // efface TOUS les bonds BLE (changement de passkey / reset)
static void secUnpairMac(const uint8_t* mac);   // efface le bond d'un pair (desappairage d'un esclave)

#include "config.h"                    // parametres NVS (g_cfg)

// DEBUG retire du firmware : DBG/DBGLN sont des no-op (aucune console USB). Les
// appels restent en place mais ne produisent rien (le compilateur les elimine).
#define DBG(...)  do {} while (0)
#define DBGLN(s)  do {} while (0)
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
BLECharacteristic* g_provChar   = nullptr;   // provisioning (bind) : présent seulement si role==STD && pair
BLEServer*         g_server     = nullptr;
static char        g_bleMac[18] = "";

uint8_t g_myId = 0;                  // id de CETTE carte : 0 (maitre/standard) ou selfId (esclave)
volatile bool g_connected = false;   // un client BLE (telephone, ou le maitre si esclave)
static volatile uint16_t g_connHandle = BLE_HS_CONN_HANDLE_NONE;  // esclave : lien vers le maitre (RSSI LED)
volatile bool g_stop      = false;   // demande d'arret de sequence
QueueHandle_t g_cmdQueue  = nullptr; // file de char* (JSON \0-termine, malloc)
SemaphoreHandle_t g_stopMux = nullptr; // serialise le fast-path STOP entre BLE et Wi-Fi

uint8_t g_heldMods    = 0;           // modificateurs maintenus (Ctrl/Shift/...)
uint8_t g_heldKeys[6] = {0};         // touches non-modif maintenues (down/up)

// Etat LED « de fond » selon le role et les connexions.
static LedMode ledBaseMode() {
  if (g_cfg.role == ROLE_SLAVE)  return g_connected ? LST_SLAVE_LINKED : LST_SLAVE_WAIT;
  if (g_cfg.role == ROLE_MASTER) return linkAnyUp() ? LST_CONNECTED    : LST_IDLE;
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
// Diffusion BRUTE : `s` est deja la frame finale (JSON). Aucun reflet LED ni
// interpretation -> sert a relayer VERBATIM les STATUS d'un esclave (deja
// tagues de son id) et est appelee par l'adaptateur notifyStatus.
static void statusRaw(const char* s) {
  if (g_statusChar) {
    g_statusChar->setValue((uint8_t*)s, strlen(s));
    if (g_connected) g_statusChar->notify();   // garde g_connected : specifique BLE
  }
  wifiQueueStatus(s);                          // diffusion Wi-Fi (drainee par la tache reseau)
  comQueueStatus(s);                           // diffusion port COM (drainee par la tache com)
  DBG("[STATUS] %s\n", s);
}

// Adaptateur : accepte les chaines LEGACY (ready/busy/err:*/pong:*/gpio:*/
// pair:ok/cfg:saved) encore produites par le dispatch, gpio_panel, com_port…
// et les convertit en JSON {"id":g_myId,...}. Une frame commencant par '{' est
// deja du JSON (relais esclave / helper) et passe telle quelle. Reflete la LED.
static void notifyStatus(const char* s) {
  if (!s) return;
  if (s[0] == '{') { statusRaw(s); return; }

  if      (!strncmp(s, "err", 3)) ledPulse(C_ORANGE, 160);
  else if (!strcmp(s, "busy"))    ledSetMode(LST_BUSY);
  else if (!strcmp(s, "ready"))   ledSetMode(ledBaseMode());

  char out[STATUS_MAX];
  if      (!strcmp(s, "ready") || !strcmp(s, "busy"))
    snprintf(out, sizeof(out), "{\"id\":%u,\"st\":\"%s\"}", g_myId, s);
  else if (!strncmp(s, "err:", 4))
    snprintf(out, sizeof(out), "{\"id\":%u,\"err\":\"%s\"}", g_myId, s + 4);
  else if (!strncmp(s, "pong:", 5))
    snprintf(out, sizeof(out), "{\"id\":%u,\"ev\":\"pong\",\"n\":%s}", g_myId, s + 5);
  else if (!strncmp(s, "gpio:", 5)) {
    const char* body  = s + 5;
    const char* colon = strrchr(body, ':');    // dernier ':' : le label peut valoir "BOOT"
    if (colon) snprintf(out, sizeof(out), "{\"id\":%u,\"ev\":\"gpio\",\"p\":\"%.*s\",\"v\":%d}",
                        g_myId, (int)(colon - body), body, atoi(colon + 1));
    else       snprintf(out, sizeof(out), "{\"id\":%u,\"st\":\"%s\"}", g_myId, s);
  }
  else if (!strcmp(s, "pair:ok"))   snprintf(out, sizeof(out), "{\"id\":0,\"ev\":\"pair\",\"ok\":true}");
  else if (!strcmp(s, "cfg:saved")) snprintf(out, sizeof(out), "{\"id\":0,\"ev\":\"cfg\",\"saved\":true}");
  else                              snprintf(out, sizeof(out), "{\"id\":%u,\"st\":\"%s\"}", g_myId, s);

  statusRaw(out);
}

// Emet un statut, laisse le temps aux transports de l'ecouler, redemarre.
static void rebootWithStatus(const char* s) {
  notifyStatus(s);
  vTaskDelay(pdMS_TO_TICKS(300));       // laisse le temps d'ecouler le statut (BLE/WS/COM)
  ESP.restart();
}

// ===========================================================================
//  Securite (phase LESC) — effacement des bonds
// ---------------------------------------------------------------------------
//  La LTK d'un bond ne derive PAS de la passkey : changer la passkey n'invalide
//  aucun bond existant. Il faut donc effacer explicitement les bonds pour que la
//  nouvelle passkey soit exercee au prochain appairage. API NimBLE directe
//  (meme pattern que ble_gap_wl_set / ble_gap_conn_rssi ailleurs dans ce sketch).
// ===========================================================================
static void secClearBonds() {
  int rc = ble_store_clear();
  DBG("[SEC] bonds effaces (rc=%d)\n", rc);
}

// Reset d'usine (BOOT maintenu 20 s) : efface TOUTE la NVS (config s3kbd : passkey,
// noms, paramètres, table d'appairage — ET les bonds BLE nimble). Le LOGICIEL est
// conservé (partition app intacte) ; au reboot, cfgLoad repart des valeurs par défaut.
// L'appelant redémarre juste après (nvs_flash_erase invalide les handles NVS).
static void cfgFactoryReset() {
  nvs_flash_erase();
  DBGLN("[RESET] NVS effacee (reset d'usine) -> redemarrage");
}

// Efface le bond d'un pair unique (MAC publique en notation aa:bb:..). Utilise au
// desappairage d'un esclave cote maitre. ble_addr_t.val est en ordre INVERSE.
static void secUnpairMac(const uint8_t* mac) {
  ble_addr_t a; a.type = BLE_ADDR_PUBLIC;
  for (int i = 0; i < 6; i++) a.val[i] = mac[5 - i];
  int rc = ble_gap_unpair(&a);
  DBG("[SEC] unpair %02x:%02x:..:%02x rc=%d\n", mac[0], mac[1], mac[5], rc);
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

// Reponse a {"t":"cfg","a":"get"} : config + table de routage des esclaves
// (maitre), avec l'etat de lien courant (up/rssi) pris dans le runtime g_link[].
static void statusCfg() {
  JsonDocument d;
  d["id"]     = 0;
  d["ev"]     = "cfg";
  d["hid_kb"] = g_cfg.hidKb ? 1 : 0;
  d["hid_ms"] = g_cfg.hidMs ? 1 : 0;
  d["serial"] = g_cfg.serial ? 1 : 0;
  d["gpio"]   = g_cfg.gpio ? 1 : 0;
  d["pair"]   = g_cfg.pair ? 1 : 0;
  d["ble"]    = g_cfg.ble ? 1 : 0;
  d["wifi"]   = g_cfg.wifi ? 1 : 0;
  d["boot5"]  = g_cfg.boot5 ? 1 : 0;
  d["bootrst"]= g_cfg.bootRst ? 1 : 0;
  d["role"]   = g_cfg.role;
  d["mac"]    = g_bleMac;
  d["self"]   = g_cfg.selfId;
  d["name"]   = g_cfg.name;                          // nom convivial du module ("" = defaut)
  d["pkset"]  = g_cfg.passkey ? 1 : 0;               // passkey personnalisee ? (jamais la valeur)
  d["wifiset"]= (strcmp(g_cfg.apPsk, AP_PSK_DEFAULT) != 0) ? 1 : 0;   // PSK Wi-Fi personnalise ?
  char peer[18] = "";
  if (cfgPeerValid()) cfgMacStr(g_cfg.peer, peer);   // esclave : MAC de son maitre
  d["peer"]   = peer;
  JsonArray sl = d["slaves"].to<JsonArray>();
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (!g_cfg.slaves[i].id) continue;
    JsonObject o = sl.add<JsonObject>();
    uint8_t id = g_cfg.slaves[i].id;
    o["id"] = id;
    char m[18]; cfgMacStr(g_cfg.slaves[i].mac, m);
    o["mac"]  = m;
    o["name"] = g_cfg.slaves[i].name;                // nom convivial de l'esclave ("" = defaut)
    o["up"]   = (g_cfg.role == ROLE_MASTER && g_link[id - 1].up) ? 1 : 0;
    o["rssi"] = (g_cfg.role == ROLE_MASTER) ? g_link[id - 1].rssi : 0;
  }
  char out[STATUS_MAX];
  serializeJson(d, out, sizeof(out));
  statusRaw(out);
}

static void handleCfg(JsonDocument& doc) {
  const char* a = doc["a"] | "get";
  if (!strcmp(a, "set")) {
    g_cfg.hidKb  = jsonFlag(doc["hid_kb"], g_cfg.hidKb);
    g_cfg.hidMs  = jsonFlag(doc["hid_ms"], g_cfg.hidMs);
    g_cfg.serial = jsonFlag(doc["serial"], g_cfg.serial);
    g_cfg.gpio   = jsonFlag(doc["gpio"],   g_cfg.gpio);
    g_cfg.pair   = jsonFlag(doc["pair"],   g_cfg.pair);
    g_cfg.ble    = jsonFlag(doc["ble"],    g_cfg.ble);
    g_cfg.wifi   = jsonFlag(doc["wifi"],   g_cfg.wifi);
    g_cfg.boot5  = jsonFlag(doc["boot5"],  g_cfg.boot5);
    g_cfg.bootRst= jsonFlag(doc["bootrst"],g_cfg.bootRst);
    cfgSave();
    DBGLN("[CFG] sauvegarde, redemarrage");
    rebootWithStatus("cfg:saved");
    return;
  }
  statusCfg();
}

// ===========================================================================
//  Securite (phase LESC) : {"t":"sec","a":"passkey|wifi|get",...} — TOUJOURS locale
// ---------------------------------------------------------------------------
//  passkey : refuse si des esclaves sont appaires (regle) ; sinon ecrit la
//            nouvelle passkey, EFFACE les bonds (sinon l'ancienne LTK reste
//            valable) et redemarre. Cote telephone : « oublier » le module.
//  wifi    : change la cle WPA2 du SoftAP (>= 8 car.) et redemarre.
//  get     : renvoie SEULEMENT des indicateurs (jamais la passkey ni le PSK).
// ===========================================================================
static void handleSec(JsonDocument& doc) {
  const char* a = doc["a"] | "get";
  if (!strcmp(a, "passkey")) {
    if (g_cfg.role == ROLE_MASTER && slaveCount() > 0) { notifyStatus("err:slaves"); return; }
    long pk = -1;
    if      (doc["pk"].is<int>())          pk = doc["pk"].as<long>();
    else if (doc["pk"].is<const char*>())  pk = atol(doc["pk"].as<const char*>());   // "000042" -> 42
    if (pk < 0 || !secPasskeyValid((uint32_t)pk)) { notifyStatus("err:sec"); return; }
    g_cfg.passkey = (uint32_t)pk;
    cfgSave();
    secClearBonds();                       // force un re-appairage avec la nouvelle passkey
    DBGLN("[SEC] passkey changee, bonds effaces, redemarrage");
    rebootWithStatus("{\"id\":0,\"ev\":\"sec\",\"ok\":true}");
    return;
  }
  if (!strcmp(a, "wifi")) {
    const char* psk = doc["psk"] | "";
    if (!secWifiPskValid(psk)) { notifyStatus("err:sec"); return; }
    strlcpy(g_cfg.apPsk, psk, sizeof(g_cfg.apPsk));
    cfgSave();
    DBGLN("[SEC] cle Wi-Fi changee, redemarrage");
    rebootWithStatus("{\"id\":0,\"ev\":\"sec\",\"ok\":true}");
    return;
  }
  char s[80];
  snprintf(s, sizeof(s), "{\"id\":0,\"ev\":\"sec\",\"pkset\":%d,\"wifiset\":%d}",
           g_cfg.passkey ? 1 : 0, strcmp(g_cfg.apPsk, AP_PSK_DEFAULT) ? 1 : 0);
  statusRaw(s);
}

// ===========================================================================
//  Renommage : {"t":"name","name":"…"} — nom convivial persistant.
// ---------------------------------------------------------------------------
//  id 0 / absent : renomme CE module. Le nom est AUSSI l'annonce BLE (fixée au
//                  boot) -> standalone/maître REDÉMARRE (event "reboot":true) pour
//                  la rafraîchir ; un esclave (rename routé du maître) NE redémarre
//                  PAS (garder le lien) — son annonce importe peu (whitelist).
//  maitre + id!=0 : met a jour la table locale (slaves[].name) ET route la
//                   commande a l'esclave (qui persiste le sien, sans reboot).
//  Serialisation via ArduinoJson (echappe les caracteres speciaux du nom).
// ===========================================================================
static void handleName(JsonDocument& doc) {
  int id = doc["id"] | 0;
  const char* nm = doc["name"] | "";
  if (g_cfg.role == ROLE_MASTER && id != 0) {
    int idx = slaveIndexById((uint8_t)id);
    if (id < 1 || id > MAX_SLAVES || idx < 0) { notifyStatus("err:id"); return; }
    slaveSetName((uint8_t)id, nm);
    cfgSave();
    if (linkUp((uint8_t)id)) {                       // repercute chez l'esclave (locale chez lui)
      JsonDocument fwd; fwd["t"] = "name"; fwd["name"] = g_cfg.slaves[idx].name;
      char j[96]; serializeJson(fwd, j, sizeof(j));
      linkForward((uint8_t)id, j, true);
    }
    JsonDocument ev; ev["id"] = id; ev["ev"] = "name"; ev["name"] = g_cfg.slaves[idx].name;
    char s[128]; serializeJson(ev, s, sizeof(s)); statusRaw(s);
    return;
  }
  strlcpy(g_cfg.name, nm, NAME_MAX);
  cfgSave();
  JsonDocument ev; ev["id"] = g_myId; ev["ev"] = "name"; ev["name"] = g_cfg.name;
  // Le nom convivial est AUSSI le nom annoncé en BLE (fixé à bleBegin, donc au
  // boot). EXCEPTION : un esclave reçoit un rename ROUTÉ de son maître — ne pas
  // redémarrer (cela casserait le lien) ; son annonce importe peu (whitelist
  // maître) et le maître garde le nom en cache dans slaves[].
  char s[128];
  if (g_cfg.role == ROLE_SLAVE) { serializeJson(ev, s, sizeof(s)); statusRaw(s); return; }
  // standalone / maître : redémarrer pour que le scan et le sélecteur d'appareil
  // montrent le nouveau nom annoncé.
  ev["reboot"] = true;
  serializeJson(ev, s, sizeof(s));
  rebootWithStatus(s);
}

static void handlePair(JsonDocument& doc) {
  const char* a = doc["a"] | "";
  if (!strcmp(a, "scan"))   { linkScan(); return; }
  if (!strcmp(a, "bind"))   { linkBind(doc["mac"] | "", doc["name"] | ""); return; }
  if (!strcmp(a, "unbind")) { linkUnbind((uint8_t)(doc["id"] | 0)); return; }   // id==0 = tous
  if (!strcmp(a, "slave")) {                       // ordre de provisioning recu du futur maitre (via PROV)
    uint8_t m[6];
    if (!g_cfg.pair || g_cfg.role == ROLE_MASTER || !cfgParseMac(doc["mac"] | "", m)) { notifyStatus("err:pair"); return; }
    memcpy(g_cfg.peer, m, 6);
    g_cfg.role   = ROLE_SLAVE;
    g_cfg.selfId = (uint8_t)(doc["id"] | 1);       // id sous lequel le maitre me verra
    long pk = doc["pk"].is<int>() ? doc["pk"].as<long>() : -1;   // passkey PARTAGEE du maitre
    if (pk >= 0 && secPasskeyValid((uint32_t)pk)) g_cfg.passkey = (uint32_t)pk;
    cfgSave();
    // NB : on N'efface PAS les bonds ici (le lien bootstrap est encore actif et
    // porte l'ack). Au reboot, le maitre a deja efface SON bond (linkBind) et
    // reinitie un pairing MITM neuf ; le bond bootstrap obsolete cote esclave est
    // alors remplace via REPEAT_PAIRING. Pas de risque de bond « Just Works » residuel.
    DBG("[PAIR] esclave id=%u de %s, passkey recue, redemarrage\n", g_cfg.selfId, (const char*)(doc["mac"] | ""));
    rebootWithStatus("pair:ok");
    return;
  }
  if (!strcmp(a, "reset")) {                       // retour au mode standard (aussi via COM / 5xBOOT)
    g_cfg.role    = ROLE_STD;
    g_cfg.selfId  = 0;
    g_cfg.passkey = 0;                             // passkey usine 000000
    memset(g_cfg.peer, 0, 6);
    memset(g_cfg.slaves, 0, sizeof(g_cfg.slaves));
    // g_cfg.name CONSERVE : repere pour un futur re-appairage manuel.
    cfgSave();
    secClearBonds();                               // libere aussi les bonds
    DBGLN("[PAIR] reset -> mode standard (nom conserve), redemarrage");
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

  // --- Toujours locales, quel que soit l'id : cfg / pair / sec / name ---
  //     (name gere lui-meme le routage maitre->esclave : ne pas le laisser au
  //      routage verbatim generique ci-dessous, qui ne mettrait pas a jour la
  //      table locale du maitre.)
  if (!strcmp(t, "cfg"))  { handleCfg(doc);  return; }
  if (!strcmp(t, "pair")) { handlePair(doc); return; }
  if (!strcmp(t, "sec"))  { handleSec(doc);  return; }
  if (!strcmp(t, "name")) { handleName(doc); return; }

  // --- Routage par id : 0 = cette carte (maitre/standard/esclave local),
  //     1..MAX_SLAVES = esclave route par le maitre. id absent => 0. ---
  int id = doc["id"] | 0;
  if (g_cfg.role == ROLE_MASTER && id != 0) {
    if (id < 1 || id > MAX_SLAVES || slaveIndexById((uint8_t)id) < 0) { notifyStatus("err:id"); return; }
    if (!linkUp((uint8_t)id)) {
      char e[40]; snprintf(e, sizeof(e), "{\"id\":%d,\"err\":\"nolink\"}", id);
      statusRaw(e);
      return;
    }
    linkForward((uint8_t)id, json, !strcmp(t, "txt") || !strcmp(t, "seq"));
    return;
  }
  // id == 0 (ou standard / esclave) : execution locale ci-dessous.

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
    if (g_cfg.role == ROLE_MASTER) {           // STOP diffuse en priorite a TOUS les esclaves
      linkPurgeAllTx();
      linkBroadcastStop();
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
  uint32_t firstMs = 0, lastEdge = 0, pressStart = 0;
  bool pressed = false, longFired = false;
  for (;;) {
    bool now = (digitalRead(RESET_BTN_PIN) == LOW);
    uint32_t t = millis();
    if (now != pressed && (t - lastEdge) > 30) {   // anti-rebond 30 ms
      pressed = now; lastEdge = t;
      if (pressed) {                                // front descendant = 1 appui
        pressStart = t; longFired = false;          // démarre le chrono du maintien long
        if (g_cfg.boot5) {                          // BOOT ×5 = désappairage (si activé)
          if (taps == 0 || (t - firstMs) > BOOT_RESET_WIN_MS) { taps = 0; firstMs = t; }
          taps++;
          ledPulse(C_VIOLET, 60);                   // retour visuel par appui
          if (taps >= BOOT_RESET_TAPS) {
            taps = 0;
            if (g_cfg.role == ROLE_MASTER) {
              const char* j = "{\"t\":\"pair\",\"a\":\"unbind\"}";
              enqueueCommand((const uint8_t*)j, strlen(j));
            } else if (g_cfg.role == ROLE_SLAVE) {
              const char* j = "{\"t\":\"pair\",\"a\":\"reset\"}";
              enqueueCommand((const uint8_t*)j, strlen(j));
            } else {
              ledPulse(C_ORANGE, 200);              // deja standard : rien a defaire
            }
          }
        }
      }
    }
    // Maintien long (20 s) = RESET D'USINE (config + bonds), logiciel conservé. Si activé.
    if (g_cfg.bootRst && pressed && !longFired && (t - pressStart) >= BOOT_FACTORY_MS) {
      longFired = true;
      ledSetError(4);                               // signal visuel (4 clignotements)
      cfgFactoryReset();
      rebootWithStatus("{\"id\":0,\"ev\":\"sys\",\"factory\":true}");
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
    if (g_cfg.role == ROLE_SLAVE) g_connHandle = d->conn_handle;
    DBG("[BLE] client connecte %s\n", remote.toString().c_str());
    ledSetMode(ledBaseMode());
    ledPulse(C_GREEN, 90);             // confirmation de connexion
    notifyStatus("ready");
  }
  void onDisconnect(BLEServer* s) override {
    g_connected = false;
    g_connHandle = BLE_HS_CONN_HANDLE_NONE;
    ledSetRssi(0);
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

// PROV : canal de provisioning d'un esclave (bind). Chiffrement ENC seul (un
// bootstrap Just Works le satisfait), present uniquement sur un module vierge
// (role==STD && pair). Ne recoit que l'ordre {"t":"pair","a":"slave",...} : on
// l'enfile comme une commande normale (handlePair est local, le worker execute).
class ProvCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    enqueueCommand(c->getData(), c->getLength());
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
  USB.manufacturerName("S3-KBD");
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
  // Nom annonce : nom convivial si defini (repere au scan / au ré-appairage),
  // sinon defaut S3-KBD-XXYY (2 derniers octets MAC). Garder des noms UNIQUES.
  char name[32];
  if (g_cfg.name[0]) strlcpy(name, g_cfg.name, sizeof(name));
  else               snprintf(name, sizeof(name), "%s-%02X%02X", BLE_NAME, mac[4], mac[5]);

  BLEDevice::init(name);

  // --- Securite LESC (APRES init : init() reinitialise ble_hs_cfg) ---
  //  bonding + MITM + Secure Connections ; passkey statique auto-injectee.
  //  iocap GLOBALE par role : standard/maitre = DisplayOnly (le telephone SAISIT
  //  la passkey ; l'esclave l'auto-injecte cote lien) ; esclave = KeyboardOnly
  //  -> couple maitre(Display)/esclave(Keyboard) = Passkey-Entry MITM automatique.
  BLESecurity::setAuthenticationMode(true, true, true);          // bond, mitm, sc
  BLESecurity::setCapability(g_cfg.role == ROLE_SLAVE ? ESP_IO_CAP_IN : ESP_IO_CAP_OUT);
  BLESecurity::setPassKey(true, g_cfg.passkey);                  // statique (0 = 000000)

  // Conso/chaleur : en mode standard (telephone a courte portee) on baisse la
  // puissance BLE ; en appaire (maitre/esclave) le lien BLE<->BLE reste a fond.
  if (g_cfg.role == ROLE_STD) BLEDevice::setPower(ESP_PWR_LVL_N0);   // 0 dBm : couvre une piece
  else                        BLEDevice::setPower(ESP_PWR_LVL_P9);   // +9 dBm : portee max du lien
  BLEDevice::setMTU(517);                // negociation MTU eleve (socle §5.4)
  strncpy(g_bleMac, BLEDevice::getAddress().toString().c_str(), sizeof(g_bleMac) - 1);

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCB());

  BLEService* svc = g_server->createService(SERVICE_UUID);

  // CMD (canal de frappes) : chiffrement ET authentification MITM exiges
  // (WRITE_AUTHEN). Un pair non appaire (ou en Just Works) ne peut RIEN ecrire
  // -> garde-fou anti-injection non autorisee. NB : setAccessPermissions() est un no-op sous
  // NimBLE ; ce sont les bits de propriete qui portent l'exigence de securite.
  g_cmdChar = svc->createCharacteristic(
      CMD_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
      | BLECharacteristic::PROPERTY_WRITE_AUTHEN);
  g_cmdChar->setCallbacks(new CmdCB());

  // STATUS : notify, lecture/souscription chiffree (READ_ENC) -> l'abonnement
  // CCCD declenche l'appairage si le lien n'est pas chiffre.
  g_statusChar = svc->createCharacteristic(
      STATUS_UUID,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ_ENC);
  g_statusChar->addDescriptor(new BLE2902());
  g_statusChar->setValue("{\"id\":0,\"st\":\"ready\"}");   // valeur initiale (frame JSON)

  // PROV : provisioning d'un esclave. Present UNIQUEMENT sur un module vierge et
  // disponible (role STD + pair). WRITE_ENC : un bootstrap Just Works suffit (le
  // maitre s'appaire avant d'ecrire) -> la passkey poussee circule sur lien chiffre.
  if (g_cfg.role == ROLE_STD && g_cfg.pair) {
    g_provChar = svc->createCharacteristic(
        PROV_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_ENC);
    g_provChar->setCallbacks(new ProvCB());
  }

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
  // Annonce le service (visible d'un contrôleur BLE) seulement si `ble` est actif.
  // EXCEPTIONS : un esclave annonce toujours (pour son maître) ; un module vierge
  // en appairage annonce pour se faire provisionner. Un maître avec `ble` coupé
  // garde sa pile BLE (client vers ses esclaves) mais N'ANNONCE PAS → aucun
  // contrôleur (smartphone/OS) ne peut s'y connecter.
  bool doAdvertise = g_cfg.ble || g_cfg.role == ROLE_SLAVE || (g_cfg.role == ROLE_STD && g_cfg.pair);
  if (doAdvertise) {
    BLEDevice::startAdvertising();
    DBG("[BLE] annonce '%s' (%s) — service %s\n", name, g_bleMac, SERVICE_UUID);
  } else {
    DBGLN("[BLE] pile active (liens) mais SANS annonce (contrôleur BLE désactivé)");
  }

  return g_server && svc && g_cmdChar && g_statusChar;
}

// ===========================================================================
//  setup / loop
// ===========================================================================
void setup() {
  setCpuFrequencyMhz(160);                       // 240->160 MHz : moitie moins de chaleur CPU, large pour cet usage

  // --- LED d'etat (tache dediee) : auto-test puis fond BOOT ---
  ledBegin();

  // --- Parametres persistants (AVANT l'USB : ils choisissent les interfaces) ---
  cfgLoad();
  g_myId = (g_cfg.role == ROLE_SLAVE) ? g_cfg.selfId : 0;   // id de cette carte (tag des STATUS)

  // --- File + tache worker (avant l'USB, pour ne rien perdre) ---
  g_cmdQueue = xQueueCreate(CMD_QUEUE_LEN, sizeof(char*));
  g_stopMux  = xSemaphoreCreateMutex();          // avant l'USB/BLE (les callbacks l'utilisent)
  xTaskCreatePinnedToCore(workerTask, "worker", 8192, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(bootResetTask, "boot", 2560, nullptr, 2, nullptr, 0);  // 5 appuis BOOT = desappairage

  // --- USB (console + HID clavier / souris + COM selon config), puis USB.begin() ---
  usbBegin();
  delay(300);                                    // laisse l'hote enumerer avant les 1res traces
  DBGLN("\n[S3-KBD] demarrage");
  {
    char pm[18] = "-"; if (cfgPeerValid()) cfgMacStr(g_cfg.peer, pm);
    DBG("[CFG] kb=%d ms=%d serial=%d gpio=%d pair=%d role=%s peer=%s\n",
                  g_cfg.hidKb, g_cfg.hidMs, g_cfg.serial, g_cfg.gpio, g_cfg.pair,
                  g_cfg.role == ROLE_MASTER ? "MAITRE" : g_cfg.role == ROLE_SLAVE ? "ESCLAVE" : "standard", pm);
  }
  DBG("[PWR] cpu=%u MHz\n", getCpuFrequencyMhz());

  // --- BLE serveur GATT : radio allumée seulement si utile ---
  // ble (contrôleur smartphone/OS) OU pair (provisioning + liens) OU rôle
  // maître/esclave (lien étoile). Sinon — module autonome avec BLE et appairage
  // coupés — l'ANTENNE BLE RESTE ÉTEINTE (conso / chaleur / surface d'attaque).
  bool bleNeeded = g_cfg.ble || g_cfg.pair || g_cfg.role != ROLE_STD;
  bool bleOk = true;
  if (bleNeeded) bleOk = bleBegin();

  // --- GPIO (pas sur le maitre : les GPIO pilotes sont ceux de l'esclave) ---
  // GPIO locaux : le maitre gere DESORMAIS ses propres GPIO (id 0), en plus de
  // router les GPIO des esclaves. Actifs sur tous les roles si le flag est mis.
  if (g_cfg.gpio) gpioBegin();

  // --- Transport Wi-Fi : jamais sur un esclave, et seulement si le flag wifi est actif ---
  if (g_cfg.role != ROLE_SLAVE && g_cfg.wifi) wifiPortalBegin();

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
  // Tout se passe dans les taches ; ici : RSSI du lien (esclave, pour la LED)
  // toutes les secondes et mesure de heap periodique (§ risques).
  static uint8_t tick = 0;
  vTaskDelay(pdMS_TO_TICKS(1000));
  uint16_t h = g_connHandle;
  int8_t rssi;
  if (h != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_rssi(h, &rssi) == 0 && rssi < 0)
    ledSetRssi(rssi);                  // intensite LED verte = force du signal
  if (++tick < 10) return;
  tick = 0;
  DBG("[HEAP] free=%u internal=%u minint=%u psram=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
