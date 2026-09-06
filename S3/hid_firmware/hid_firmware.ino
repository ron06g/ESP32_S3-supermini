// ===========================================================================
//  hid_firmware.ino  —  Lot S3 « HID-Bridge » (MOCK / POC, sans securite)
// ---------------------------------------------------------------------------
//  ESP32-S3 (SuperMini) qui est SIMULTANEMENT :
//    - un clavier USB HID (+ Consumer Control) vu par la machine cible ;
//    - un serveur BLE GATT qui recoit les commandes du site web ;
//    - un traducteur commande -> frappe (disposition AZERTY, sequences autonomes).
//
//  Contrat GATT : socle §5.  Table AZERTY : keymap_azerty.h (spec S3 §6).
//
//  Dependances :
//    - Coeur Arduino-ESP32 >= 2.0.14 (recommande) — USB natif TinyUSB.
//    - Bibliotheque ArduinoJson v7 (Gestionnaire de bibliotheques).
//
//  Reglages Arduino IDE (Outils) :
//    - Board            : "ESP32S3 Dev Module"
//    - USB Mode         : "USB-OTG (TinyUSB)"        <-- OBLIGATOIRE pour le HID
//    - USB CDC On Boot  : "Enabled"  (logs Serial sur l'USB natif)
//    - Upload Mode      : "UART0 / Hardware CDC"
//    - PSRAM            : selon la carte (N8R2 -> "QSPI PSRAM" / OPI si N16R8)
//
//  ATTENTION (socle §7 BadUSB) : ce peripherique EST un injecteur de frappes.
//  Tant que la securite n'est pas en place, ne le brancher que sur des machines
//  de confiance.
// ===========================================================================

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDMouse.h"
#include <ArduinoJson.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "keymap_azerty.h"
#include "status_led.h"

// ---------------------------------------------------------------------------
//  Contrat d'interface — UUIDs du service HID-Bridge (socle §5.1)
// ---------------------------------------------------------------------------
#define SERVICE_UUID  "9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define CMD_UUID      "9f1d0001-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define STATUS_UUID   "9f1d0002-5b8e-4a4a-9c2a-2b7f3e6a1001"
#define BLE_NAME      "S3-KBD"

// ---------------------------------------------------------------------------
//  Reglages
// ---------------------------------------------------------------------------
static const uint16_t KBD_VID = 0x303A;             // Espressif (dev)
static const uint16_t KBD_PID = 0x8161;             // PID de dev, stable
static const char*    KBD_PRODUCT = "S3-KBD (mock)";
static const char*    KBD_SERIAL  = "S3KBD-0001";   // serie stable -> pas de recache pilote Windows

static const uint32_t KEY_PRESS_MS   = 5;   // duree appui d'une touche
static const uint32_t KEY_GAP_MS     = 5;   // repos entre deux frappes
static const uint32_t TXT_INTERKEY_MS= 6;   // cadence inter-caractere du mode macro
static const size_t   CMD_QUEUE_LEN  = 16;  // profondeur de la file de commandes
static const size_t   CMD_MAX_BYTES  = 600; // garde-fou taille d'une commande

// ---------------------------------------------------------------------------
//  Objets USB
// ---------------------------------------------------------------------------
USBHIDKeyboard        Keyboard;
USBHIDConsumerControl Consumer;
USBHIDMouse           Mouse;

// ---------------------------------------------------------------------------
//  Etat global
// ---------------------------------------------------------------------------
BLECharacteristic* g_cmdChar    = nullptr;
BLECharacteristic* g_statusChar = nullptr;
BLEServer*         g_server     = nullptr;

volatile bool g_connected = false;
volatile bool g_stop      = false;   // demande d'arret de sequence
QueueHandle_t g_cmdQueue  = nullptr; // file de char* (JSON \0-termine, malloc)

uint8_t g_heldMods    = 0;           // modificateurs maintenus (Ctrl/Shift/...)
uint8_t g_heldKeys[6] = {0};         // touches non-modif maintenues (down/up)

// ===========================================================================
//  Bas niveau : emission des rapports HID clavier
// ===========================================================================
static void sendHID() {
  KeyReport kr = {0};
  kr.modifiers = g_heldMods;
  for (int i = 0; i < 6; i++) kr.keys[i] = g_heldKeys[i];
  Keyboard.sendReport(&kr);
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
  Keyboard.releaseAll();
  sendHID();
}

// ===========================================================================
//  STATUS (socle §5 / spec S3 §4)
// ===========================================================================
static void notifyStatus(const char* s) {
  // Reflet LED (point central) : busy/ready pilotent le fond, err:* fait une
  // impulsion orange transitoire sans figer la LED.
  if      (!strncmp(s, "err", 3)) ledPulse(C_ORANGE, 160);
  else if (!strcmp(s, "busy"))    ledSetMode(LST_BUSY);
  else if (!strcmp(s, "ready"))   ledSetMode(g_connected ? LST_CONNECTED : LST_IDLE);

  if (g_statusChar) {
    g_statusChar->setValue((uint8_t*)s, strlen(s));
    if (g_connected) g_statusChar->notify();
  }
  Serial.printf("[STATUS] %s\n", s);
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
  ledPulse(C_CYAN, 90);              // impulsion cyan pour la telecommande media
  Consumer.press(usage);
  vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS));
  Consumer.release();
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
    Mouse.move((int8_t)sx, (int8_t)sy, (int8_t)sw);
    dx -= sx; dy -= sy; w -= sw;
  }
}

static void handleMouse(JsonDocument& doc) {
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
    if      (!strcmp(a, "down")) Mouse.press(btn);
    else if (!strcmp(a, "up"))   Mouse.release(btn);
    else if (!strcmp(a, "dbl"))  { Mouse.click(btn); vTaskDelay(pdMS_TO_TICKS(40)); Mouse.click(btn); }
    else                         Mouse.click(btn);
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
//  Dispatch d'une commande JSON (execute dans la tache worker)
// ===========================================================================
static void processCommand(const char* json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) { notifyStatus("err:json"); return; }

  const char* t = doc["t"] | "";

  if (!strcmp(t, "char")) {
    const char* v = doc["v"] | "";
    if (!*v) { notifyStatus("err:empty"); return; }
    size_t len = strlen(v), i = 0;
    uint32_t cp = utf8Decode(v, len, &i);
    if (!typeChar(cp, maskToHid(doc["m"] | 0))) notifyStatus("err:unmapped");

  } else if (!strcmp(t, "txt")) {
    const char* v = doc["v"] | "";
    notifyStatus("busy");
    typeString(v);
    notifyStatus("ready");

  } else if (!strcmp(t, "key")) {
    handleKey(doc["c"] | "", doc["a"] | "tap", doc["m"] | 0);

  } else if (!strcmp(t, "seq")) {
    const char* n = doc["n"] | "";
    if (!strcmp(n, "stop")) { releaseAll(); notifyStatus("ready"); return; }
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
//  Callbacks BLE
// ===========================================================================
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    g_connected = true;
    Serial.println("[BLE] client connecte");
    ledSetMode(LST_CONNECTED);
    ledPulse(C_GREEN, 90);             // confirmation de connexion
    notifyStatus("ready");
  }
  void onDisconnect(BLEServer* s) override {
    g_connected = false;
    Serial.println("[BLE] client deconnecte");
    ledSetMode(LST_IDLE);
    ledPulse(C_CYAN, 140);             // marque la deconnexion, retour au repos
    g_stop = true;
    // Vide la file (et libere les buffers) pour ne rien taper apres coup.
    char* p;
    while (xQueueReceive(g_cmdQueue, &p, 0) == pdTRUE) free(p);
    releaseAll();                        // relachement de surete (socle §5.1)
    BLEDevice::startAdvertising();       // re-annonce
  }
};

class CmdCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    uint8_t* data = c->getData();
    size_t   len  = c->getLength();
    if (!data || !len) return;
    if (len > CMD_MAX_BYTES) { notifyStatus("err:toolong"); return; }

    // Detection d'un STOP prioritaire : ne pas le mettre en file derriere une
    // sequence en cours ; l'appliquer tout de suite (interruptibilite, S3 §5.3).
    bool isStop = false;
    for (size_t i = 0; i + 4 <= len; i++)
      if (memcmp(data + i, "stop", 4) == 0) { isStop = true; break; }
    if (isStop) {
      g_stop = true;
      char* p;
      while (xQueueReceive(g_cmdQueue, &p, 0) == pdTRUE) free(p);
      releaseAll();
      notifyStatus("ready");
      return;
    }

    // Copie \0-terminee, mise en file.
    char* buf = (char*)malloc(len + 1);
    if (!buf) { notifyStatus("err:mem"); return; }
    memcpy(buf, data, len);
    buf[len] = 0;
    if (xQueueSend(g_cmdQueue, &buf, 0) != pdTRUE) {
      free(buf);
      notifyStatus("err:busy");          // file pleine
    }
  }
};

// ===========================================================================
//  setup / loop
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[S3-KBD] demarrage (mock)");

  // --- LED d'etat (tache dediee) : auto-test puis fond BOOT ---
  ledBegin();

  // --- File + tache worker (avant l'USB, pour ne rien perdre) ---
  g_cmdQueue = xQueueCreate(CMD_QUEUE_LEN, sizeof(char*));
  xTaskCreatePinnedToCore(workerTask, "worker", 8192, nullptr, 5, nullptr, 1);

  // --- USB HID (clavier + consumer) ---
  USB.VID(KBD_VID);
  USB.PID(KBD_PID);
  USB.productName(KBD_PRODUCT);
  USB.manufacturerName("POC");
  USB.serialNumber(KBD_SERIAL);
  Keyboard.begin();
  Consumer.begin();
  Mouse.begin();
  USB.begin();
  Serial.println("[USB] HID clavier + consumer + souris prets");

  // --- BLE serveur GATT (service HID-Bridge) ---
  BLEDevice::init(BLE_NAME);
  BLEDevice::setMTU(517);                // negociation MTU eleve (socle §5.4)

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
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] annonce '%s' — service %s\n", BLE_NAME, SERVICE_UUID);

  // --- Etat final : repos (bleu clignotant) ou erreur si le GATT a echoue ---
  if (!g_server || !svc || !g_cmdChar || !g_statusChar) {
    Serial.println("[BLE] ERREUR init GATT");
    ledSetError(1);                    // 1 clignotement rouge = echec BLE
  } else {
    ledSetMode(LST_IDLE);
  }

  Serial.println("[S3-KBD] pret. En attente d'un client BLE...");
}

void loop() {
  // Tout se passe dans la tache worker et les callbacks BLE.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
