// ===========================================================================
//  gpio_panel.h  —  Panneau GPIO : sorties (niveau, clignotement, PWM) + entrées
// ---------------------------------------------------------------------------
//  Nommage = sérigraphie de la S3 SuperMini (numéro de GPIO), plus « BOOT »
//  (GPIO0, bouton intégré : testable sans câblage).
//
//  Protocole (le champ `id` désigne la carte ; le maître route, la carte visée exécute) :
//    {"t":"gpio","p":"4","a":"set|clr|tgl|read"}      niveau / lecture d'une broche
//    {"t":"gpio","a":"read"}                           lecture de toutes les broches
//    {"t":"gpio","a":"clr"}                            toutes les sorties à 0, effets arrêtés
//    {"t":"gpio","p":"4","a":"loop","t_set":200,"t_clr":800,"nb":3}
//        clignotement AUTONOME : nb cycles (haut t_set ms puis bas t_clr ms) ;
//        nb 0 ou absent = infini. Fin : sortie à 0 + {"…","done":true}.
//    {"t":"gpio","p":"5","a":"pwm","duty":40,"t_pwm":5000,"hz":1000}
//        PWM MATÉRIEL (LEDC) : duty 0..100 %, pendant t_pwm ms (0 ou absent =
//        infini), hz facultatif (défaut 1000). Fin : sortie à 0 + "done".
//    STATUS {"id":n,"ev":"gpio","p":"4","v":1[,"fx":"loop"|"fx":"pwm","duty":d][,"done":true]}
//
//  Règle : toute commande d'ÉCRITURE sur une sortie (set / clr / tgl / loop /
//  pwm) interrompt l'effet en cours sur CETTE sortie ; `read` n'interrompt rien.
//  Une commande invalide est refusée (err:gpio) SANS toucher l'effet en cours.
//  Les effets tournent sur la carte concernée : aucune trame radio par transition,
//  seulement un STATUS au départ et un à la fin (nb atteint / durée écoulée).
//
//  Convention : entrées en pull-up, actif bas -> « 1 » = actif (bouton pressé).
//  Sorties : « 1 » = niveau haut (PWM : 1 = rapport cyclique non nul).
//
//  Threads :
//    - tâche `gpio`   : LIT les entrées (anti-rebond 3 x 20 ms), n'écrit rien ;
//    - worker         : applique les commandes (gpioHandle), de façon synchrone :
//                       le STATUS part avant la commande suivante (barrière ping) ;
//    - tâche `gpiofx` : UNE seule tâche pour toutes les sorties. Elle dort jusqu'à
//                       la prochaine échéance (ou jusqu'à une nouvelle commande) :
//                       ni scrutation, ni coût CPU au repos, 2 réveils par cycle de
//                       clignotement. Le PWM est matériel : zéro réveil pendant qu'il
//                       tourne (un seul, à la fin, s'il est borné).
//    Les sorties et l'état des effets sont protégés par g_gpioMux (worker + gpiofx).
//  Fournis par hid_firmware.ino : notifyStatus(), statusRaw(), g_myId, ledPulse().
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

static volatile uint8_t g_gpioState[GPIO_COUNT];   // sorties : niveau logique ; entrées : valeur débouncée
static bool g_gpioReady = false;

// ---------------------------------------------------------------------------
//  Effets autonomes des sorties
// ---------------------------------------------------------------------------
enum : uint8_t { FX_NONE = 0, FX_LOOP = 1, FX_PWM = 2 };
static const uint32_t FX_T_MIN_MS = 10;            // phase mini d'un clignotement (au-delà de 50 Hz : pwm)
static const uint32_t FX_T_MAX_MS = 86400000UL;    // 24 h (phases, durée de PWM)
static const uint32_t FX_HZ_MIN   = 10;
static const uint32_t FX_HZ_MAX   = 40000;
static const uint32_t FX_HZ_DEF   = 1000;
// Horloge source du LEDC : le cœur 3.x prend le QUARTZ 40 MHz sur l'ESP32-S3
// (LEDC_USE_XTAL_CLK, esp32-hal-ledc.c) et non l'APB 80 MHz. Contrainte du
// diviseur : hz x 2^résolution <= 40 MHz -> résolution choisie par fxPwmBits().
static const uint32_t FX_LEDC_SRC_HZ = 40000000UL;

// Plus grande résolution (<= 14 bits, maximum du S3) compatible avec la fréquence :
// 14 bits jusqu'à 2 441 Hz, 13 jusqu'à 4 882 Hz … 9 bits à 40 kHz.
static uint8_t fxPwmBits(uint32_t hz) {
  uint8_t res = 14;
  while (res > 1 && ((uint64_t)hz << res) > FX_LEDC_SRC_HZ) res--;
  return res;
}

struct gpio_fx_t {
  volatile uint8_t mode;      // FX_* (lu sans verrou pour les rapports)
  bool       timed;           // une échéance `next` est programmée
  bool       pwmOn;           // broche attachée au LEDC
  uint8_t    duty;            // rapport cyclique arrondi (%), pour les STATUS
  TickType_t tSet, tClr;      // durées des phases haute / basse (ticks)
  uint32_t   left;            // cycles restants ; 0 = infini
  TickType_t next;            // prochaine échéance (ticks)
};
static gpio_fx_t         g_fx[GPIO_COUNT];
static SemaphoreHandle_t g_gpioMux = nullptr;
static TaskHandle_t      g_fxTask  = nullptr;

static TickType_t fxTicks(uint32_t ms) {           // ms -> ticks sans débordement (pdMS_TO_TICKS déborde > 71 min)
  uint64_t t = ((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000;
  return t ? (TickType_t)t : 1;
}

static int gpioFind(const char* label) {
  if (!label) return -1;
  for (size_t i = 0; i < GPIO_COUNT; i++) if (!strcmp(GPIO_TABLE[i].label, label)) return (int)i;
  return -1;
}

// STATUS d'une broche. `doneMode` != FX_NONE : fin naturelle de cet effet.
// À appeler HORS de g_gpioMux (statusRaw diffuse vers BLE / Wi-Fi / COM).
static void gpioReport(size_t i, uint8_t doneMode = FX_NONE) {
  char s[112];
  int n = snprintf(s, sizeof(s), "{\"id\":%u,\"ev\":\"gpio\",\"p\":\"%s\",\"v\":%u",
                   g_myId, GPIO_TABLE[i].label, g_gpioState[i]);
  uint8_t m = doneMode ? doneMode : g_fx[i].mode;
  if (m == FX_LOOP)     n += snprintf(s + n, sizeof(s) - n, ",\"fx\":\"loop\"");
  else if (m == FX_PWM) n += snprintf(s + n, sizeof(s) - n, ",\"fx\":\"pwm\",\"duty\":%u", g_fx[i].duty);
  snprintf(s + n, sizeof(s) - n, doneMode ? ",\"done\":true}" : "}");
  statusRaw(s);
}

// --- Primitives de sortie : appelées SOUS g_gpioMux ---
static void outWrite(size_t i, uint8_t v) {
  digitalWrite(GPIO_TABLE[i].pin, v ? HIGH : LOW);
  g_gpioState[i] = v;
}
static void fxCancel(size_t i) {                   // arrête l'effet de la sortie i (niveau laissé à l'appelant)
  if (g_fx[i].pwmOn) {
    ledcDetach(GPIO_TABLE[i].pin);
    pinMode(GPIO_TABLE[i].pin, OUTPUT);            // la broche redevient une sortie GPIO simple
    g_fx[i].pwmOn = false;
  }
  g_fx[i].mode  = FX_NONE;
  g_fx[i].timed = false;
}

// ---------------------------------------------------------------------------
//  Tâche des effets : dort jusqu'à l'échéance la plus proche, applique les
//  transitions dues, émet les STATUS de fin hors verrou.
// ---------------------------------------------------------------------------
static void gpioFxTask(void*) {
  for (;;) {
    TickType_t wait = portMAX_DELAY;
    xSemaphoreTake(g_gpioMux, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < GPIO_COUNT; i++) {
      if (!g_fx[i].timed) continue;
      int32_t d = (int32_t)(g_fx[i].next - now);
      TickType_t w = d > 0 ? (TickType_t)d : 0;
      if (w < wait) wait = w;
    }
    xSemaphoreGive(g_gpioMux);
    if (wait) ulTaskNotifyTake(pdTRUE, wait);      // réveil : échéance OU nouvelle commande (xTaskNotifyGive)

    uint8_t done[GPIO_COUNT] = {0};
    xSemaphoreTake(g_gpioMux, portMAX_DELAY);
    now = xTaskGetTickCount();
    for (size_t i = 0; i < GPIO_COUNT; i++) {
      gpio_fx_t& f = g_fx[i];
      if (!f.timed || (int32_t)(f.next - now) > 0) continue;
      if (f.mode == FX_LOOP) {
        if (g_gpioState[i]) {                                    // fin de phase haute
          outWrite(i, 0);
          if (f.left && --f.left == 0) { f.mode = FX_NONE; f.timed = false; done[i] = FX_LOOP; continue; }
          f.next += f.tClr;
        } else {                                                 // fin de phase basse
          outWrite(i, 1);
          f.next += f.tSet;
        }
        // Retard d'une phase entière (tâche longtemps préemptée) : on se recale
        // plutôt que d'enchaîner des transitions en rafale.
        if ((int32_t)(f.next - now) <= 0) f.next = now + (g_gpioState[i] ? f.tSet : f.tClr);
      } else if (f.mode == FX_PWM) {                             // durée du PWM écoulée
        fxCancel(i);
        outWrite(i, 0);
        done[i] = FX_PWM;
      } else {
        f.timed = false;
      }
    }
    xSemaphoreGive(g_gpioMux);
    for (size_t i = 0; i < GPIO_COUNT; i++) if (done[i]) gpioReport(i, done[i]);
  }
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
  g_gpioMux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(gpioTask, "gpio", 3072, nullptr, 2, nullptr, 0);
  // Priorité > worker (5) : les échéances sont tenues même pendant une frappe ;
  // le travail par réveil se compte en microsecondes.
  xTaskCreatePinnedToCore(gpioFxTask, "gpiofx", 4096, nullptr, 6, &g_fxTask, 1);
  g_gpioReady = true;                                // en DERNIER : le worker peut déjà recevoir des commandes
}

// Nombre JSON (entier ou décimal) dans [lo, hi].
static bool gpioNum(JsonVariant v, double lo, double hi, double& out) {
  if (!v.is<double>()) return false;
  out = v.as<double>();
  return out >= lo && out <= hi;
}

// Exécuté dans le worker.
static void gpioHandle(JsonDocument& doc) {
  if (!g_gpioReady) { notifyStatus("err:gpio"); return; }
  const char* a = doc["a"] | "read";
  const char* p = doc["p"] | "";

  if (!*p) {                                          // commandes globales
    if (!strcmp(a, "read")) {
      for (size_t i = 0; i < GPIO_COUNT; i++) { gpioReport(i); vTaskDelay(pdMS_TO_TICKS(3)); }
      return;
    }
    if (!strcmp(a, "clr")) {                          // tout éteindre : effets arrêtés, sorties à 0
      xSemaphoreTake(g_gpioMux, portMAX_DELAY);
      for (size_t i = 0; i < GPIO_COUNT; i++)
        if (GPIO_TABLE[i].dir == GP_OUT) { fxCancel(i); outWrite(i, 0); }
      xSemaphoreGive(g_gpioMux);
      ledPulse(C_GREEN, 40);
      for (size_t i = 0; i < GPIO_COUNT; i++)
        if (GPIO_TABLE[i].dir == GP_OUT) { gpioReport(i); vTaskDelay(pdMS_TO_TICKS(3)); }
      return;
    }
    notifyStatus("err:gpio"); return;
  }

  int i = gpioFind(p);
  if (i < 0) { notifyStatus("err:gpio"); return; }
  if (!strcmp(a, "read")) { gpioReport(i); return; }
  if (GPIO_TABLE[i].dir != GP_OUT) { notifyStatus("err:gpio"); return; }   // pas d'écriture sur une entrée

  // Paramètres validés AVANT de toucher la sortie : une commande invalide
  // n'interrompt pas l'effet en cours.
  enum { W_SET, W_CLR, W_TGL, W_LOOP, W_PWM } op;
  double tSet = 0, tClr = 0, nb = 0, duty = 0, tPwm = 0, hz = FX_HZ_DEF;
  if      (!strcmp(a, "set")) op = W_SET;
  else if (!strcmp(a, "clr")) op = W_CLR;
  else if (!strcmp(a, "tgl")) op = W_TGL;
  else if (!strcmp(a, "loop")) {
    if (!gpioNum(doc["t_set"], FX_T_MIN_MS, FX_T_MAX_MS, tSet) ||
        !gpioNum(doc["t_clr"], FX_T_MIN_MS, FX_T_MAX_MS, tClr) ||
        (!doc["nb"].isNull() && !gpioNum(doc["nb"], 0, 4294967295.0, nb))) { notifyStatus("err:gpio"); return; }
    op = W_LOOP;
  } else if (!strcmp(a, "pwm")) {
    if (!gpioNum(doc["duty"], 0, 100, duty) ||
        (!doc["t_pwm"].isNull() && !gpioNum(doc["t_pwm"], 0, FX_T_MAX_MS, tPwm)) ||
        (!doc["hz"].isNull()    && !gpioNum(doc["hz"], FX_HZ_MIN, FX_HZ_MAX, hz))) { notifyStatus("err:gpio"); return; }
    op = W_PWM;
  } else { notifyStatus("err:gpio"); return; }

  bool ok = true;
  const uint8_t pin = GPIO_TABLE[i].pin;
  xSemaphoreTake(g_gpioMux, portMAX_DELAY);
  gpio_fx_t& f = g_fx[i];
  const uint8_t cur = g_gpioState[i];
  fxCancel(i);                                        // toute écriture interrompt l'effet en cours
  const TickType_t now = xTaskGetTickCount();
  switch (op) {
    case W_SET: outWrite(i, 1);    break;
    case W_CLR: outWrite(i, 0);    break;
    case W_TGL: outWrite(i, !cur); break;
    case W_LOOP:
      f.tSet = fxTicks((uint32_t)tSet);
      f.tClr = fxTicks((uint32_t)tClr);
      f.left = (uint32_t)nb;                          // 0 = infini
      outWrite(i, 1);                                 // démarre par la phase haute
      f.next = now + f.tSet; f.timed = true; f.mode = FX_LOOP;
      break;
    case W_PWM:
      f.duty = (uint8_t)lround(duty);
      if (duty <= 0)        outWrite(i, 0);           // 0 % / 100 % : niveau fixe, LEDC inutile
      else if (duty >= 100) outWrite(i, 1);
      else {
        uint8_t res = fxPwmBits((uint32_t)hz);        // hz x 2^res <= 40 MHz (horloge LEDC)
        if (ledcAttach(pin, (uint32_t)hz, res)) {
          ledcWrite(pin, (uint32_t)lround(duty * ((1u << res) - 1) / 100.0));
          f.pwmOn = true;
          g_gpioState[i] = 1;
        } else {                                      // aucun timer LEDC disponible : sortie à 0
          pinMode(pin, OUTPUT);
          outWrite(i, 0);
          ok = false;
        }
      }
      if (ok) {
        f.mode = FX_PWM;
        if (tPwm > 0) { f.next = now + fxTicks((uint32_t)tPwm); f.timed = true; }
      }
      break;
  }
  const bool wake = f.timed;
  xSemaphoreGive(g_gpioMux);
  if (!ok) { notifyStatus("err:gpio"); return; }
  if (wake) xTaskNotifyGive(g_fxTask);                // la tâche recalcule sa prochaine échéance
  ledPulse(C_GREEN, 40);
  gpioReport(i);
}
