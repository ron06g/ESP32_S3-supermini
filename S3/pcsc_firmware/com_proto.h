// ===========================================================================
//  com_proto.h  —  Port COM « debug » (UART0 via le pont CH343, prise « COM »)
// ---------------------------------------------------------------------------
//  115200 8N1. Une trame = STX (0x02) + TEXTE + CR LF + EOT (0x04), dans les
//  deux sens. Le texte ne contient que des caracteres imprimables 0x20..0x7E :
//  tout autre octet est remplace par '?' a l'emission, pour ne jamais casser le
//  tramage.
//
//  Reception (tache `com`) :
//    - STX ouvre une trame NEUVE (un reste partiel est jete : resynchronisation) ;
//    - CR, LF, EOT ou ETX la ferment ; les trames vides sont ignorees (le LF et
//      l'EOT qui suivent le CR d'une trame complete) ;
//    - une ligne nue (sans STX) terminee par CR/LF est aussi acceptee : test
//      direct depuis un terminal ;
//    - espaces rognes, casse indifferente, COM_CMD_MAX caracteres au plus
//      (au-dela : UNKNOWN_CMD).
//  Les commandes sont postees dans g_cmdQueue, la file de la tache `ccid` (seul
//  proprietaire du lecteur) : les reponses s'intercalent dans l'ordre avec les
//  evenements. File pleine (hote trop bavard pendant une lecture) : la tache
//  `com` ATTEND (les octets s'accumulent dans le tampon RX), aucune commande
//  n'est perdue. Chaque commande recue fait un bref eclair blanc sur la LED.
//
//  Emission : comSend() ecrit une trame entiere en UN appel Serial0.write(),
//  sous mutex : appelable de n'importe quelle tache, les trames ne se melangent
//  jamais.
//
//  Fournis par pcsc_firmware.ino / status_led.h avant l'include : CMD_*,
//  g_cmdQueue, ccidWake(), ledFlash().
// ===========================================================================
#pragma once

#define COM_FRAME_MAX  1100     // texte d'une trame emise (le resultat CARD_READ tient dans RESULT_MAX)
#define COM_CMD_MAX    64       // texte d'une commande recue

static const uint8_t COM_STX = 0x02, COM_ETX = 0x03, COM_EOT = 0x04;
static SemaphoreHandle_t g_comTxMux = nullptr;

// Emet <STX>text<CR><LF><EOT>. Le texte est assaini (0x20..0x7E, sinon '?').
static void comSend(const char* text) {
  static uint8_t buf[COM_FRAME_MAX + 4];          // protege par g_comTxMux
  xSemaphoreTake(g_comTxMux, portMAX_DELAY);
  size_t n = 0;
  buf[n++] = COM_STX;
  for (const char* p = text; *p && n < COM_FRAME_MAX; ++p) {
    uint8_t c = (uint8_t)*p;
    buf[n++] = (c >= 0x20 && c <= 0x7E) ? c : '?';
  }
  buf[n++] = '\r';
  buf[n++] = '\n';
  buf[n++] = COM_EOT;
  Serial0.write(buf, n);
  xSemaphoreGive(g_comTxMux);
}

struct ComCmdName { const char* name; uint8_t id; };
static const ComCmdName COM_CMDS[] = {
  { "RESTART",    CMD_RESTART    },
  { "IS_READER",  CMD_IS_READER  },
  { "IS_CARD",    CMD_IS_CARD    },
  { "READ_CARD",  CMD_READ_CARD  },
  { "CARD_STATE", CMD_CARD_STATE },
  { "INFO",       CMD_INFO       },     // diagnostic (hors specification initiale)
  { "DEBUG",      CMD_DEBUG      },     // bascule le mode DEBUG (debug_log.h)
  { "DEBUG ON",   CMD_DEBUG_ON   },
  { "DEBUG OFF",  CMD_DEBUG_OFF  },
};

// Ligne recue (non terminee par 0) -> commande postee a la tache ccid.
static void comDispatch(char* line, size_t len, bool overflow) {
  uint8_t cmd = CMD_UNKNOWN;
  if (!overflow) {
    while (len && line[len - 1] == ' ') --len;     // espaces de fin
    size_t i = 0;
    while (i < len && line[i] == ' ') ++i;         // espaces de debut
    if (i == len) return;                          // ligne blanche : ignoree
    line[len] = 0;
    char* s = line + i;
    for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
    for (const ComCmdName& c : COM_CMDS) {
      if (strcmp(s, c.name) == 0) { cmd = c.id; break; }
    }
  }
  ledFlash();
  xQueueSend(g_cmdQueue, &cmd, portMAX_DELAY);
  ccidWake();
}

static void comTask(void*) {
  static char line[COM_CMD_MAX + 1];
  size_t len = 0;
  bool overflow = false;
  for (;;) {
    while (Serial0.available()) {
      int c = Serial0.read();
      if (c < 0) break;
      if (c == COM_STX) { len = 0; overflow = false; continue; }          // trame neuve
      if (c == '\r' || c == '\n' || c == COM_EOT || c == COM_ETX) {       // fin de trame / de ligne
        if (len || overflow) comDispatch(line, len, overflow);
        len = 0;
        overflow = false;
        continue;
      }
      if (len < COM_CMD_MAX) line[len++] = (char)c;
      else overflow = true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Apres Serial0.begin(). Cree le mutex d'emission et la tache de reception.
static void comBegin() {
  g_comTxMux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(comTask, "com", 3072, nullptr, 3, nullptr, 1);
}
