// ===========================================================================
//  status_led.h  —  Indicateur d'etat sur LED RGB (WS2812, GPIO48 sur les deux profils)
// ---------------------------------------------------------------------------
//  Moteur d'animation dans une TACHE FreeRTOS dediee. Le reste du firmware ne
//  fait que declarer un ETAT (ledSetMode / ledSetError) ou demander une
//  IMPULSION breve superposee (ledPulse). La tache calcule la couleur a ~50 Hz.
//
//  Concu pour evoluer : couleurs melangees (cyan/violet/orange deja utilisees),
//  fondus (respiration), intensite reglable (LED_MAX), impulsions en file.
//
//  Semantique (etat de fond) :
//    LST_BOOT      blanc qui respire        (initialisation)
//    LST_IDLE      bleu clignotement lent   (en attente d'un client BLE)   <- repos
//    LST_CONNECTED bleu fixe                (client BLE connecte)
//    LST_BUSY      violet qui respire       (sequence / texte en cours)
//    LST_ERROR     rouge, N clignotements = code d'erreur, pause, repete
//    LST_SLAVE_WAIT   ambre qui respire     (esclave : en attente du maitre)
//    LST_SLAVE_LINKED vert doux fixe        (esclave : lie au maitre)
//
//  Force du signal (appairage maitre/esclave) : si un RSSI est connu
//  (ledSetRssi, < 0 dBm), l'intensite des fonds LST_CONNECTED (maitre lie) et
//  LST_SLAVE_LINKED (esclave lie) suit le signal : -40 dBm et mieux = pleine
//  intensite, -90 dBm et pire = plancher encore visible. RSSI = 0 -> inconnu,
//  pleine intensite (cas du mode normal avec telephone). Transition lissee.
//
//  Impulsions (superposees, breves) :
//    verte   frappe d'une touche  (ledPulse C_GREEN) — visible en repetition
//    cyan    touche media / deconnexion
//    orange  commande rejetee (err:*) — signal transitoire, ne fige pas la LED
//
//  Codes d'erreur (ledSetError) :
//    1  echec init BLE / service GATT
//    2  (reserve) echec USB / montage HID
//    3  (reserve) memoire
// ===========================================================================
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static uint8_t g_ledPin = 48;  // WS2812 : broche du profil de carte (board.h), fixee avant ledBegin()
#define LED_MAX 48        // intensite max par canal (0-255) — doux, non eblouissant
#define LED_TICK_MS 20    // periode de rafraichissement (~50 FPS)
#define LED_PULSE_GAP 45  // temps eteint entre 2 impulsions -> repetitions distinctes
#define LED_QUEUE_LEN 24  // profondeur de la file d'impulsions
#define LED_RSSI_HI  -40  // dBm : au-dessus -> intensite pleine
#define LED_RSSI_LO  -90  // dBm : en dessous -> intensite plancher
#define LED_RSSI_MIN  28  // plancher (0-255) : la LED reste visible en limite de portee

typedef enum { LST_BOOT, LST_IDLE, LST_CONNECTED, LST_BUSY, LST_ERROR,
               LST_SLAVE_WAIT, LST_SLAVE_LINKED } LedMode;

typedef struct { uint8_t r, g, b; } led_rgb_t;
typedef struct { uint8_t r, g, b; uint16_t on_ms; } led_pulse_t;

// --- Palette (deja a l'intensite LED_MAX ; melanges = extensibilite) ---
static const led_rgb_t C_OFF    = {0, 0, 0};
static const led_rgb_t C_BLUE   = {0, 0, LED_MAX};
static const led_rgb_t C_GREEN  = {0, LED_MAX, 0};
static const led_rgb_t C_RED    = {LED_MAX, 0, 0};
static const led_rgb_t C_CYAN   = {0, LED_MAX * 5 / 6, LED_MAX};
static const led_rgb_t C_VIOLET = {LED_MAX * 2 / 3, 0, LED_MAX};
static const led_rgb_t C_ORANGE = {LED_MAX, LED_MAX * 3 / 8, 0};
static const led_rgb_t C_WHITE  = {LED_MAX * 5 / 6, LED_MAX * 5 / 6, LED_MAX * 5 / 6};

// --- Etat partage (ecrit par le firmware, lu par la tache) ---
static volatile LedMode g_ledMode    = LST_BOOT;
static volatile uint8_t g_ledErrCode = 1;
static volatile int     g_ledRssi    = 0;     // dBm du lien maitre/esclave, 0 = inconnu
static uint8_t          g_ledSigLvl  = 255;   // facteur lisse (tache LED uniquement)
static QueueHandle_t    g_ledQueue   = nullptr;

// ---------------------------------------------------------------------------
//  Outils de rendu
// ---------------------------------------------------------------------------
static inline led_rgb_t ledScale(led_rgb_t c, uint8_t f) {   // f : 0..255
  return (led_rgb_t){ (uint8_t)(c.r * f / 255),
                      (uint8_t)(c.g * f / 255),
                      (uint8_t)(c.b * f / 255) };
}
// Onde triangulaire 0..255..0 (fondu) de periode donnee.
static inline uint8_t ledTri(uint32_t t, uint32_t period) {
  uint32_t p = t % period, half = period / 2;
  uint32_t v = (p < half) ? (p * 255 / half) : (255 - (p - half) * 255 / half);
  return (uint8_t)v;
}
// Facteur d'intensite cible (0..255) selon le RSSI. Le dBm est deja
// logarithmique ; le carre donne une variation percue plus reguliere.
static uint8_t ledRssiTarget() {
  int r = g_ledRssi;
  if (r >= 0) return 255;                                    // inconnu
  if (r >= LED_RSSI_HI) return 255;
  if (r <= LED_RSSI_LO) return LED_RSSI_MIN;
  uint32_t x = (uint32_t)(r - LED_RSSI_LO) * 255 / (LED_RSSI_HI - LED_RSSI_LO);  // 0..255
  return (uint8_t)(LED_RSSI_MIN + (255 - LED_RSSI_MIN) * x * x / (255 * 255));
}
// Lissage (~0,3 s) pour eviter les sauts entre deux mesures RSSI.
static void ledRssiStep() {
  int tgt = ledRssiTarget(), cur = g_ledSigLvl;
  int d = (tgt - cur) / 8;
  if (d == 0 && tgt != cur) d = (tgt > cur) ? 1 : -1;
  g_ledSigLvl = (uint8_t)(cur + d);
}

static inline void ledWrite(led_rgb_t c) { rgbLedWrite(g_ledPin, c.r, c.g, c.b); }

// Rendu du fond « erreur » : g_ledErrCode clignotements rouges, puis pause.
static led_rgb_t ledRenderError(uint32_t t) {
  uint8_t code = g_ledErrCode ? g_ledErrCode : 1;
  const uint32_t on = 180, off = 180, tail = 900;
  uint32_t cycle = code * (on + off) + tail;
  uint32_t p = t % cycle;
  for (uint8_t i = 0; i < code; i++) {
    uint32_t start = i * (on + off);
    if (p >= start && p < start + on) return C_RED;
  }
  return C_OFF;
}

static led_rgb_t ledRenderBase(uint32_t t) {
  switch (g_ledMode) {
    case LST_BOOT:      return ledScale(C_WHITE,  ledTri(t, 1200));
    case LST_IDLE:      return ledScale(C_BLUE,   ledTri(t, 1600));  // clignotement lent (fondu)
    case LST_CONNECTED: return ledScale(C_BLUE, g_ledSigLvl);        // bleu fixe (x signal si lie)
    case LST_BUSY:      return ledScale(C_VIOLET, ledTri(t, 700));   // activite
    case LST_ERROR:     return ledRenderError(t);
    case LST_SLAVE_WAIT:   return ledScale(C_ORANGE, ledTri(t, 1600));  // esclave sans maitre
    case LST_SLAVE_LINKED: return ledScale(C_GREEN, 110 * g_ledSigLvl / 255);  // esclave lie (x signal)
  }
  return C_OFF;
}

// ---------------------------------------------------------------------------
//  Tache d'animation
// ---------------------------------------------------------------------------
static void ledTask(void*) {
  // Auto-test au demarrage : prouve les 3 canaux (comme le sketch de test).
  ledWrite(ledScale(C_RED, 255));   vTaskDelay(pdMS_TO_TICKS(150));
  ledWrite(ledScale(C_GREEN, 255)); vTaskDelay(pdMS_TO_TICKS(150));
  ledWrite(ledScale(C_BLUE, 255));  vTaskDelay(pdMS_TO_TICKS(150));

  uint32_t t = 0;
  bool inPulse = false; led_rgb_t pulseCol = C_OFF; int pulseLeft = 0, gapLeft = 0;
  led_pulse_t pend;

  for (;;) {
    ledRssiStep();
    if (!inPulse && gapLeft <= 0 && xQueueReceive(g_ledQueue, &pend, 0) == pdTRUE) {
      inPulse = true; pulseCol = (led_rgb_t){pend.r, pend.g, pend.b}; pulseLeft = pend.on_ms;
    }

    led_rgb_t out;
    if (inPulse) {
      out = pulseCol;
      pulseLeft -= LED_TICK_MS;
      if (pulseLeft <= 0) { inPulse = false; gapLeft = LED_PULSE_GAP; }
    } else if (gapLeft > 0) {
      out = C_OFF; gapLeft -= LED_TICK_MS;      // court noir pour separer les repetitions
    } else {
      out = ledRenderBase(t);
    }

    ledWrite(out);
    t += LED_TICK_MS;
    vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
  }
}

// ---------------------------------------------------------------------------
//  API publique
// ---------------------------------------------------------------------------
static void ledBegin() {
  g_ledQueue = xQueueCreate(LED_QUEUE_LEN, sizeof(led_pulse_t));
  g_ledMode = LST_BOOT;
  xTaskCreatePinnedToCore(ledTask, "led", 4096, nullptr, 1, nullptr, 0);
}
static inline void ledSetMode(LedMode m) { g_ledMode = m; }
static inline void ledSetRssi(int dbm) { g_ledRssi = dbm; }   // 0 = inconnu (pleine intensite)
static inline void ledSetError(uint8_t code) { g_ledErrCode = code; g_ledMode = LST_ERROR; }
static inline void ledPulse(led_rgb_t c, uint16_t on_ms) {
  if (!g_ledQueue) return;
  led_pulse_t p = {c.r, c.g, c.b, on_ms};
  xQueueSend(g_ledQueue, &p, 0);   // non bloquant : si pleine, on ignore
}
