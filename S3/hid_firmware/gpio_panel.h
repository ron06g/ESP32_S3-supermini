// ===========================================================================
//  gpio_panel.h  —  Panneau GPIO (faisabilité) : quelques sorties + entrées
// ---------------------------------------------------------------------------
//  Nommage = sérigraphie de la S3 SuperMini (numéro de GPIO), plus « BOOT »
//  (GPIO0, bouton intégré : testable sans câblage).
//
//  Protocole :
//    {"t":"gpio","p":"4","a":"set|clr|tgl|read"}   sortie / lecture d'une broche
//    {"t":"gpio","a":"read"}                        lecture de toutes les broches
//    STATUS  gpio:<label>:<0|1>   (réponse, et spontané sur changement d'entrée)
//
//  Convention : entrées en pull-up, actif bas -> « 1 » = actif (bouton pressé).
//  Sorties : « 1 » = niveau haut.
//
//  Threads : les SORTIES ne sont écrites que par le worker (gpioHandle) ; la
//  tâche `gpio` ne fait que LIRE les entrées (anti-rebond 3 x 20 ms).
//  Fournis par hid_firmware.ino : notifyStatus(), ledPulse().
// ===========================================================================
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

enum : uint8_t { GP_IN = 0, GP_OUT = 1 };
struct gpio_def_t { const char* label; uint8_t pin; uint8_t dir; };

// >>> Table unique — adapter ici si la sérigraphie diffère. <<<
static const gpio_def_t GPIO_TABLE[] = {
  { "BOOT", 0,  GP_IN  },
  { "4",    4,  GP_OUT }, { "5",  5,  GP_OUT }, { "6",  6,  GP_OUT }, { "7",  7,  GP_OUT },
  { "8",    8,  GP_IN  }, { "9",  9,  GP_IN  }, { "10", 10, GP_IN  }, { "11", 11, GP_IN  },
};
static const size_t GPIO_COUNT = sizeof(GPIO_TABLE) / sizeof(GPIO_TABLE[0]);

static uint8_t g_gpioState[GPIO_COUNT];   // sorties : ombre écrite par le worker ; entrées : valeur débouncée
static bool    g_gpioReady = false;

static int gpioFind(const char* label) {
  if (!label) return -1;
  for (size_t i = 0; i < GPIO_COUNT; i++) if (!strcmp(GPIO_TABLE[i].label, label)) return (int)i;
  return -1;
}

static void gpioReport(size_t i) {
  char s[32];
  snprintf(s, sizeof(s), "gpio:%s:%u", GPIO_TABLE[i].label, g_gpioState[i]);
  notifyStatus(s);
}

// Tâche de scrutation des entrées (core 0) : 3 échantillons identiques avant changement.
static void gpioTask(void*) {
  uint8_t stable[GPIO_COUNT] = {0};
  for (;;) {
    for (size_t i = 0; i < GPIO_COUNT; i++) {
      if (GPIO_TABLE[i].dir != GP_IN) continue;
      uint8_t v = digitalRead(GPIO_TABLE[i].pin) ? 0 : 1;     // actif bas
      if (v == g_gpioState[i]) { stable[i] = 0; continue; }
      if (++stable[i] >= 3) { g_gpioState[i] = v; stable[i] = 0; gpioReport(i); }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void gpioBegin() {
  for (size_t i = 0; i < GPIO_COUNT; i++) {
    if (GPIO_TABLE[i].dir == GP_OUT) {
      pinMode(GPIO_TABLE[i].pin, OUTPUT); digitalWrite(GPIO_TABLE[i].pin, LOW); g_gpioState[i] = 0;
    } else {
      pinMode(GPIO_TABLE[i].pin, INPUT_PULLUP); g_gpioState[i] = digitalRead(GPIO_TABLE[i].pin) ? 0 : 1;
    }
  }
  g_gpioReady = true;
  xTaskCreatePinnedToCore(gpioTask, "gpio", 3072, nullptr, 2, nullptr, 0);
}

// Exécuté dans le worker.
static void gpioHandle(JsonDocument& doc) {
  if (!g_gpioReady) { notifyStatus("err:gpio"); return; }
  const char* a = doc["a"] | "read";
  const char* p = doc["p"] | "";
  if (!*p) {                                        // lecture globale
    if (strcmp(a, "read")) { notifyStatus("err:gpio"); return; }
    for (size_t i = 0; i < GPIO_COUNT; i++) { gpioReport(i); vTaskDelay(pdMS_TO_TICKS(3)); }
    return;
  }
  int i = gpioFind(p);
  if (i < 0) { notifyStatus("err:gpio"); return; }
  if (!strcmp(a, "read")) { gpioReport(i); return; }
  if (GPIO_TABLE[i].dir != GP_OUT) { notifyStatus("err:gpio"); return; }   // pas d'écriture sur une entrée
  uint8_t v = g_gpioState[i];
  if      (!strcmp(a, "set")) v = 1;
  else if (!strcmp(a, "clr")) v = 0;
  else if (!strcmp(a, "tgl")) v = !v;
  else { notifyStatus("err:gpio"); return; }
  digitalWrite(GPIO_TABLE[i].pin, v ? HIGH : LOW);
  g_gpioState[i] = v;
  ledPulse(C_GREEN, 40);
  gpioReport(i);
}
