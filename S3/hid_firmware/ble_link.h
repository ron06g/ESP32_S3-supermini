// ===========================================================================
//  ble_link.h  —  Étoile BLE↔BLE : un maître, 1..MAX_SLAVES esclaves
// ---------------------------------------------------------------------------
//  Le maître (id 0) est CLIENT GATT de CHAQUE esclave et rejoue le protocole
//  JSON sur le service HID-Bridge de l'esclave (même CMD/STATUS qu'un téléphone).
//  Chaque esclave se signale avec SON id (selfId) dans ses réponses ; le maître
//  ne fait que RELAYER verbatim (aucune réécriture).
//
//    scan   : linkScan()            -> {"id":0,"ev":"scan",...} ... "done":true
//    bind   : linkBind(mac,name)    -> connexion one-shot ; bootstrap LESC
//                                      « Just Works » (chiffre, non authentifie,
//                                      le maitre garde sa passkey) ; ordre de
//                                      provisioning sur PROV (mac+id+passkey) ;
//                                      efface le bond bootstrap ; slaveAdd(), reboot
//    unbind : linkUnbind(id)        -> reset à l'esclave + unpair (id==0 = tous), reboot
//    lien   : linkBegin()           -> tâche `relay` + une tâche `linkN` par esclave
//                                      (secureConnection LESC MITM avant tout writeValue)
//    envoi  : linkForward(id,json)  -> file TX du slot id
//
//  Pile : NimBLE (coeur 3.x) derrière l'API Arduino BLEDevice ; BLEAddress(uint8_t[6])
//  attend l'ordre « humain » (aa:bb:…), l'inversion NimBLE est faite par le constructeur.
//
//  BUDGET : total des connexions BLE simultanées <= 3 (CONFIG_BT_NIMBLE_MAX_
//  CONNECTIONS). L'hôte pilote le maître par COM/Wi-Fi -> les 3 connexions vont
//  aux esclaves (MAX_SLAVES=3). Un téléphone BLE simultané réduirait d'autant.
//
//  INVARIANT : la tâche `linkN` est l'UNIQUE propriétaire de SON client BLE
//  (connect / writeValue / getRssi). BLERemoteCharacteristic::writeValue attend
//  un événement GATTC libéré par la tâche BTC : l'appeler depuis un callback BLE
//  interbloquerait. Les callbacks notify ne font que copier dans la file relais.
//  scan/bind/unbind s'exécutent dans le WORKER (jamais dans onWrite).
//
//  Fournis par hid_firmware.ino avant l'include : SERVICE_UUID/CMD_UUID/
//  STATUS_UUID, STATUS_MAX, g_cfg (config.h), notifyStatus(), statusRaw(),
//  rebootWithStatus(), ledSetMode()/ledPulse()/LST_*.
// ===========================================================================
#pragma once
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ---------------------------------------------------------------------------
//  Runtime par esclave (index = id-1 ; écrit par la tâche linkN du slot)
// ---------------------------------------------------------------------------
struct link_rt_t {
  volatile bool up;
  volatile int  rssi;
  QueueHandle_t tx;      // file d'émission (link_item_t) vers CET esclave
};
static link_rt_t g_link[MAX_SLAVES] = {};

// File d'émission d'un slot : le JSON à écrire sur la caractéristique CMD.
struct link_item_t { bool rsp; char* buf; };

// File de relais PARTAGÉE : les callbacks notify de tous les esclaves y copient
// les STATUS reçus (déjà tagués de l'id de l'esclave) ; la tâche `relay` les
// rediffuse verbatim. Découple RX du TX par slot -> pas de mapping char*->slot.
static QueueHandle_t g_relayQueue = nullptr;

static const uint32_t LINK_CONNECT_TIMEOUT_MS = 5000;
static const uint32_t LINK_RETRY_MS           = 3000;
static const uint32_t LINK_RSSI_PERIOD_MS     = 2000;

// ---------------------------------------------------------------------------
//  État de lien (lecture libre depuis n'importe quelle tâche)
// ---------------------------------------------------------------------------
static bool linkAnyUp() { for (int i = 0; i < MAX_SLAVES; i++) if (g_link[i].up) return true; return false; }
static bool linkUp(uint8_t id) { return (id >= 1 && id <= MAX_SLAVES) && g_link[id - 1].up; }

// ---------------------------------------------------------------------------
//  Événements de lien (JSON direct ; id = l'esclave concerné)
// ---------------------------------------------------------------------------
static void linkEmitUp(uint8_t id, bool up) {
  char s[56]; snprintf(s, sizeof(s), "{\"id\":%u,\"ev\":\"link\",\"up\":%s}", id, up ? "true" : "false");
  notifyStatus(s);
}
static void linkEmitRssi(uint8_t id, int rssi) {
  char s[56]; snprintf(s, sizeof(s), "{\"id\":%u,\"ev\":\"link\",\"rssi\":%d}", id, rssi);
  notifyStatus(s);
}

// ---------------------------------------------------------------------------
//  Émission vers un esclave (appelable de partout : ne fait qu'enfiler)
// ---------------------------------------------------------------------------
static void linkForward(uint8_t id, const char* json, bool rsp) {
  if (id < 1 || id > MAX_SLAVES) { notifyStatus("err:nolink"); return; }
  QueueHandle_t q = g_link[id - 1].tx;
  if (!q || !json) { notifyStatus("err:nolink"); return; }
  size_t n = strlen(json);
  link_item_t it = { rsp, (char*)malloc(n + 1) };
  if (!it.buf) { notifyStatus("err:mem"); return; }
  memcpy(it.buf, json, n + 1);
  if (xQueueSend(q, &it, 0) != pdTRUE) { free(it.buf); notifyStatus("err:busy"); }
}

// Purge la file TX d'un slot (idx = id-1).
static void linkPurgeTx(int idx) {
  QueueHandle_t q = g_link[idx].tx;
  if (!q) return;
  link_item_t it;
  while (xQueueReceive(q, &it, 0) == pdTRUE) free(it.buf);
}

// STOP : purge toutes les files TX, puis diffuse un stop à chaque esclave lié.
static void linkPurgeAllTx() { for (int i = 0; i < MAX_SLAVES; i++) linkPurgeTx(i); }
static void linkBroadcastStop() {
  for (int i = 0; i < MAX_SLAVES; i++)
    if (g_link[i].up) linkForward(i + 1, "{\"t\":\"stop\"}", true);   // un esclave ancien le reconnait aussi
}

// Callback notify (contexte tâche BTC) : copie -> file relais, AUCUN appel BLE.
static void linkNotifyCB(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (!g_relayQueue || !len) return;
  if (len > STATUS_MAX - 1) len = STATUS_MAX - 1;
  char* buf = (char*)malloc(len + 1);
  if (!buf) return;
  memcpy(buf, data, len);
  buf[len] = 0;
  if (xQueueSend(g_relayQueue, &buf, 0) != pdTRUE) free(buf);
}

// Tâche de relais : rediffuse verbatim les STATUS des esclaves (statusRaw).
static void relayTask(void*) {
  char* buf;
  for (;;) {
    if (xQueueReceive(g_relayQueue, &buf, portMAX_DELAY) == pdTRUE && buf) {
      statusRaw(buf);
      free(buf);
    }
  }
}

// ---------------------------------------------------------------------------
//  Tâche link d'UN esclave. arg = id (1..MAX_SLAVES).
// ---------------------------------------------------------------------------
static void linkTask(void* arg) {
  uint8_t id  = (uint8_t)(uintptr_t)arg;
  int     idx = id - 1;
  int     ci  = slaveIndexById(id);
  if (ci < 0) vTaskDelete(nullptr);
  uint8_t mac[6]; memcpy(mac, g_cfg.slaves[ci].mac, 6);

  BLEClient* client = BLEDevice::createClient();
  BLERemoteCharacteristic* cmd = nullptr;
  uint32_t lastRssi = 0;
  link_item_t it;

  for (;;) {
    if (!client->isConnected()) {
      if (g_link[idx].up) {
        g_link[idx].up = false; cmd = nullptr;
        linkPurgeTx(idx);
        linkEmitUp(id, false);
        ledSetMode(linkAnyUp() ? LST_CONNECTED : LST_IDLE);
      }
      ledPulse(C_CYAN, 60);                          // tentative de connexion
      DBG("[LINK%u] connexion...\n", id);
      if (client->connect(BLEAddress(mac), BLE_ADDR_PUBLIC, LINK_CONNECT_TIMEOUT_MS)) {
        client->setMTU(517);
        // Chiffrement LESC (Passkey-Entry MITM) AVANT tout writeValue : la CMD de
        // l'esclave exige WRITE_AUTHEN. secureConnection() bloque jusqu'a la fin du
        // pairing (bond reutilise aux reconnexions). Sans ca, la 1re ecriture
        // echouerait (« insufficient authentication ») et emettrait err:nolink.
        if (!client->secureConnection()) {
          DBG("[LINK%u] securisation echouee\n", id);
          client->disconnect();
          vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS));
          continue;
        }
        BLERemoteService* svc = client->getService(SERVICE_UUID);
        BLERemoteCharacteristic* st = svc ? svc->getCharacteristic(STATUS_UUID) : nullptr;
        cmd = svc ? svc->getCharacteristic(CMD_UUID) : nullptr;
        if (!cmd || !st) {
          DBG("[LINK%u] service HID-Bridge absent\n", id);
          client->disconnect();
          vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS));
          continue;
        }
        st->registerForNotify(linkNotifyCB);
        g_link[idx].up = true;
        lastRssi = millis() - LINK_RSSI_PERIOD_MS;   // 1re mesure RSSI immédiate
        DBG("[LINK%u] lien etabli\n", id);
        ledSetMode(LST_CONNECTED);
        linkEmitUp(id, true);
      } else {
        vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS + id * 250));   // jitter par slot : évite la tempête
        continue;
      }
    }

    if (xQueueReceive(g_link[idx].tx, &it, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (!cmd || !cmd->writeValue((uint8_t*)it.buf, strlen(it.buf), it.rsp)) notifyStatus("err:nolink");
      free(it.buf);
    }

    if (g_link[idx].up && millis() - lastRssi > LINK_RSSI_PERIOD_MS) {
      lastRssi = millis();
      g_link[idx].rssi = client->getRssi();
      linkEmitRssi(id, g_link[idx].rssi);
    }
  }
}

// À appeler depuis setup() si rôle == maître (après l'init BLE).
static void linkBegin() {
  g_relayQueue = xQueueCreate(24, sizeof(char*));
  xTaskCreatePinnedToCore(relayTask, "relay", 4096, nullptr, 4, nullptr, 0);
  for (int i = 0; i < MAX_SLAVES; i++) {
    uint8_t id = g_cfg.slaves[i].id;
    if (!id) continue;
    g_link[id - 1].tx = xQueueCreate(16, sizeof(link_item_t));
    char nm[8]; snprintf(nm, sizeof(nm), "link%u", id);
    xTaskCreatePinnedToCore(linkTask, nm, 6144, (void*)(uintptr_t)id, 4, nullptr, 0);
  }
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
      snprintf(s, sizeof(s), "{\"id\":0,\"ev\":\"scan\",\"mac\":\"%s\",\"rssi\":%d,\"name\":\"%s\"}",
               d.getAddress().toString().c_str(), d.getRSSI(), d.getName().c_str());
      notifyStatus(s);
      found++;
      vTaskDelay(pdMS_TO_TICKS(3));
    }
  }
  sc->clearResults();
  DBG("[LINK] scan termine : %d module(s)\n", found);
  notifyStatus("{\"id\":0,\"ev\":\"scan\",\"done\":true}");
  notifyStatus("ready");
}

// ---------------------------------------------------------------------------
//  Bind (worker, one-shot) : appairer `mac` comme nouvel esclave (id auto)
// ---------------------------------------------------------------------------
static volatile bool g_bindAck = false;
static void bindNotifyCB(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  char t[64]; size_t n = len < 63 ? len : 63;
  memcpy(t, data, n); t[n] = 0;
  if (strstr(t, "pair")) g_bindAck = true;    // ack {"...,"ev":"pair","ok":true} ou "pair:ok"
}

static void linkBind(const char* mac, const char* name) {
  uint8_t peer[6];
  if (!g_cfg.pair || (g_cfg.role != ROLE_STD && g_cfg.role != ROLE_MASTER) || !cfgParseMac(mac, peer)) {
    notifyStatus("err:pair"); return;
  }
  for (int i = 0; i < MAX_SLAVES; i++)                       // déjà appairé à cette MAC ?
    if (g_cfg.slaves[i].id && !memcmp(g_cfg.slaves[i].mac, peer, 6)) { notifyStatus("err:pair"); return; }
  uint8_t id = slaveFreeId();
  if (!id) { notifyStatus("err:full"); return; }            // table pleine (MAX_SLAVES)
  notifyStatus("busy");
  BLEClient* c = BLEDevice::createClient();
  bool ok = false;
  if (c->connect(BLEAddress(peer), BLE_ADDR_PUBLIC, LINK_CONNECT_TIMEOUT_MS)) {
    c->setMTU(517);
    // Bootstrap : les deux modules sont DisplayOnly -> LESC « Just Works »
    // (chiffre, non authentifie). Le maitre GARDE sa passkey (aucune saisie a
    // 000000). La vraie passkey est ensuite poussee sur ce lien DEJA chiffre,
    // via la caracteristique de provisioning PROV (WRITE_ENC, presente seulement
    // sur un module vierge). CMD (WRITE_AUTHEN) refuserait ce lien non authentifie.
    if (c->secureConnection()) {
      BLERemoteService* svc = c->getService(SERVICE_UUID);
      BLERemoteCharacteristic* prov = svc ? svc->getCharacteristic(PROV_UUID)   : nullptr;
      BLERemoteCharacteristic* st   = svc ? svc->getCharacteristic(STATUS_UUID) : nullptr;
      if (prov && st) {
        g_bindAck = false;
        st->registerForNotify(bindNotifyCB);
        char order[128];
        snprintf(order, sizeof(order),
                 "{\"t\":\"pair\",\"a\":\"slave\",\"mac\":\"%s\",\"id\":%u,\"pk\":%lu}",
                 BLEDevice::getAddress().toString().c_str(), id, (unsigned long)g_cfg.passkey);
        if (prov->writeValue((uint8_t*)order, strlen(order), true)) {
          for (int i = 0; i < 40 && !g_bindAck; i++) vTaskDelay(pdMS_TO_TICKS(50));   // <= 2 s
          ok = g_bindAck;
        }
      } else {
        DBGLN("[LINK] bind : PROV absent (cible deja appairee ou appairage off ?)");
      }
    } else {
      DBGLN("[LINK] bind : securisation (bootstrap) echouee");
    }
    if (c->isConnected()) c->disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  delete c;
  if (!ok) { DBGLN("[LINK] bind : echec"); notifyStatus("err:pair"); notifyStatus("ready"); return; }
  secUnpairMac(peer);                    // efface le bond bootstrap (Just Works) cote maitre : reconnexion en MITM neuf
  slaveAdd(id, peer);
  slaveSetName(id, name);                // nom capte au scan (affichage + repere au re-appairage)
  g_cfg.role = ROLE_MASTER;
  cfgSave();
  DBG("[LINK] bind OK -> esclave id=%u (%s), reboot\n", id, mac);
  rebootWithStatus("pair:ok");
}

// ---------------------------------------------------------------------------
//  Unbind (worker, maître) : libère un esclave (id) ou TOUS (id==0), puis reboot
// ---------------------------------------------------------------------------
static void linkUnbind(uint8_t id) {
  if (g_cfg.role != ROLE_MASTER) { notifyStatus("err:pair"); return; }

  if (id == 0) {                                   // désappairage de toute l'étoile
    for (int i = 0; i < MAX_SLAVES; i++) {
      uint8_t sid = g_cfg.slaves[i].id;
      if (sid && g_link[sid - 1].up) linkForward(sid, "{\"t\":\"pair\",\"a\":\"reset\"}", true);
    }
    vTaskDelay(pdMS_TO_TICKS(600));
    memset(g_cfg.slaves, 0, sizeof(g_cfg.slaves));
    g_cfg.role = ROLE_STD;
    cfgSave();
    secClearBonds();                               // efface tous les bonds d'esclaves cote maitre
    DBGLN("[LINK] unbind ALL -> standard, reboot");
    rebootWithStatus("pair:ok");
    return;
  }

  int idx = slaveIndexById(id);
  if (idx < 0) { notifyStatus("err:pair"); return; }
  uint8_t smac[6]; memcpy(smac, g_cfg.slaves[idx].mac, 6);   // MAC avant retrait (pour l'unpair)
  if (g_link[id - 1].up) {
    linkForward(id, "{\"t\":\"pair\",\"a\":\"reset\"}", true);
    vTaskDelay(pdMS_TO_TICKS(600));
  }
  slaveRemove(id);
  secUnpairMac(smac);                              // efface le bond de cet esclave cote maitre
  if (slaveCount() == 0) g_cfg.role = ROLE_STD;    // plus d'esclave -> retour standard
  cfgSave();
  DBG("[LINK] unbind id=%u, reboot\n", id);
  rebootWithStatus("pair:ok");
}
