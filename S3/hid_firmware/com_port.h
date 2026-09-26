// ===========================================================================
//  com_port.h  —  Port COM « standard » sur l'USB CDC (interface 0 partagée)
// ---------------------------------------------------------------------------
//  CONTRAINTE MATÉRIELLE (ESP32-S3, USB-OTG FS) : le contrôleur n'a que 6
//  endpoints (5 en IN). Clavier + souris (1 interface HID) + la console CDC en
//  consomment déjà la quasi-totalité ; un SECOND CDC fait dépasser le budget et
//  Windows refuse le périphérique composite (code 10 : plus aucun HID/COM). Cf.
//  cahier des charges §3.2 (« budget d'endpoints »). On n'ajoute donc PAS de 2e
//  CDC : le port COM RÉUTILISE l'unique CDC (interface 0), la console `Console`.
//
//  Rôle du CDC selon le flag `serial` (config NVS) — le DEBUG est SUPPRIMÉ :
//    - serial OFF : AUCUN CDC (le module n'expose que le HID clavier/souris) ;
//    - serial ON  : PORT PROTOCOLE — comBegin() crée le CDC + démarre la tâche `com` qui lit le
//                   JSON (1 ligne = 1 commande) et écrit les STATUS (1 par ligne),
//                   et les logs de debug sont tus (DBG dans le .ino) pour garder
//                   le flux propre. Vitesse nominale (USB CDC l'ignore : 9600 8N1
//                   côté hôte convient).
//
//  Tramage (RX) — deux formes, reconnues en permanence :
//    - LIGNE   : JSON terminé par CR, LF ou CRLF (terminaux, scripts simples) ;
//    - TRAME   : STX (0x02) + JSON + ETX (0x03), CR LF facultatifs après ETX
//                (automates). STX ouvre une trame NEUVE (tout reste partiel est
//                jeté : resynchronisation après un envoi interrompu) ; entre STX
//                et ETX, CR/LF sont de simples blancs JSON (JSON multi-ligne
//                accepté). JSON n'autorise aucun caractère de contrôle brut dans
//                ses chaînes : STX/ETX ne peuvent donc pas apparaître dans une
//                commande valide.
//  Réponses (TX) : terminées par CR LF. Elles adoptent le format de la DERNIÈRE
//  commande reçue sur ce port : ligne -> « JSON CR LF » ; trame -> « STX JSON ETX CR LF ».
//
//  Invariant : USBCDC::write peut bloquer (tx_lock + timeout) si l'hôte a ouvert
//  le port sans le lire -> SEULE la tâche `com` écrit le protocole ; notifyStatus
//  ne fait que poster dans une file (comQueueStatus).
//
//  Fournis par hid_firmware.ino avant l'include : STATUS_MAX, CMD_MAX_BYTES,
//  enqueueCommand(), notifyStatus(). (Le CDC est créé ici, plus de console debug.)
// ===========================================================================
#pragma once
#include "USB.h"
#include "USBCDC.h"

static USBCDC*       g_com        = nullptr;   // = &Console quand le port protocole est actif
static QueueHandle_t g_comTxQueue = nullptr;

// Appelable de n'importe quelle tâche. Silencieux si le port est absent/plein.
static void comQueueStatus(const char* s) {
  if (!g_comTxQueue || !s) return;
  char item[STATUS_MAX];
  strncpy(item, s, STATUS_MAX - 1);
  item[STATUS_MAX - 1] = 0;
  xQueueSend(g_comTxQueue, item, 0);
}

static const char COM_STX = 0x02, COM_ETX = 0x03;

static void comTask(void*) {
  static char line[CMD_MAX_BYTES + 1];
  size_t len = 0;
  bool overflow = false;
  bool inFrame  = false;        // entre STX et ETX
  bool framedTx = false;        // format des réponses = celui de la dernière commande reçue
  char item[STATUS_MAX];
  for (;;) {
    // --- RX : ligne (CR / LF / CRLF) ou trame STX…ETX ---
    while (g_com->available()) {
      int c = g_com->read();
      if (c < 0) break;
      if (c == COM_STX) { len = 0; overflow = false; inFrame = true; continue; }   // trame neuve
      bool eol = (c == COM_ETX) || (!inFrame && (c == '\r' || c == '\n'));
      if (eol) {
        if (overflow) notifyStatus("err:toolong");
        else if (len) {
          framedTx = inFrame && c == COM_ETX;
          enqueueCommand((const uint8_t*)line, len);
        }
        len = 0; overflow = false; inFrame = false;   // CR LF après ETX : lignes vides, ignorées
        continue;
      }
      if (len < CMD_MAX_BYTES) line[len++] = (char)c;   // en trame, CR/LF = blancs JSON
      else overflow = true;
    }
    // --- TX : draine la file de statuts (réponses protocole propres) ---
    while (xQueueReceive(g_comTxQueue, item, 0) == pdTRUE) {
      if (framedTx) g_com->write((const uint8_t*)&COM_STX, 1);
      g_com->write((const uint8_t*)item, strlen(item));
      if (framedTx) g_com->write((const uint8_t*)&COM_ETX, 1);
      g_com->write((const uint8_t*)"\r\n", 2);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// À appeler depuis usbBegin() seulement si g_cfg.serial. Crée l'UNIQUE CDC
// (interface 0) — pas de debug, donc il n'existe QUE dans ce mode. Un seul CDC :
// aucun endpoint supplémentaire, HID clavier+souris préservé.
static void comBegin() {
  g_com = new USBCDC(0);                    // enregistre l'interface AVANT USB.begin() (appelé juste après)
  g_com->begin(115200);
  g_com->setRxBufferSize(1024);
  g_com->setTxTimeoutMs(20);               // ne jamais bloquer longtemps la tâche com
  g_comTxQueue = xQueueCreate(16, STATUS_MAX);
  xTaskCreatePinnedToCore(comTask, "com", 4096, nullptr, 3, nullptr, 0);
}
