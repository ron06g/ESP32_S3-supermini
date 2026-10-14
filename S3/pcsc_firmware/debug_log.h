// ===========================================================================
//  debug_log.h  —  MODE DEBUG (commande « DEBUG ») : journal sur le port COM
// ---------------------------------------------------------------------------
//  Desactive par defaut (non memorise : un redemarrage le coupe). Bascule par
//  la commande DEBUG (ou DEBUG ON / DEBUG OFF), reponse DEBUG_ON / DEBUG_OFF.
//
//  Chaque ligne de journal est une trame normale du protocole, prefixee « DBG: »
//  et horodatee (secondes depuis le demarrage) :
//    <STX>DBG:12.345 ...<CR><LF><EOT>
//  -> le tramage reste valide, un hote peut simplement ignorer les DBG:.
//
//  Contenu : etat electrique du port USB (registre HPRT : appareil connecte,
//  niveau de D+/D-, vitesse), routage du PHY, enumeration (descripteur de
//  peripherique vu par le filtre d'enumeration, avant meme NEW_DEV),
//  evenements USB, descripteurs de configuration et CCID decodes, echanges
//  CCID et APDU en hexa, etapes de lecture de la carte, et les logs d'ERREUR
//  de l'ESP-IDF (pile USB Host) captures par esp_log_set_vprintf() et
//  re-tramees « DBG:IDF: ». (Les libs precompilees du coeur ne contiennent que
//  le niveau ERREUR : INFO/DEBUG de l'IDF n'existent pas dans ce build.)
//
//  ATTENTION : en mode DEBUG, les echanges APDU (donc nom, NIR...) passent en
//  clair sur le port COM.
//
//  Fournis avant l'include : comSend() (com_proto.h).
// ===========================================================================
#pragma once
#include <stdarg.h>
#include "esp_log.h"

#define DBG_LINE_MAX  320       // texte d'une ligne de journal
#define DBG_HEX_MAX   300       // octets affiches au plus par vidage hexa

static volatile bool g_debug    = false;
static volatile bool g_dbgQuiet = false;   // tait le detail des echanges de scrutation (250 ms)

// Journal formate (printf). Sans effet hors mode DEBUG.
static void dbg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dbg(const char* fmt, ...) {
  if (!g_debug) return;
  char line[DBG_LINE_MAX];
  uint32_t t = millis();
  int k = snprintf(line, sizeof(line), "DBG:%lu.%03lu ", (unsigned long)(t / 1000), (unsigned long)(t % 1000));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + k, sizeof(line) - k, fmt, ap);
  va_end(ap);
  comSend(line);
}

// Vidage hexa : « DBG:<tag> [n] 0102AB... » (tronque a DBG_HEX_MAX octets).
static void dbgHex(const char* tag, const uint8_t* d, size_t n) {
  if (!g_debug) return;
  static char line[64 + 2 * DBG_HEX_MAX + 16];     // tache ccid seulement
  uint32_t t = millis();
  int k = snprintf(line, sizeof(line), "DBG:%lu.%03lu %s [%u] ", (unsigned long)(t / 1000),
                   (unsigned long)(t % 1000), tag, (unsigned)n);
  size_t shown = n > DBG_HEX_MAX ? DBG_HEX_MAX : n;
  for (size_t i = 0; i < shown && k + 3 < (int)sizeof(line); ++i)
    k += snprintf(line + k, sizeof(line) - k, "%02X", d[i]);
  if (shown < n) snprintf(line + k, sizeof(line) - k, "..");
  comSend(line);
}

// Logs de l'ESP-IDF -> trames « DBG:IDF:... » (codes couleur ANSI et fin de
// ligne retires). Reentrant (tampon sur la pile) : appele par n'importe quelle
// tache de l'IDF (usbLib, usbCli...).
static int dbgIdfVprintf(const char* fmt, va_list ap) {
  if (!g_debug) return 0;
  char raw[200], line[220];
  int n = vsnprintf(raw, sizeof(raw), fmt, ap);
  size_t k = 0;
  memcpy(line, "DBG:IDF:", 8);
  k = 8;
  for (const char* p = raw; *p && k + 1 < sizeof(line); ++p) {
    if (*p == 0x1B) {                                  // sequence ESC [ ... m
      while (*p && *p != 'm') ++p;
      if (!*p) break;
      continue;
    }
    if (*p == '\r' || *p == '\n') continue;
    line[k++] = *p;
  }
  line[k] = 0;
  if (k > 8) comSend(line);
  return n;
}

// Active / coupe le mode DEBUG (niveau des logs IDF suivi).
static void dbgSet(bool on) {
  g_debug = on;
  esp_log_level_set("*", on ? ESP_LOG_VERBOSE : ESP_LOG_NONE);
}

// A appeler une fois au demarrage (le crochet ne fait rien hors mode DEBUG).
static void dbgBegin() {
  esp_log_set_vprintf(dbgIdfVprintf);
  esp_log_level_set("*", ESP_LOG_NONE);
}
