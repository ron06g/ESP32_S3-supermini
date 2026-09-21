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
//  Rôle du CDC selon le flag `serial` (config NVS) :
//    - serial OFF : console de debug (logs `Console.printf`), pas d'I/O protocole ;
//    - serial ON  : PORT PROTOCOLE — comBegin() démarre la tâche `com` qui lit le
//                   JSON (1 ligne = 1 commande) et écrit les STATUS (1 par ligne),
//                   et les logs de debug sont tus (DBG dans le .ino) pour garder
//                   le flux propre. Vitesse nominale (USB CDC l'ignore : 9600 8N1
//                   côté hôte convient).
//
//  Invariant : USBCDC::write peut bloquer (tx_lock + timeout) si l'hôte a ouvert
//  le port sans le lire -> SEULE la tâche `com` écrit le protocole ; notifyStatus
//  ne fait que poster dans une file (comQueueStatus).
//
//  Fournis par hid_firmware.ino avant l'include : STATUS_MAX, CMD_MAX_BYTES,
//  enqueueCommand(), notifyStatus(), et l'objet console `USBCDC Console`.
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

static void comTask(void*) {
  static char line[CMD_MAX_BYTES + 1];
  size_t len = 0;
  bool overflow = false;
  char item[STATUS_MAX];
  for (;;) {
    // --- RX : accumule jusqu'au \n ---
    while (g_com->available()) {
      int c = g_com->read();
      if (c < 0) break;
      if (c == '\r') continue;
      if (c == '\n') {
        if (overflow) notifyStatus("err:toolong");
        else if (len) enqueueCommand((const uint8_t*)line, len);
        len = 0; overflow = false;
        continue;
      }
      if (len < CMD_MAX_BYTES) line[len++] = (char)c;
      else overflow = true;
    }
    // --- TX : draine la file de statuts (lignes protocole propres) ---
    while (xQueueReceive(g_comTxQueue, item, 0) == pdTRUE) {
      g_com->write((const uint8_t*)item, strlen(item));
      g_com->write((const uint8_t*)"\n", 1);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// À appeler depuis usbBegin() seulement si g_cfg.serial. Réutilise la console
// (interface 0) : aucun endpoint supplémentaire, donc HID clavier+souris préservé.
static void comBegin() {
  g_com = &Console;
  g_com->setRxBufferSize(1024);
  g_com->setTxTimeoutMs(20);               // ne jamais bloquer longtemps la tâche com
  g_comTxQueue = xQueueCreate(16, STATUS_MAX);
  xTaskCreatePinnedToCore(comTask, "com", 4096, nullptr, 3, nullptr, 0);
}
