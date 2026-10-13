// ===========================================================================
//  status_led.h  —  LED RGB d'etat (WS2812 sur GPIO48 de la N8R2)
// ---------------------------------------------------------------------------
//  Meme principe que le firmware HID : une TACHE dediee calcule la couleur a
//  ~50 Hz ; le reste du firmware ne fait que declarer un ETAT (ledSet) ou
//  demander un bref eclair superpose (ledFlash). Il ne pilote jamais la LED.
//
//  Etats (100 % = LED_MAX, intensite volontairement douce) :
//    LED_BOOT         bleu, respiration lente       demarrage / RESTART (delai de grace)
//    LED_READER       vert, respiration lente       lecteur pret, pas de carte (aussi apres CARD_OUT)
//    LED_NO_READER    rouge fixe 50 %               aucun lecteur
//    LED_BAD_DEVICE   orange fixe 50 %              peripherique USB branche mais pas un lecteur CCID
//    LED_CARD         vert fixe 100 %               carte presente (lue avec succes)
//    LED_READING      vert 50 %, clignotement rapide  lecture en cours
//    LED_CARD_ERROR   rouge fixe 50 %               CARD_READ_ERROR
//    LED_CARD_NOT_CV  violet fixe 50 %              CARD_READ_NOT_CV (rouge + bleu)
//    LED_USB_FAULT    rouge 100 %, clignotement rapide  pile USB en panne (verrouille)
//  Eclair : bref blanc a chaque commande recue sur le port COM.
//  Auto-test au demarrage : rouge, vert, bleu (prouve les 3 canaux).
// ===========================================================================
#pragma once

#define LED_PIN      48     // WS2812 de la N8R2
#define LED_MAX      64     // intensite « 100 % » par canal (0-255) : douce, peu de chaleur
#define LED_TICK_MS  20     // rafraichissement ~50 Hz
#define LED_SLOW_MS  1600   // periode de la respiration lente
#define LED_FAST_MS  200    // periode du clignotement rapide (5 Hz)
#define LED_FLASH_MS 60     // duree de l'eclair « commande recue »

enum : uint8_t { LED_BOOT, LED_READER, LED_NO_READER, LED_BAD_DEVICE, LED_CARD,
                 LED_READING, LED_CARD_ERROR, LED_CARD_NOT_CV, LED_USB_FAULT };

struct LedRgb { uint8_t r, g, b; };

static volatile uint8_t  g_ledState    = LED_BOOT;
static volatile uint32_t g_ledFlashEnd = 0;       // millis() de fin de l'eclair en cours

static inline LedRgb ledRgb(uint8_t r, uint8_t g, uint8_t b) { return LedRgb{ r, g, b }; }

static inline LedRgb ledScale(LedRgb c, uint8_t f) {          // f : 0..255
  return LedRgb{ (uint8_t)(c.r * f / 255), (uint8_t)(c.g * f / 255), (uint8_t)(c.b * f / 255) };
}

// Respiration : onde triangulaire 0..255..0, corrigee (carre) pour un fondu
// percu regulier (l'oeil est logarithmique).
static inline uint8_t ledBreath(uint32_t t, uint32_t period) {
  uint32_t p = t % period, half = period / 2;
  uint32_t v = (p < half) ? (p * 255 / half) : (255 - (p - half) * 255 / half);
  return (uint8_t)(v * v / 255);
}

static inline bool ledBlink(uint32_t t, uint32_t period) { return (t % period) < period / 2; }

static LedRgb ledRender(uint32_t t) {
  const uint8_t M = LED_MAX, H = LED_MAX / 2;
  switch (g_ledState) {
    case LED_BOOT:        return ledScale(ledRgb(0, 0, M), ledBreath(t, LED_SLOW_MS));
    case LED_READER:      return ledScale(ledRgb(0, M, 0), ledBreath(t, LED_SLOW_MS));
    case LED_NO_READER:   return ledRgb(H, 0, 0);
    case LED_BAD_DEVICE:  return ledRgb(H, H * 3 / 8, 0);
    case LED_CARD:        return ledRgb(0, M, 0);
    case LED_READING:     return ledBlink(t, LED_FAST_MS) ? ledRgb(0, H, 0) : ledRgb(0, 0, 0);
    case LED_CARD_ERROR:  return ledRgb(H, 0, 0);
    case LED_CARD_NOT_CV: return ledRgb(H, 0, H);
    case LED_USB_FAULT:   return ledBlink(t, LED_FAST_MS) ? ledRgb(M, 0, 0) : ledRgb(0, 0, 0);
  }
  return ledRgb(0, 0, 0);
}

static inline void ledWrite(LedRgb c) { rgbLedWrite(LED_PIN, c.r, c.g, c.b); }

static void ledTask(void*) {
  const LedRgb test[3] = { ledRgb(LED_MAX, 0, 0), ledRgb(0, LED_MAX, 0), ledRgb(0, 0, LED_MAX) };
  for (const LedRgb& c : test) { ledWrite(c); vTaskDelay(pdMS_TO_TICKS(150)); }
  uint32_t t = 0;
  for (;;) {
    LedRgb c = ((int32_t)(g_ledFlashEnd - millis()) > 0) ? ledRgb(LED_MAX, LED_MAX, LED_MAX) : ledRender(t);
    ledWrite(c);
    t += LED_TICK_MS;
    vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
  }
}

static void ledBegin() {
  g_ledState = LED_BOOT;
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 0);
}

// Change l'etat de fond. LED_USB_FAULT est verrouille : seule une panne
// irreversible le declenche, rien ne doit le masquer.
static inline void ledSet(uint8_t s) {
  if (g_ledState != LED_USB_FAULT) g_ledState = s;
}

static inline void ledFlash() { g_ledFlashEnd = millis() + LED_FLASH_MS; }
