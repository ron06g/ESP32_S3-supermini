// ============================================================================
//  wifi_portal.h — Transport Wi-Fi (résolution A), MOCK / POC sans sécurité.
// ---------------------------------------------------------------------------
//  Ajoute, en cohabitation avec le BLE et l'USB HID déjà en place :
//    - un SoftAP WPA2 « S3-KBD » (IP fixe 192.168.4.1, sans uplink internet) ;
//    - un DNS wildcard + un portail captif (302 vers la page « Accepter ») ;
//    - un serveur HTTP servant l'app WEB/Keyboard/ embarquée (web_assets.h) ;
//    - un serveur WebSocket (port 81) qui REJOUE le protocole JSON existant.
//
//  Invariants respectés :
//    - Les transports ne font qu'ENFILER via enqueueCommand() ; seul le worker
//      touche l'USB HID.
//    - Tout le TX WebSocket (statut) se fait depuis la SEULE tâche réseau
//      (la lib Links2004 n'est pas thread-safe) : notifyStatus poste dans une
//      file drainée ici.
//
//  Fournis par hid_firmware.ino (déclarations avancées, avant l'include) :
//    #define STATUS_MAX 200   (taille max d'une frame STATUS)
//    static void enqueueCommand(const uint8_t* data, size_t len);
//    static void releaseAll();
//    static USBCDC Console;   (console debug)
//  Dépendance bibliothèque : « WebSockets » de Markus Sattler (Links2004).
// ============================================================================
#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include "web_assets.h"

// ---------------------------------------------------------------------------
//  Réglages SoftAP (mock : clé WPA2 >= 8 caractères, cf. cahier §6.3)
// ---------------------------------------------------------------------------
static const char*     AP_SSID   = "S3-KBD";
// Clé WPA2 : désormais dans la config NVS (g_cfg.apPsk, cf. config.h), modifiable
// via {"t":"sec","a":"wifi","psk":"…"}. Défaut « apikey00 » posé par cfgLoad.
// (>= 8 car. requis par WPA2 : une clé plus courte fait échouer softAP.)
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);
static const uint16_t  HTTP_PORT = 80;
static const uint16_t  WS_PORT   = 81;
static const byte      DNS_PORT  = 53;
static const char*     ACCEPT_URL = "http://192.168.4.1/Keyboard/accept";

// ---------------------------------------------------------------------------
//  Objets serveurs
// ---------------------------------------------------------------------------
static WebServer        httpServer(HTTP_PORT);
static DNSServer        dnsServer;
static WebSocketsServer wsServer(WS_PORT);

// ---------------------------------------------------------------------------
//  File de statut vers la tâche réseau (broadcast WS fait UNIQUEMENT par elle)
// ---------------------------------------------------------------------------
static QueueHandle_t g_statusQueue = nullptr;
static char          g_lastStatus[STATUS_MAX] = "{\"id\":0,\"st\":\"ready\"}";   // etat courant (JSON) pour un nouveau client

// Appelable depuis n'importe quelle tâche (worker, callbacks). Ne touche PAS le WS.
static void wifiQueueStatus(const char* s) {
  if (!g_statusQueue || !s) return;
  char item[STATUS_MAX];
  strncpy(item, s, sizeof(item) - 1);
  item[sizeof(item) - 1] = 0;
  xQueueSend(g_statusQueue, item, 0);          // silencieux si pleine (état transitoire)
}

// ---------------------------------------------------------------------------
//  HTTP : envoi d'un asset embarqué (PROGMEM, gzip éventuel)
// ---------------------------------------------------------------------------
static void wifiSendAsset(const WebAsset& a) {
  if (a.gzip) httpServer.sendHeader("Content-Encoding", "gzip");
  httpServer.sendHeader("Cache-Control", "no-cache");
  httpServer.send_P(200, a.mime, (PGM_P)a.data, a.len);
}

// Redirection captive : 302 vers la page « Accepter » locale (jamais la page de succès).
static void wifiCaptiveRedirect() {
  httpServer.sendHeader("Location", ACCEPT_URL, true);
  httpServer.send(302, "text/plain", "");
}

static const WebAsset* wifiFindAsset(const String& path) {
  for (size_t i = 0; i < WEB_ASSETS_COUNT; i++)
    if (path == WEB_ASSETS[i].path) return &WEB_ASSETS[i];
  return nullptr;
}

// Tout ce qui n'est pas une route explicite : sert l'asset si le chemin existe,
// sinon redirige vers la page « Accepter » (grâce au DNS wildcard, tout tombe ici).
static void wifiHandleReq() {
  const WebAsset* a = wifiFindAsset(httpServer.uri());
  if (a) wifiSendAsset(*a);
  else   wifiCaptiveRedirect();
}

// ---------------------------------------------------------------------------
//  WebSocket : 1 frame texte = 1 JSON de commande (même protocole qu'en BLE)
// ---------------------------------------------------------------------------
static void wifiOnWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsServer.sendTXT(num, g_lastStatus);       // état courant à la connexion (ex. "ready")
      DBG("[WS] client #%u connecte\n", num);
      break;
    case WStype_TEXT:
      enqueueCommand(payload, len);              // ne fait qu'enfiler (worker = USB)
      break;
    case WStype_DISCONNECTED:
      releaseAll();                              // libère toute touche maintenue (pas de purge)
      DBG("[WS] client #%u deconnecte\n", num);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
//  Tâche réseau (core 0) : pompe DNS/HTTP/WS + draine la file de statut
// ---------------------------------------------------------------------------
static void wifiNetTask(void* arg) {
  for (;;) {
    dnsServer.processNextRequest();
    httpServer.handleClient();
    wsServer.loop();

    char item[STATUS_MAX];
    while (xQueueReceive(g_statusQueue, item, 0) == pdTRUE) {
      // Seul l'état du MAÎTRE (frame {"id":0,"st":...}) est « courant » (rejoué à
      // un nouveau client) ; ev:gpio/pong/scan/cfg/link et états d'esclave sont
      // des événements ponctuels.
      if (!strncmp(item, "{\"id\":0,\"st\":", 13)) {
        strncpy(g_lastStatus, item, sizeof(g_lastStatus) - 1);
        g_lastStatus[sizeof(g_lastStatus) - 1] = 0;
      }
      wsServer.broadcastTXT(item);               // diffusion : SEULE la tâche réseau émet
    }
    vTaskDelay(1);
  }
}

// ---------------------------------------------------------------------------
//  Enregistrement des routes : sondes captives + filet « tout le reste »
// ---------------------------------------------------------------------------
static void wifiRegisterRoutes() {
  // Sondes de détection captive → 302 (surtout PAS la réponse de succès attendue).
  static const char* probes[] = {
    "/hotspot-detect.html", "/library/test/success.html",     // Apple (iOS / macOS)
    "/generate_204", "/gen_204",                              // Android
    "/connecttest.txt", "/ncsi.txt", "/redirect", "/fwlink/", // Windows
  };
  for (auto p : probes) httpServer.on(p, wifiCaptiveRedirect);

  // Tout le reste (assets de l'app + noms captifs rabattus par le DNS) passe ici.
  httpServer.onNotFound(wifiHandleReq);
}

// ---------------------------------------------------------------------------
//  Init du transport Wi-Fi — appelé depuis setup(), APRÈS l'init BLE
// ---------------------------------------------------------------------------
static void wifiPortalBegin() {
  g_statusQueue = xQueueCreate(16, STATUS_MAX);  // 16 statuts de STATUS_MAX octets

  // Le SSID suit le nom convivial du module (comme l'annonce BLE) : renommer le
  // maître (qui redémarre) renomme donc aussi son réseau Wi-Fi. Défaut « S3-KBD ».
  const char* apSsid = g_cfg.name[0] ? g_cfg.name : AP_SSID;
  WiFi.mode(WIFI_AP);                             // AP seul (moins de RAM que AP_STA)
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);      // IP/passerelle fixes AVANT softAP
  bool ok = WiFi.softAP(apSsid, g_cfg.apPsk, 1 /*canal*/, 0 /*visible*/, 1 /*max_conn*/);
  WiFi.setSleep(false);                           // pas de modem-sleep : captif + WS réactifs
  WiFi.setTxPower(WIFI_POWER_11dBm);              // conso/chaleur : AP courte portée (tél. en main), ~19->11 dBm
  DBG("[WiFi] SoftAP '%s' %s — IP %s\n",
                apSsid, ok ? "OK" : "ECHEC (cle < 8 car. ?)",
                WiFi.softAPIP().toString().c_str());

  dnsServer.start(DNS_PORT, "*", AP_IP);          // wildcard : tout nom -> 192.168.4.1

  wifiRegisterRoutes();
  httpServer.begin();

  wsServer.begin();
  wsServer.onEvent(wifiOnWsEvent);

  // Tâche réseau sur le core 0 (le worker HID reste seul sur le core 1, prio 5).
  xTaskCreatePinnedToCore(wifiNetTask, "net", 8192, nullptr, 4, nullptr, 0);
  DBGLN("[WiFi] portail captif + WebSocket prets (ws://192.168.4.1:81/)");
}
