// ===========================================================================
//  ble_link.h  —  Appairage BLE↔BLE entre deux modules (maître / esclave)
// ---------------------------------------------------------------------------
//  Le maître est CLIENT GATT de l'esclave et rejoue le protocole JSON existant
//  sur le service HID-Bridge de l'esclave (même CMD / STATUS qu'un téléphone).
//
//    scan   : linkScan()          -> STATUS scan:<mac>:<rssi>:<name> ... scan:done
//    bind   : linkBind(mac)       -> connexion one-shot, ordre {"t":"pair","a":"slave"},
//                                    attente de « pair:ok », sauvegarde, reboot
//    unbind : linkUnbind()        -> {"t":"pair","a":"reset"} à l'esclave, puis reboot
//    lien   : linkBegin()         -> tâche `link` (reconnexion, écriture, RSSI, relais)
//    envoi  : linkForward(json)   -> file vers la tâche link
//
//  Pile : NimBLE (coeur 3.x) derrière l'API Arduino BLEDevice ; BLEAddress(uint8_t[6])
//  attend l'ordre « humain » (aa:bb:…), l'inversion NimBLE est faite par le constructeur.
//
//  INVARIANT : la tâche `link` est l'UNIQUE propriétaire du client BLE
//  (connect / writeValue / getRssi). BLERemoteCharacteristic::writeValue
//  attend un événement GATTC libéré par la tâche BTC : l'appeler depuis un
//  callback BLE interbloquerait. Le callback notify ne fait que copier dans
//  la file. scan/bind/unbind s'exécutent dans le WORKER (jamais dans onWrite).
//
//  Fournis par hid_firmware.ino avant l'include : SERVICE_UUID/CMD_UUID/
//  STATUS_UUID, STATUS_MAX, g_cfg (config.h), notifyStatus(), rebootWithStatus().
// ===========================================================================
#pragma once
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ---------------------------------------------------------------------------
//  État partagé (lecture libre ; écrit par la tâche link)
// ---------------------------------------------------------------------------
static volatile bool g_linkUp   = false;
static volatile int  g_linkRssi = 0;

enum : uint8_t { LK_TX = 0, LK_RX = 1 };
struct link_item_t { uint8_t kind; bool rsp; char* buf; };
static QueueHandle_t g_linkQueue = nullptr;   // items link_item_t, buf malloc

static const uint32_t LINK_CONNECT_TIMEOUT_MS = 5000;
static const uint32_t LINK_RETRY_MS           = 3000;
static const uint32_t LINK_RSSI_PERIOD_MS     = 2000;

// ---------------------------------------------------------------------------
//  Envoi vers l'esclave (appelable de partout : ne fait qu'enfiler)
// ---------------------------------------------------------------------------
static void linkForward(const char* json, bool rsp) {
  if (!g_linkQueue || !json) return;
  size_t n = strlen(json);
  link_item_t it = { LK_TX, rsp, (char*)malloc(n + 1) };
  if (!it.buf) { notifyStatus("err:mem"); return; }
  memcpy(it.buf, json, n + 1);
  if (xQueueSend(g_linkQueue, &it, 0) != pdTRUE) { free(it.buf); notifyStatus("err:busy"); }
}

// Purge les envois en attente (STOP prioritaire, ou lien coupé).
static void linkPurgeTx() {
  if (!g_linkQueue) return;
  link_item_t it;
  size_t n = uxQueueMessagesWaiting(g_linkQueue);
  for (size_t i = 0; i < n; i++) {
    if (xQueueReceive(g_linkQueue, &it, 0) != pdTRUE) break;
    if (it.kind == LK_TX) free(it.buf);
    else xQueueSend(g_linkQueue, &it, 0);          // on garde les relais de statut
  }
}

// Callback notify (contexte tâche BTC) : copie -> file, AUCUN appel BLE.
static void linkNotifyCB(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (!g_linkQueue || !len) return;
  if (len > STATUS_MAX - 1) len = STATUS_MAX - 1;
  link_item_t it = { LK_RX, false, (char*)malloc(len + 1) };
  if (!it.buf) return;
  memcpy(it.buf, data, len);
  it.buf[len] = 0;
  if (xQueueSend(g_linkQueue, &it, 0) != pdTRUE) free(it.buf);
}

// ---------------------------------------------------------------------------
//  Tâche link (maître) : reconnexion, écriture, RSSI, relais des STATUS
// ---------------------------------------------------------------------------
static void linkTask(void*) {
  BLEClient* client = BLEDevice::createClient();
  BLERemoteCharacteristic* cmd = nullptr;
  uint32_t lastRssi = 0;
  link_item_t it;

  for (;;) {
    if (!client->isConnected()) {
      if (g_linkUp) {
        g_linkUp = false; cmd = nullptr;
        linkPurgeTx();
        ledSetRssi(0);
        notifyStatus("link:down");
        ledSetMode(LST_IDLE);
      }
      ledPulse(C_CYAN, 60);                          // tentative de connexion
      DBGLN("[LINK] connexion a l'esclave...");
      if (client->connect(BLEAddress(g_cfg.peer), BLE_ADDR_PUBLIC, LINK_CONNECT_TIMEOUT_MS)) {
        client->setMTU(517);
        BLERemoteService* svc = client->getService(SERVICE_UUID);
        BLERemoteCharacteristic* st = svc ? svc->getCharacteristic(STATUS_UUID) : nullptr;
        cmd = svc ? svc->getCharacteristic(CMD_UUID) : nullptr;
        if (!cmd || !st) {
          DBGLN("[LINK] service HID-Bridge absent sur le pair");
          client->disconnect();
          vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS));
          continue;
        }
        st->registerForNotify(linkNotifyCB);
        g_linkUp = true;
        lastRssi = millis() - LINK_RSSI_PERIOD_MS;   // 1re mesure RSSI immediate (LED)
        DBGLN("[LINK] lien etabli");
        ledSetMode(LST_CONNECTED);
        notifyStatus("link:up");
      } else {
        vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS));
        continue;
      }
    }

    if (xQueueReceive(g_linkQueue, &it, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (it.kind == LK_TX) {
        if (!cmd || !cmd->writeValue((uint8_t*)it.buf, strlen(it.buf), it.rsp)) notifyStatus("err:nolink");
      } else {
        notifyStatus(it.buf);                        // relais tel quel (ready/busy/pong/gpio/err…)
      }
      free(it.buf);
    }

    if (g_linkUp && millis() - lastRssi > LINK_RSSI_PERIOD_MS) {
      lastRssi = millis();
      g_linkRssi = client->getRssi();
      if (g_linkRssi < 0) ledSetRssi(g_linkRssi);    // intensite LED = force du signal
      char s[24]; snprintf(s, sizeof(s), "link:rssi:%d", (int)g_linkRssi);
      notifyStatus(s);
    }
  }
}

// À appeler depuis setup() si rôle == maître (après l'init BLE).
static void linkBegin() {
  g_linkQueue = xQueueCreate(16, sizeof(link_item_t));
  xTaskCreatePinnedToCore(linkTask, "link", 8192, nullptr, 4, nullptr, 0);
}

// ---------------------------------------------------------------------------
//  Scan (worker, bloquant ~4 s) : modules annonçant le service HID-Bridge
// ---------------------------------------------------------------------------
static void linkScan() {
  if (!g_cfg.pair || g_cfg.role == ROLE_SLAVE) { notifyStatus("err:pair"); return; }
  notifyStatus("busy");
  BLEScan* sc = BLEDevice::getScan();
  sc->setActiveScan(true);
  sc->setInterval(100);
  sc->setWindow(80);
  BLEScanResults* r = sc->start(4, false);
  int found = 0;
  if (r) {
    BLEUUID svcUuid(SERVICE_UUID);
    for (int i = 0; i < r->getCount(); i++) {
      BLEAdvertisedDevice d = r->getDevice(i);
      if (!d.isAdvertisingService(svcUuid)) continue;
      char s[STATUS_MAX];
      snprintf(s, sizeof(s), "scan:%s:%d:%s", d.getAddress().toString().c_str(), d.getRSSI(), d.getName().c_str());
      notifyStatus(s);
      found++;
      vTaskDelay(pdMS_TO_TICKS(3));
    }
  }
  sc->clearResults();
  DBG("[LINK] scan termine : %d module(s)\n", found);
  notifyStatus("scan:done");
  notifyStatus("ready");
}

// ---------------------------------------------------------------------------
//  Bind (worker, one-shot) : devenir maître de `mac`
// ---------------------------------------------------------------------------
static volatile bool g_bindAck = false;
static void bindNotifyCB(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len >= 7 && !memcmp(data, "pair:ok", 7)) g_bindAck = true;
}

static void linkBind(const char* mac) {
  uint8_t peer[6];
  if (!g_cfg.pair || g_cfg.role != ROLE_STD || !cfgParseMac(mac, peer)) { notifyStatus("err:pair"); return; }
  notifyStatus("busy");
  BLEClient* c = BLEDevice::createClient();
  bool ok = false;
  if (c->connect(BLEAddress(peer), BLE_ADDR_PUBLIC, LINK_CONNECT_TIMEOUT_MS)) {
    c->setMTU(517);
    BLERemoteService* svc = c->getService(SERVICE_UUID);
    BLERemoteCharacteristic* cmd = svc ? svc->getCharacteristic(CMD_UUID) : nullptr;
    BLERemoteCharacteristic* st  = svc ? svc->getCharacteristic(STATUS_UUID) : nullptr;
    if (cmd && st) {
      g_bindAck = false;
      st->registerForNotify(bindNotifyCB);
      char order[80];
      snprintf(order, sizeof(order), "{\"t\":\"pair\",\"a\":\"slave\",\"mac\":\"%s\"}",
               BLEDevice::getAddress().toString().c_str());
      if (cmd->writeValue((uint8_t*)order, strlen(order), true)) {
        for (int i = 0; i < 40 && !g_bindAck; i++) vTaskDelay(pdMS_TO_TICKS(50));   // <= 2 s
        ok = g_bindAck;
      }
    }
    if (c->isConnected()) c->disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  delete c;
  if (!ok) { DBGLN("[LINK] bind : echec"); notifyStatus("err:pair"); notifyStatus("ready"); return; }
  memcpy(g_cfg.peer, peer, 6);
  g_cfg.role = ROLE_MASTER;
  cfgSave();
  DBG("[LINK] bind OK -> maitre de %s, reboot\n", mac);
  rebootWithStatus("pair:ok");
}

// ---------------------------------------------------------------------------
//  Unbind (worker, maître) : libère l'esclave puis soi-même
// ---------------------------------------------------------------------------
static void linkUnbind() {
  if (g_cfg.role != ROLE_MASTER) { notifyStatus("err:pair"); return; }
  if (g_linkUp) {
    linkForward("{\"t\":\"pair\",\"a\":\"reset\"}", true);
    vTaskDelay(pdMS_TO_TICKS(600));               // laisse la tâche link écrire l'ordre
  }
  g_cfg.role = ROLE_STD;
  memset(g_cfg.peer, 0, 6);
  cfgSave();
  rebootWithStatus("pair:ok");
}
