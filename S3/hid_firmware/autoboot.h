// ===========================================================================
//  autoboot.h  —  Macro lancée automatiquement au démarrage
// ---------------------------------------------------------------------------
//  Une macro (commande JSON txt / seq / key / char) mémorisée en NVS est ENFILÉE
//  une fois par démarrage, `d` secondes après que la machine cible a reconnu le
//  clavier USB (montage TinyUSB). Elle passe par la file comme une commande reçue :
//  STOP l'interrompt, le worker reste seul à toucher l'USB. Rien sur un esclave
//  (il ne fait que des GPIO) ni sans clavier HID.
//
//  Protocole — TOUJOURS LOCAL (comme cfg : jamais routé vers un esclave) :
//    {"t":"autoboot","a":"get"}
//        -> {"id":0,"ev":"autoboot","on":1,"d":5,"name":"…","len":123}   (on:0 si aucune)
//    {"t":"autoboot","a":"put","i":0,"n":N,"c":"<morceau>"[,"d":5,"name":"…"]}
//        Envoi en N morceaux (une écriture BLE plafonne à ~500 o) : `c` = tranche
//        de la macro JSON sérialisée, dans l'ordre i = 0..N-1. `d` (délai, s) et
//        `name` sont lus sur le DERNIER morceau, qui valide puis enregistre.
//        Accusé par morceau : {"id":0,"ev":"autoboot","put":i} ; au dernier, la
//        frame d'état ci-dessus. Morceau hors séquence = tout est abandonné.
//    {"t":"autoboot","a":"clr"}   efface la macro (-> on:0)
//    {"t":"autoboot","a":"run"}   l'exécute tout de suite (essai)
//    Au déclenchement : {"id":0,"ev":"autoboot","run":"boot"|"run","name":"…"}.
//    Erreur : {"id":0,"err":"autoboot"}.
//
//  NVS (namespace s3kbd, effacée par le reset d'usine) : abm (macro JSON,
//  <= AB_MAX o), abd (délai en s), abn (nom affiché).
//  Fournis par hid_firmware.ino : statusRaw(), notifyStatus(), enqueueTrusted().
// ===========================================================================
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "USB.h"

#define AB_MAX       2048       // taille max de la macro (octets JSON)
#define AB_DELAY_DEF 5          // s
#define AB_DELAY_MAX 600        // s
#define AB_NAME_MAX  64         // octets, \0 compris

// Assemblage des morceaux : worker seul (handleAutoboot y est exécuté).
static char*  g_abBuf   = nullptr;
static size_t g_abLen   = 0;
static int    g_abNext  = 0;
static int    g_abParts = 0;

static void abReset() {
  free(g_abBuf);
  g_abBuf = nullptr; g_abLen = 0; g_abNext = 0; g_abParts = 0;
}

// Copie tronquée sans couper un caractère UTF-8 en deux.
static void abCopyName(char* dst, const char* src) {
  strlcpy(dst, src ? src : "", AB_NAME_MAX);
  size_t n = strlen(dst);
  if (n < AB_NAME_MAX - 1) return;                   // pas de troncature
  size_t k = n;
  while (k > 0 && ((uint8_t)dst[k - 1] & 0xC0) == 0x80) k--;   // octets de continuation
  if (k > 0) {
    uint8_t lead = (uint8_t)dst[k - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (k - 1) < need) dst[k - 1] = 0;          // dernier caractère incomplet
  }
}

// Seules des commandes de frappe sont admises (pas de cfg, pair, sec…).
static bool abValidMacro(JsonDocument& m) {
  const char* t = m["t"] | "";
  if (!strcmp(t, "txt"))  return m["v"].is<const char*>() && *(m["v"].as<const char*>());
  if (!strcmp(t, "seq"))  return m["s"].is<JsonArray>() || m["n"].is<const char*>();
  if (!strcmp(t, "key"))  return m["c"].is<const char*>();
  if (!strcmp(t, "char")) return m["v"].is<const char*>();
  return false;
}

static void abStatus() {
  JsonDocument d;
  d["id"] = 0; d["ev"] = "autoboot";
  Preferences p;
  bool on = p.begin(CFG_NS, true) && p.isKey("abm");
  d["on"] = on ? 1 : 0;
  if (on) {
    char nm[AB_NAME_MAX] = "";
    p.getString("abn", nm, sizeof(nm));
    d["d"]    = p.getUShort("abd", AB_DELAY_DEF);
    d["name"] = nm;
    d["len"]  = p.getString("abm", "").length();
  }
  p.end();
  char s[192];
  serializeJson(d, s, sizeof(s));
  statusRaw(s);
}

// Enfile la macro mémorisée. why = "boot" (démarrage) ou "run" (essai).
static bool abRunStored(const char* why) {
  Preferences p;
  if (!p.begin(CFG_NS, true)) return false;
  String js = p.getString("abm", "");
  char nm[AB_NAME_MAX] = "";
  p.getString("abn", nm, sizeof(nm));
  p.end();
  if (!js.length()) return false;
  JsonDocument ev;
  ev["id"] = 0; ev["ev"] = "autoboot"; ev["run"] = why; ev["name"] = nm;
  char s[160];
  serializeJson(ev, s, sizeof(s));
  statusRaw(s);
  ledPulse(C_CYAN, 160);
  return enqueueTrusted(js.c_str(), js.length());
}

// Exécuté dans le worker.
static void handleAutoboot(JsonDocument& doc) {
  const char* a = doc["a"] | "get";

  if (!strcmp(a, "put")) {
    int i = doc["i"] | -1, n = doc["n"] | 0;
    const char* c = doc["c"].is<const char*>() ? doc["c"].as<const char*>() : nullptr;
    if (!c || n < 1 || n > 64 || i < 0 || i >= n) { abReset(); notifyStatus("err:autoboot"); return; }
    if (i == 0) {                                     // 1er morceau : nouvel envoi
      abReset();
      g_abBuf = (char*)malloc(AB_MAX + 1);
      if (!g_abBuf) { notifyStatus("err:mem"); return; }
      g_abBuf[0] = 0;
      g_abParts = n;
    }
    size_t cl = strlen(c);
    if (!g_abBuf || i != g_abNext || n != g_abParts || g_abLen + cl > AB_MAX) {
      abReset(); notifyStatus("err:autoboot"); return;
    }
    memcpy(g_abBuf + g_abLen, c, cl);
    g_abLen += cl; g_abBuf[g_abLen] = 0; g_abNext++;
    if (i < n - 1) {
      char s[48];
      snprintf(s, sizeof(s), "{\"id\":0,\"ev\":\"autoboot\",\"put\":%d}", i);
      statusRaw(s);
      return;
    }
    // Dernier morceau : la macro doit être un JSON de frappe valide.
    long d = doc["d"].is<long>() ? doc["d"].as<long>() : AB_DELAY_DEF;
    JsonDocument m;
    bool ok = d >= 0 && d <= AB_DELAY_MAX && !deserializeJson(m, g_abBuf) && abValidMacro(m);
    abReset();
    if (!ok) { notifyStatus("err:autoboot"); return; }
    m.remove("id");                                   // jamais routée : s'exécute sur CE module
    String js;
    serializeJson(m, js);
    if (js.length() > AB_MAX) { notifyStatus("err:autoboot"); return; }
    char nm[AB_NAME_MAX];
    abCopyName(nm, doc["name"] | "");
    Preferences p;
    p.begin(CFG_NS, false);
    p.putString("abm", js);
    p.putUShort("abd", (uint16_t)d);
    p.putString("abn", nm);
    p.end();
    ledPulse(C_GREEN, 80);
    abStatus();
    return;
  }

  if (!strcmp(a, "clr")) {
    abReset();
    Preferences p;
    p.begin(CFG_NS, false);
    p.remove("abm"); p.remove("abd"); p.remove("abn");
    p.end();
    abStatus();
    return;
  }

  if (!strcmp(a, "run")) {
    if (!abRunStored("run")) notifyStatus("err:autoboot");
    return;
  }

  abStatus();                                         // get (défaut)
}

// Tâche à usage unique : attend le montage USB par la machine cible, puis le délai
// (recommencé si la cible se débranche entre-temps), enfile la macro et se termine.
static void autobootTask(void*) {
  uint16_t d = AB_DELAY_DEF;
  {
    Preferences p;
    if (p.begin(CFG_NS, true)) { d = p.getUShort("abd", AB_DELAY_DEF); p.end(); }
  }
  for (;;) {
    while (!USB) vTaskDelay(pdMS_TO_TICKS(100));      // clavier pas encore reconnu
    uint32_t waited = 0;
    bool lost = false;
    while (waited < (uint32_t)d * 1000UL) {
      vTaskDelay(pdMS_TO_TICKS(100));
      waited += 100;
      if (!USB) { lost = true; break; }
    }
    if (!lost) break;
  }
  abRunStored("boot");
  vTaskDelete(nullptr);
}

// allowed : rôle non esclave ET clavier HID présent (décidé par setup()).
static void autobootBegin(bool allowed) {
  if (!allowed) return;
  Preferences p;
  bool on = p.begin(CFG_NS, true) && p.isKey("abm");
  p.end();
  if (on) xTaskCreatePinnedToCore(autobootTask, "autoboot", 4096, nullptr, 2, nullptr, 0);
}
