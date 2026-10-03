// ===========================================================================
//  board.h  —  Profils de carte : reconnaissance du modèle au démarrage
// ---------------------------------------------------------------------------
//  UN SEUL binaire pour toutes les cartes. boardDetect() lit les eFuses (valeurs
//  gravées en usine dans la puce, indépendantes de la compilation) :
//    - EFUSE_FLASH_CAP != 0 : flash INTÉGRÉE à la puce (ESP32-S3FH4R2)
//                             -> profil « supermini » ;
//    - EFUSE_FLASH_CAP == 0 : flash externe, dans un module WROOM-1 (N8R2, N16R8…)
//                             -> profil « devkit » (carte type DevKitC, 2 USB-C).
//  Le profil fixe la broche de la LED et les GPIO exposés : sorties o1.., entrées
//  i1.. (+ BOOT, GPIO0, sur toutes les cartes). o1–o4 / i1–i4 sont sur les MÊMES
//  GPIO partout : une commande écrite pour une carte vaut pour l'autre.
//
//  Limite : on identifie la PUCE, pas le circuit imprimé. Deux cartes WROOM de
//  fabricants différents (LED sur GPIO38 au lieu de 48…) donnent le même profil.
//
//  Nouvelle carte : un profil dans BOARDS[] + sa règle dans boardDetect().
//  Sorties / entrées : au plus BOARD_MAX_OUT / BOARD_MAX_IN (tables de gpio_panel.h).
//  À éviter : 0/3/45/46 (démarrage), 19/20 (USB), 26–32 (flash/PSRAM),
//  33–37 (PSRAM octale des modules R8), 43/44 (UART0 / pont USB-série), 48 (LED).
// ===========================================================================
#pragma once
#include <Arduino.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_flash.h>

#define BOARD_MAX_OUT 8
#define BOARD_MAX_IN  8

struct board_t {
  const char*    id;       // identifiant stable (protocole : champ "b")
  const char*    name;     // nom affiché (champ "bn")
  uint8_t        ledPin;   // WS2812 intégrée
  const uint8_t* out;      // GPIO des sorties o1, o2…
  uint8_t        nOut;
  const uint8_t* in;       // GPIO des entrées i1, i2…
  uint8_t        nIn;
};

// SuperMini (ESP32-S3FH4R2) : peu de broches sorties sur ses connecteurs.
static const uint8_t SM_OUT[] = { 4, 5, 6, 7 };
static const uint8_t SM_IN[]  = { 8, 9, 10, 11 };
// DevKit WROOM-1 : o1–o8 = GPIO 4,5,6,7,15,16,17,18 se suivent sur le connecteur
// gauche ; i1–i7 = 8..14 (même connecteur, 3 et 46 sautés : démarrage), i8 = 21.
static const uint8_t DK_OUT[] = { 4, 5, 6, 7, 15, 16, 17, 18 };
static const uint8_t DK_IN[]  = { 8, 9, 10, 11, 12, 13, 14, 21 };

static const board_t BOARDS[] = {
  { "supermini", "S3 SuperMini", 48, SM_OUT, sizeof(SM_OUT), SM_IN, sizeof(SM_IN) },
  { "devkit",    "S3 DevKit",    48, DK_OUT, sizeof(DK_OUT), DK_IN, sizeof(DK_IN) },
};
static_assert(sizeof(SM_OUT) <= BOARD_MAX_OUT && sizeof(DK_OUT) <= BOARD_MAX_OUT, "trop de sorties");
static_assert(sizeof(SM_IN)  <= BOARD_MAX_IN  && sizeof(DK_IN)  <= BOARD_MAX_IN,  "trop d'entrées");

static const board_t* g_board = &BOARDS[0];   // SuperMini tant que boardDetect() n'a pas tourné
static char g_chip[12] = "";                  // ex. "F4R2" (flash intégrée 4 Mo + PSRAM 2 Mo), "N8R2"

// À appeler tôt dans setup() (avant ledBegin : la LED dépend du profil).
static void boardDetect() {
  uint8_t flashCap = 0;
  esp_efuse_read_field_blob(ESP_EFUSE_FLASH_CAP, &flashCap, esp_efuse_get_field_size(ESP_EFUSE_FLASH_CAP));
  g_board = flashCap ? &BOARDS[0] : &BOARDS[1];

  // Tailles RÉELLES (pas celles de l'en-tête du binaire, compilé FlashSize=4M) :
  // flash lue par son identifiant JEDEC, PSRAM telle que détectée au démarrage.
  uint32_t flash = 0;
  esp_flash_get_physical_size(esp_flash_default_chip, &flash);
  uint32_t psram = ESP.getPsramSize();
  unsigned fMB = (unsigned)((flash + (1u << 19)) >> 20);
  unsigned pMB = (unsigned)((psram + (1u << 19)) >> 20);
  if (pMB) snprintf(g_chip, sizeof(g_chip), "%c%uR%u", flashCap ? 'F' : 'N', fMB, pMB);
  else     snprintf(g_chip, sizeof(g_chip), "%c%u",    flashCap ? 'F' : 'N', fMB);
}
