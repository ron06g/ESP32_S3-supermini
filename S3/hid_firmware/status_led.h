// ===========================================================================
//  status_led.h  —  Indicateur d'etat sur LED RGB (WS2812, GPIO48)
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

#define LED_PIN 48        // WS2812 sur la SuperMini (cf. cahier des charges S3 §2)
#define LED_MAX 48        // intensite max par canal (0-255) — doux, non eblouissant
#define LED_TICK_MS 20    // periode de rafraichissement (~50 FPS)
#define LED_PULSE_GAP 45  // temps eteint entre 2 impulsions -> repetitions distinctes
#define LED_QUEUE_LEN 24  // profondeur de la file d'impulsions

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
static inline void ledWrite(led_rgb_t c) { rgbLedWrite(LED_PIN, c.r, c.g, c.b); }

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
    case LST_CONNECTED: return C_BLUE;                               // bleu fixe
    case LST_BUSY:      return ledScale(C_VIOLET, ledTri(t, 700));   // activite
    case LST_ERROR:     return ledRenderError(t);
    case LST_SLAVE_WAIT:   return ledScale(C_ORANGE, ledTri(t, 1600));  // esclave sans maitre
    case LST_SLAVE_LINKED: return ledScale(C_GREEN, 110);             // esclave lie
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
static inline void ledSetError(uint8_t code) { g_ledErrCode = code; g_ledMode = LST_ERROR; }
static inline void ledPulse(led_rgb_t c, uint16_t on_ms) {
  if (!g_ledQueue) return;
  led_pulse_t p = {c.r, c.g, c.b, on_ms};
  xQueueSend(g_ledQueue, &p, 0);   // non bloquant : si pleine, on ignore
}
