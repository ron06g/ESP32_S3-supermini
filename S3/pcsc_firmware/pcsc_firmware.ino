// ===========================================================================
//  pcsc_firmware.ino  —  Lecteur de Carte Vitale (ESP32-S3 N8R2)
// ---------------------------------------------------------------------------
//  Firmware INDEPENDANT du firmware HID (S3/hid_firmware), dedie a la carte
//  ESP32-S3-WROOM-1 N8R2 « 2 USB-C » :
//    - prise « USB » (USB natif GPIO19/20) en USB HOST : on y branche, via un
//      adaptateur OTG, un lecteur de carte a puce CCID (« PC/SC », classe 0x0B) ;
//    - prise « COM » (pont CH343 sur UART0, GPIO43/44) : port COM texte
//      <STX>TEXTE<CR><LF><EOT> a 115200 8N1 (protocole : com_proto.h, README.md) ;
//    - LED RGB WS2812 (GPIO48) : etat du lecteur et de la carte (status_led.h).
//
//  Le module lit les donnees PUBLIQUES d'une Carte Vitale 2 (nom, prenom, date
//  de naissance, NIR) et les annonce sur le port COM. Preuve de faisabilite :
//  le transfert Base64 des donnees brutes viendra ensuite.
//
//  Sobriete : BLE et Wi-Fi ne sont JAMAIS initialises (aucune bibliotheque radio
//  incluse : le coeur libere la memoire du controleur BT), CPU a 80 MHz (FQBN
//  CPUFreq=80 ; pas en dessous : la PLL est necessaire a l'USB), carte a puce
//  coupee apres chaque lecture.
//
//  Sans lecteur, une NOUVELLE RECHERCHE est lancee toutes les 30 s (coupure
//  logique du port racine -> re-enumeration ; LED bleue en respiration rapide
//  pendant ~3 s). Mode DEBUG (commande DEBUG) : journal detaille sur le COM
//  (debug_log.h).
//
//  Taches (un seul proprietaire par ressource) :
//    - usbLib : usb_host_lib_handle_events() (enumeration, pile USB Host) ;
//    - usbCli : usb_host_client_handle_events() (callbacks client + transferts) ;
//    - ccid   : SEUL proprietaire du lecteur : evenements USB (g_usbEvtQueue),
//               commandes COM (g_cmdQueue), scrutation de la carte, lecture,
//               recherche periodique, journal DEBUG du port ;
//    - com    : lecture du port COM, decoupage des trames, postage des commandes ;
//    - led    : animation de la LED d'etat.
//  Deux files distinctes : un flot de commandes ne peut jamais evincer un
//  evenement USB (debranchement du lecteur). Les reponses restent dans l'ordre
//  des evenements (une seule tache les emet).
//
//  FQBN (cf. pcsc_firmware_compile.bat) :
//    esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,CPUFreq=80,
//      FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled,DebugLevel=none
//  USB.begin() n'est JAMAIS appele (CDC On Boot = Disabled) : le controleur
//  USB-OTG reste libre pour usb_host_install().
// ===========================================================================

#include <Arduino.h>
#include "esp_log.h"

#define FW_BANNER        "CARD READER v1.0"
#define POLL_MS          250       // periode de scrutation de la carte (GetSlotStatus)
#define BOOT_GRACE_MS    2500      // delai laisse a l'enumeration avant READER_KO
#define RESCAN_MS        30000     // sans lecteur : nouvelle recherche toutes les 30 s
#define SCAN_WINDOW_MS   2500      // enumeration attendue apres une recherche
#define PORT_OFF_MS      500       // duree de la coupure logique du port racine
#define DBG_BEAT_MS      10000     // mode DEBUG : etat periodique
#define RESULT_MAX       1024      // trame resultat (CARD_READ:...) en cache

// --- Files de la tache ccid -------------------------------------------------
enum : uint8_t { MSG_NEW_DEV, MSG_DEV_GONE };
struct UsbEvt {
  uint8_t type;      // MSG_*
  uint8_t addr;      // adresse USB (MSG_NEW_DEV)
  void*   hdl;       // usb_device_handle_t (MSG_DEV_GONE)
};
enum : uint8_t { CMD_RESTART, CMD_IS_READER, CMD_IS_CARD, CMD_READ_CARD,
                 CMD_CARD_STATE, CMD_INFO, CMD_DEBUG, CMD_DEBUG_ON, CMD_DEBUG_OFF,
                 CMD_UNKNOWN };
static QueueHandle_t g_usbEvtQueue = nullptr;   // UsbEvt  (callbacks USB)
static QueueHandle_t g_cmdQueue    = nullptr;   // uint8_t (tache com)
static TaskHandle_t  g_ccidTask    = nullptr;

// Reveille la tache ccid apres un postage (sinon elle attend sa scrutation).
static inline void ccidWake() {
  if (g_ccidTask) xTaskNotifyGive(g_ccidTask);
}

// --- Diagnostic (commande INFO), ecrit par la tache ccid uniquement ---------
struct DiagInfo {
  char     step[12];       // derniere etape en echec ("" = aucune)
  uint16_t sw;             // dernier SW carte (0 = aucun)
  uint32_t ms;             // duree de la derniere lecture
  uint8_t  atr[33];
  uint8_t  atrLen;
  char     lastDev[24];    // dernier peripherique USB vu / refuse (ENUM, NOT_CCID, CHAR)
};
static DiagInfo g_diag = {};

static void diagStep(const char* s) {
  strncpy(g_diag.step, s, sizeof(g_diag.step) - 1);
  g_diag.step[sizeof(g_diag.step) - 1] = 0;
}

#include "status_led.h"
#include "com_proto.h"
#include "debug_log.h"
#include "usb_ccid.h"
#include "apdu_t0.h"
#include "vitale.h"
#include "cartes.h"

// --- Etat du lecteur / de la carte (tache ccid uniquement) ------------------
static bool     g_readerOk   = false;   // lecteur CCID utilisable attache
static bool     g_cardIn     = false;   // presence telle qu'annoncee (CARD_IN / CARD_OUT)
static bool     g_graceOn    = false;   // delai de grace (boot / RESTART) en cours
static uint32_t g_graceEnd   = 0;
static bool     g_scanOn     = false;   // recherche periodique en cours (fenetre d'enumeration)
static uint32_t g_scanEnd    = 0;
static bool     g_rescanArmed = false;  // prochaine recherche programmee
static uint32_t g_nextScan   = 0;
static uint32_t g_lastPoll   = 0;
static uint8_t  g_lastIcc    = 0xFF;    // dernier bmICCStatus (journal DEBUG)
static uint32_t g_dbgHprt    = 0xFFFFFFFF;  // dernier etat du port journalise
static uint32_t g_dbgBeat    = 0;
static char     g_result[RESULT_MAX];   // dernier resultat de lecture ("" = aucun)

static void startGrace() {
  g_graceOn  = true;
  g_graceEnd = millis() + BOOT_GRACE_MS;
  ledSet(LED_BOOT);
}

// Remet le lecteur et la carte a l'etat « rien de connu » (sans rien annoncer).
static void resetState() {
  g_readerOk  = false;
  g_cardIn    = false;
  g_lastIcc   = 0xFF;
  g_result[0] = 0;
}

// LED sans lecteur : orange si un peripherique USB (non lecteur) est branche,
// rouge sinon. Interroge la liste de la pile (aucun trafic USB).
static void ledNoReader() {
  uint8_t addrs[8];
  int n = 0;
  if (g_usbHostOk) usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n);
  ledSet(n > 0 ? LED_BAD_DEVICE : LED_NO_READER);
}

// Coupure LOGIQUE du port racine (VBUS est cable en dur sur la carte) : tout
// appareil est deconnecte puis re-enumere.
static void usbPortCycle() {
  esp_err_t off = usb_host_lib_set_root_port_power(false);
  vTaskDelay(pdMS_TO_TICKS(PORT_OFF_MS));
  esp_err_t on = usb_host_lib_set_root_port_power(true);
  dbg("PORT : coupure logique du port racine (off=%s, on=%s)", esp_err_to_name(off), esp_err_to_name(on));
}

// Retente les peripheriques deja enumeres (filet de securite si un NEW_DEV a
// ete manque). Renvoie true si un lecteur est maintenant pret.
static bool attachExisting() {
  if (!g_usbHostOk) return false;
  uint8_t addrs[8];
  int n = 0;
  if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) != ESP_OK) return false;
  dbg("USB : %d peripherique(s) enumere(s) deja present(s)", n);
  for (int i = 0; i < n; ++i)
    if (ccidAttach(addrs[i])) return true;
  return false;
}

// --- Recherche periodique sans lecteur ---------------------------------------
static void armRescan() {
  if (!g_usbHostOk || g_usbStuck) return;
  g_rescanArmed = true;
  g_nextScan    = millis() + RESCAN_MS;
  dbg("RECHERCHE : prochaine tentative dans %u s", RESCAN_MS / 1000);
}

static void startScan() {
  g_rescanArmed = false;
  ledSet(LED_SCAN);
  dbg("RECHERCHE : nouvelle tentative (coupure du port + %u ms d'enumeration)", SCAN_WINDOW_MS);
  usbPortCycle();
  g_scanOn  = true;
  g_scanEnd = millis() + SCAN_WINDOW_MS;
}

static void endScan() {
  g_scanOn = false;
  if (g_readerOk) return;
  if (attachExisting()) {
    g_readerOk = true;
    comSend("READER_OK");
    ledSet(LED_READER);
    g_lastPoll = 0;
    return;
  }
  dbg("RECHERCHE : aucun lecteur");
  ledNoReader();
  armRescan();
}

// --- Lecture / scrutation ---------------------------------------------------
// Lit la carte, la COUPE ensuite (sobriete ; un slot « presente inactive » est
// l'etat de repos), met le resultat en cache et l'annonce.
static void readCardAndReport() {
  ledSet(LED_READING);
  dbg("LECTURE : debut");
  if (g_cardPowered) ccidPowerOff();
  uint32_t t0 = millis();
  int res = vitaleRead(g_result, sizeof(g_result));
  g_diag.ms = millis() - t0;
  ccidPowerOff();
  dbg("LECTURE : fin en %lu ms (etape en echec : %s)", (unsigned long)g_diag.ms,
      g_diag.step[0] ? g_diag.step : "aucune");
  comSend(g_result);
  ledSet(res == VIT_OK ? LED_CARD : res == VIT_NOT_CV ? LED_CARD_NOT_CV : LED_CARD_ERROR);
}

// Scrutation de presence. Renvoie true si une insertion vient d'etre annoncee
// et lue (CARD_IN + resultat deja emis).
static bool pollCard() {
  if (!g_readerOk) return false;
  uint8_t icc;
  g_dbgQuiet = true;                                // pas de vidage hexa toutes les 250 ms
  int r = ccidSlotStatus(&icc);
  g_dbgQuiet = false;
  if (r != CC_OK) return false;                     // erreur passagere ou lecteur parti (DEV_GONE suit)
  if (icc != g_lastIcc) {
    static const char* const ICC[] = { "carte active", "carte presente inactive", "pas de carte", "?" };
    dbg("SLOT : bmICCStatus %u (%s)", icc, ICC[icc & 3]);
    g_lastIcc = icc;
  }
  bool present = (icc != 2);
  if (present && !g_cardIn) {
    g_cardIn = true;
    comSend("CARD_IN");
    ledSet(LED_CARD);
    readCardAndReport();
    return true;
  }
  if (!present && g_cardIn) {
    g_cardIn    = false;
    g_result[0] = 0;
    comSend("CARD_OUT");
    ledSet(LED_READER);
  }
  return false;
}

// Fin du delai de grace : READER_KO si aucun lecteur ne s'est annonce.
static void endGrace() {
  g_graceOn = false;
  if (g_readerOk) return;
  g_readerOk = attachExisting();
  comSend(g_readerOk ? "READER_OK" : "READER_KO");
  if (g_readerOk) {
    ledSet(LED_READER);
    g_lastPoll = 0;
  } else {
    ledNoReader();
    armRescan();
  }
}

// RESTART : reinitialise le LECTEUR USB (pas l'ESP32). Coupure logique du port
// racine puis meme sequence qu'au demarrage. Dernier recours si la pile USB est
// absente ou bloquee (transfert jamais rendu) : redemarrage complet de l'ESP32
// (la banniere suivra le log ROM).
static void doRestart() {
  if (g_usbStuck || !g_usbHostOk) {
    Serial0.flush();
    esp_restart();
  }
  dbg("RESTART : reinitialisation du lecteur USB");
  if (g_readerOk && g_cardPowered) ccidPowerOff();
  ccidDetach();
  usbPortCycle();
  resetState();
  g_scanOn      = false;
  g_rescanArmed = false;
  g_diag        = {};
  comSend(FW_BANNER);
  startGrace();
}

// Un transfert USB n'a jamais ete rendu : le lecteur est inutilisable jusqu'a
// RESTART (qui redemarre alors l'ESP32). On le dit une fois.
static void checkStuck() {
  if (!g_usbStuck || !g_readerOk) return;
  bool hadCard = g_cardIn;
  resetState();
  if (hadCard) comSend("CARD_OUT");
  comSend("READER_KO");
  ledSet(LED_USB_FAULT);
}

static void handleUsbEvt(const UsbEvt& ev) {
  if (ev.type == MSG_NEW_DEV) {
    dbg("USB : NEW_DEV adresse %u", ev.addr);
    if (g_rd.open) return;                          // un seul lecteur gere
    if (ccidAttach(ev.addr)) {
      g_readerOk    = true;
      g_graceOn     = false;
      g_scanOn      = false;
      g_rescanArmed = false;
      comSend("READER_OK");
      ledSet(LED_READER);
      g_lastPoll = 0;                               // scrute la carte tout de suite
    } else if (!g_graceOn && !g_scanOn) {
      ledNoReader();                                // peripherique branche mais pas un lecteur
    }
  } else if (ev.type == MSG_DEV_GONE) {
    dbg("USB : DEV_GONE%s", (g_rd.open && ev.hdl == (void*)g_rd.dev) ? " (le lecteur)" : "");
    if (!g_rd.open || ev.hdl != (void*)g_rd.dev) return;
    bool wasOk = g_readerOk, hadCard = g_cardIn;
    ccidDetach();
    resetState();
    if (wasOk) {
      if (hadCard) comSend("CARD_OUT");
      comSend("READER_KO");
    }
    if (!g_graceOn) {
      ledNoReader();
      armRescan();
    }
  }
}

// --- Mode DEBUG ---------------------------------------------------------------
static void debugState() {
  long next = g_rescanArmed ? (long)(int32_t)(g_nextScan - millis()) / 1000 : -1;
  dbg("ETAT lecteur=%u carte=%u grace=%u recherche=%u prochaine_recherche=%lds bloque=%u led=%u",
      g_readerOk, g_cardIn, g_graceOn, g_scanOn, next, g_usbStuck, g_ledState);
}

// Instantane complet (a l'activation du mode DEBUG).
static void debugSnapshot() {
  dbg("FIRMWARE %s, compile le %s %s, CPU %lu MHz, tas libre %lu o", FW_BANNER, __DATE__, __TIME__,
      (unsigned long)getCpuFrequencyMhz(), (unsigned long)ESP.getFreeHeap());
  usbDebugSnapshot();
  debugState();
  if (g_rd.open)
    dbg("LECTEUR %04X:%04X '%s' niveau=%s dwFeatures=0x%08lX", g_rd.vid, g_rd.pid, g_rd.name,
        LVL_NAMES[g_rd.level], (unsigned long)g_rd.features);
  else if (g_diag.lastDev[0])
    dbg("DERNIER PERIPHERIQUE VU : %s", g_diag.lastDev);
  uint32_t h = usbHprt();
  if (g_usbHostOk && !(h & 1))
    dbg("DIAG : aucun appareil ELECTRIQUEMENT connecte (D+ et D- au repos). Un lecteur alimente "
        "tire D+ a l'etat haut : verifier le 5 V (VBUS) sur la prise USB / l'adaptateur OTG.");
  g_dbgHprt = h;
  g_dbgBeat = millis();
}

// Surveillance continue en mode DEBUG : changement d'etat electrique du port
// (branchement, D+/D-, activation) + etat periodique.
static void debugMonitor() {
  if (!g_debug || !g_usbHostOk) return;
  const uint32_t MASK = 0x1 | 0x4 | 0x10 | 0xC00 | 0x1000 | 0x60000;   // conn, ena, ovc, lnsts, pwr, spd
  uint32_t h = usbHprt();
  if ((h & MASK) != (g_dbgHprt & MASK)) {
    char port[120];
    usbPortStr(h, port, sizeof(port));
    dbg("%s", port);
    g_dbgHprt = h;
  }
  if (millis() - g_dbgBeat >= DBG_BEAT_MS) {
    g_dbgBeat = millis();
    usb_host_lib_info_t li = {};
    usb_host_lib_info(&li);
    char port[120];
    usbPortStr(h, port, sizeof(port));
    dbg("%s ; peripheriques=%d", port, li.num_devices);
    debugState();
  }
}

static void handleCommand(uint8_t cmd) {
  switch (cmd) {
    case CMD_RESTART:
      doRestart();
      break;
    case CMD_IS_READER:
      comSend(g_readerOk ? "READER_OK" : "READER_KO");
      break;
    case CMD_IS_CARD:
      comSend(g_cardIn ? "CARD_IN" : "CARD_OUT");
      break;
    case CMD_READ_CARD:
      if (!g_readerOk) { comSend("NO_CARD"); break; }
      if (pollCard()) break;                        // insertion a l'instant : CARD_IN + resultat deja emis
      if (!g_cardIn) comSend("NO_CARD");
      else readCardAndReport();                     // relecture forcee
      break;
    case CMD_CARD_STATE:
      if (!g_cardIn) comSend("NO_CARD");
      else if (g_result[0]) comSend(g_result);
      else readCardAndReport();
      break;
    case CMD_INFO: {
      static char info[256];
      ccidInfo(info, sizeof(info));
      comSend(info);
      break;
    }
    case CMD_DEBUG:
    case CMD_DEBUG_ON:
    case CMD_DEBUG_OFF: {
      bool on = (cmd == CMD_DEBUG) ? !g_debug : (cmd == CMD_DEBUG_ON);
      dbgSet(on);
      comSend(on ? "DEBUG_ON" : "DEBUG_OFF");
      if (on) debugSnapshot();
      break;
    }
    default:
      comSend("UNKNOWN_CMD");
      break;
  }
}

static void ccidTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
    usbDebugEnumDrain();
    // Evenements USB d'abord, puis chaque commande (un debranchement survenu
    // entre-temps est vu avant de repondre).
    for (;;) {
      UsbEvt ev;
      while (xQueueReceive(g_usbEvtQueue, &ev, 0) == pdTRUE) handleUsbEvt(ev);
      uint8_t cmd;
      if (xQueueReceive(g_cmdQueue, &cmd, 0) != pdTRUE) break;
      handleCommand(cmd);
      checkStuck();
    }
    if (g_graceOn && (int32_t)(millis() - g_graceEnd) >= 0) endGrace();
    if (g_scanOn && (int32_t)(millis() - g_scanEnd) >= 0) endScan();
    if (!g_readerOk && !g_graceOn && !g_scanOn && g_rescanArmed &&
        (int32_t)(millis() - g_nextScan) >= 0)
      startScan();
    if (millis() - g_lastPoll >= POLL_MS) {
      g_lastPoll = millis();
      if (g_readerOk) pollCard();
      else if (!g_graceOn && !g_scanOn) ledNoReader();   // suit le branchement d'un peripherique non lecteur
    }
    checkStuck();
    debugMonitor();
  }
}

void setup() {
  // Aucun log de l'IDF ni du coeur sur UART0 hors mode DEBUG : c'est le port
  // protocole (en DEBUG, les logs IDF sont re-trames « DBG:IDF: »).
  dbgBegin();
  Serial0.setRxBufferSize(1024);
  Serial0.setTxBufferSize(2048);
  Serial0.begin(115200);
  Serial0.setDebugOutput(false);

  ledBegin();
  g_usbEvtQueue = xQueueCreate(8, sizeof(UsbEvt));
  g_cmdQueue    = xQueueCreate(16, sizeof(uint8_t));
  comBegin();
  comSend(FW_BANNER);

  startGrace();
  xTaskCreatePinnedToCore(ccidTask, "ccid", 8192, nullptr, 4, &g_ccidTask, 1);
  if (!usbHostBegin()) {                   // INFO le signale ; READER_KO a la fin du delai
    diagStep("USBHOST");
    ledSet(LED_USB_FAULT);
  }
}

void loop() {
  vTaskDelete(nullptr);    // tout se passe dans les taches
}
