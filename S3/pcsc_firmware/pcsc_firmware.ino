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
//  Taches (un seul proprietaire par ressource) :
//    - usbLib : usb_host_lib_handle_events() (enumeration, pile USB Host) ;
//    - usbCli : usb_host_client_handle_events() (callbacks client + transferts) ;
//    - ccid   : SEUL proprietaire du lecteur : evenements USB (g_usbEvtQueue),
//               commandes COM (g_cmdQueue), scrutation de la carte, lecture ;
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
#define RESULT_MAX       1024      // trame resultat (CARD_READ:...) en cache

// --- Files de la tache ccid -------------------------------------------------
enum : uint8_t { MSG_NEW_DEV, MSG_DEV_GONE };
struct UsbEvt {
  uint8_t type;      // MSG_*
  uint8_t addr;      // adresse USB (MSG_NEW_DEV)
  void*   hdl;       // usb_device_handle_t (MSG_DEV_GONE)
};
enum : uint8_t { CMD_RESTART, CMD_IS_READER, CMD_IS_CARD, CMD_READ_CARD,
                 CMD_CARD_STATE, CMD_INFO, CMD_UNKNOWN };
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
  char     lastDev[24];    // dernier peripherique USB refuse (non CCID / niveau caractere)
};
static DiagInfo g_diag = {};

static void diagStep(const char* s) {
  strncpy(g_diag.step, s, sizeof(g_diag.step) - 1);
  g_diag.step[sizeof(g_diag.step) - 1] = 0;
}

#include "status_led.h"
#include "com_proto.h"
#include "usb_ccid.h"
#include "apdu_t0.h"
#include "vitale.h"

// --- Etat du lecteur / de la carte (tache ccid uniquement) ------------------
static bool     g_readerOk = false;     // lecteur CCID utilisable attache
static bool     g_cardIn   = false;     // presence telle qu'annoncee (CARD_IN / CARD_OUT)
static bool     g_graceOn  = false;     // delai de grace (boot / RESTART) en cours
static uint32_t g_graceEnd = 0;
static uint32_t g_lastPoll = 0;
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

// Lit la carte, la COUPE ensuite (sobriete ; un slot « presente inactive » est
// l'etat de repos), met le resultat en cache et l'annonce.
static void readCardAndReport() {
  ledSet(LED_READING);
  if (g_cardPowered) ccidPowerOff();
  uint32_t t0 = millis();
  int res = vitaleRead(g_result, sizeof(g_result));
  g_diag.ms = millis() - t0;
  ccidPowerOff();
  comSend(g_result);
  ledSet(res == VIT_OK ? LED_CARD : res == VIT_NOT_CV ? LED_CARD_NOT_CV : LED_CARD_ERROR);
}

// Scrutation de presence. Renvoie true si une insertion vient d'etre annoncee
// et lue (CARD_IN + resultat deja emis).
static bool pollCard() {
  if (!g_readerOk) return false;
  uint8_t icc;
  if (ccidSlotStatus(&icc) != CC_OK) return false;  // erreur passagere ou lecteur parti (DEV_GONE suit)
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

// Fin du delai de grace : READER_KO si aucun lecteur ne s'est annonce. Filet
// de securite : on retente les peripheriques deja enumeres (evenement manque).
static void endGrace() {
  g_graceOn = false;
  if (g_readerOk) return;
  if (g_usbHostOk) {
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK) {
      for (int i = 0; i < n && !g_readerOk; ++i) g_readerOk = ccidAttach(addrs[i]);
    }
  }
  comSend(g_readerOk ? "READER_OK" : "READER_KO");
  if (g_readerOk) ledSet(LED_READER);
  else ledNoReader();
}

// RESTART : reinitialise le LECTEUR USB (pas l'ESP32). Coupure logique du port
// racine (VBUS est cable en dur sur la carte : deconnexion + re-enumeration),
// puis meme sequence qu'au demarrage. Dernier recours si la pile USB est
// absente ou bloquee (transfert jamais rendu) : redemarrage complet de l'ESP32
// (la banniere suivra le log ROM).
static void doRestart() {
  if (g_usbStuck || !g_usbHostOk) {
    Serial0.flush();
    esp_restart();
  }
  if (g_readerOk && g_cardPowered) ccidPowerOff();
  ccidDetach();
  usb_host_lib_set_root_port_power(false);
  vTaskDelay(pdMS_TO_TICKS(500));
  usb_host_lib_set_root_port_power(true);
  resetState();
  g_diag = {};
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
    if (g_rd.open) return;                          // un seul lecteur gere
    if (ccidAttach(ev.addr)) {
      g_readerOk = true;
      g_graceOn  = false;
      comSend("READER_OK");
      ledSet(LED_READER);
      g_lastPoll = 0;                               // scrute la carte tout de suite
    } else if (!g_graceOn) {
      ledNoReader();                                // peripherique branche mais pas un lecteur
    }
  } else if (ev.type == MSG_DEV_GONE) {
    if (!g_rd.open || ev.hdl != (void*)g_rd.dev) return;
    bool wasOk = g_readerOk, hadCard = g_cardIn;
    ccidDetach();
    resetState();
    if (wasOk) {
      if (hadCard) comSend("CARD_OUT");
      comSend("READER_KO");
    }
    if (!g_graceOn) ledNoReader();
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
    default:
      comSend("UNKNOWN_CMD");
      break;
  }
}

static void ccidTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
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
    if (millis() - g_lastPoll >= POLL_MS) {
      g_lastPoll = millis();
      if (g_readerOk) pollCard();
      else if (!g_graceOn) ledNoReader();           // suit le branchement / retrait d'un peripherique non lecteur
    }
    checkStuck();
  }
}

void setup() {
  // Aucun log de l'IDF ni du coeur sur UART0 : c'est le port protocole.
  esp_log_level_set("*", ESP_LOG_NONE);
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
