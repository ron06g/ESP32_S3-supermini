// ===========================================================================
//  usb_ccid.h  —  USB Host (ESP-IDF) + pilote CCID minimal (lecteur « PC/SC »)
// ---------------------------------------------------------------------------
//  Espressif ne fournit pas de pilote de classe CCID : ce fichier en implemente
//  le strict necessaire au-dessus de la bibliotheque USB Host de l'IDF 5.5
//  (`usb/usb_host.h`, compilee dans le coeur Arduino 3.3.x).
//
//  Taches :
//    - usbLib : usb_host_lib_handle_events() (enumeration) ;
//    - usbCli : usb_host_client_handle_events(). Le callback client ne fait que
//      poster NEW_DEV / DEV_GONE dans g_usbEvtQueue (file reservee a l'USB : un
//      flot de commandes ne peut pas l'evincer) ; les callbacks de transfert
//      (executes dans ce meme contexte) ne font que liberer un semaphore. Aucun
//      appel bloquant depuis un callback.
//  Tout le reste (ouverture, claim, echanges, fermeture) s'execute dans la tache
//  `ccid`, seul proprietaire du lecteur.
//
//  Echanges CCID (bulk OUT puis bulk IN, en-tete de 10 octets) : synchrones,
//  delai gere ici (le champ timeout_ms des transferts n'est PAS pris en charge
//  par l'IDF). Un transfert n'est jamais laisse en vol au retour : en cas de
//  delai depasse il est annule (halt -> flush -> attente du callback -> clear).
//  S'il ne revient jamais, g_usbStuck est leve (RESTART redemarre alors l'ESP32).
//
//  Reception : IN d'exactement UN paquet (MPS) d'abord, puis le reste arrondi au
//  MPS si dwLength l'exige -> pas de blocage sur un ZLP que le lecteur n'enverrait
//  pas. bSeq verifie (reponse perimee jetee), prolongation de delai
//  (bmCommandStatus = 2) : on relit et le delai repart. Message trop long pour
//  le tampon : lu jusqu'au bout et jete (le suivant ne demarre pas en plein
//  milieu d'un autre).
//
//  Fournis par pcsc_firmware.ino avant l'include : UsbEvt, MSG_*, g_usbEvtQueue,
//  ccidWake(), g_diag, diagStep().
// ===========================================================================
#pragma once
#include "usb/usb_host.h"
#include "esp_intr_alloc.h"

#define CCID_BUF_MAX    2048     // plafond des tampons de transfert / d'un message
#define CCID_TO_SLOT    1500     // GetSlotStatus, PowerOff, SetParameters (ms)
#define CCID_TO_POWER   5000     // IccPowerOn (ms)
#define CCID_TO_XFR     10000    // XfrBlock (ms, relance a chaque prolongation)

// Resultat d'un echange
enum { CC_OK = 0, CC_FAILED, CC_TIMEOUT, CC_USB, CC_PROTO, CC_GONE };
// Niveau d'echange du lecteur (dwFeatures bits 16-18)
enum { LVL_CHAR = 0, LVL_TPDU, LVL_APDU, LVL_XAPDU };
static const char* const LVL_NAMES[] = { "CHAR", "TPDU", "APDU", "XAPDU" };

// dwFeatures utiles
#define CCID_F_AUTO_VOLTAGE   0x00000008u   // choix automatique de la tension
#define CCID_F_AUTO_NEGOT     0x00000040u   // negociation automatique des parametres

struct CcidReader {
  bool                open;          // lecteur CCID utilisable ouvert
  usb_device_handle_t dev;
  uint8_t             addr, intf, epIn, epOut;
  uint16_t            mpsIn, mpsOut;
  size_t              inSize;        // taille du tampon IN (multiple du MPS)
  usb_transfer_t*     xOut;
  usb_transfer_t*     xIn;
  usb_transfer_t*     xCtrl;
  uint8_t             voltage;       // bVoltageSupport (bit0 5 V, bit1 3 V, bit2 1,8 V)
  uint32_t            protocols;     // dwProtocols
  uint32_t            features;      // dwFeatures
  uint32_t            maxMsg;        // dwMaxCCIDMessageLength
  uint8_t             level;         // LVL_*
  uint8_t             seq;           // bSeq du prochain message
  uint16_t            vid, pid;
  char                name[32];      // nom produit (descripteur de chaine)
  uint8_t             lastErr;       // bError du dernier message en echec
};
static CcidReader g_rd = {};
static bool       g_cardPowered = false;   // carte mise sous tension par nous
static bool       g_usbStuck    = false;   // un transfert n'a jamais ete rendu
static bool       g_usbHostOk   = false;   // pile USB Host installee (sinon : aucun appel usb_host_*)

static usb_host_client_handle_t g_usbClient = nullptr;
static SemaphoreHandle_t g_semOut = nullptr, g_semIn = nullptr, g_semCtrl = nullptr;
static uint8_t g_ccidRsp[CCID_BUF_MAX];    // dernier message RDR_to_PC complet (en-tete + donnees)
static size_t  g_ccidRspLen = 0;

static inline uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------- callbacks
static void usbXferCb(usb_transfer_t* t) {
  xSemaphoreGive((SemaphoreHandle_t)t->context);
}

static void usbClientCb(const usb_host_client_event_msg_t* ev, void*) {
  UsbEvt m = {};
  if (ev->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    m.type = MSG_NEW_DEV;
    m.addr = ev->new_dev.address;
  } else if (ev->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    m.type = MSG_DEV_GONE;
    m.hdl  = (void*)ev->dev_gone.dev_hdl;
  } else {
    return;
  }
  xQueueSend(g_usbEvtQueue, &m, 0);   // jamais bloquant : la tache ccid peut attendre un transfert
  ccidWake();
}

static void usbLibTask(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
}

static void usbCliTask(void*) {
  for (;;) usb_host_client_handle_events(g_usbClient, portMAX_DELAY);
}

// Installe la pile USB Host. Ordre : install -> client -> taches, pour qu'aucun
// NEW_DEV ne soit perdu si le lecteur est deja branche au demarrage.
static bool usbHostBegin() {
  g_semOut  = xSemaphoreCreateBinary();
  g_semIn   = xSemaphoreCreateBinary();
  g_semCtrl = xSemaphoreCreateBinary();
  usb_host_config_t cfg = {};
  cfg.skip_phy_setup = false;
  cfg.intr_flags     = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&cfg) != ESP_OK) return false;
  usb_host_client_config_t cc = {};
  cc.is_synchronous    = false;
  cc.max_num_event_msg = 8;
  cc.async.client_event_callback = usbClientCb;
  cc.async.callback_arg          = nullptr;
  if (usb_host_client_register(&cc, &g_usbClient) != ESP_OK) return false;
  xTaskCreatePinnedToCore(usbLibTask, "usbLib", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(usbCliTask, "usbCli", 4096, nullptr, 5, nullptr, 0);
  g_usbHostOk = true;
  return true;
}

// ---------------------------------------------------------------- transferts
static int usbSubmitWait(usb_transfer_t* t, uint32_t ms, bool ctrl);

// Endpoint bloque (STALL) : remise a zero cote hote + CLEAR_FEATURE(ENDPOINT_HALT).
static void usbClearStall(uint8_t ep) {
  usb_host_endpoint_halt(g_rd.dev, ep);
  usb_host_endpoint_flush(g_rd.dev, ep);
  usb_host_endpoint_clear(g_rd.dev, ep);
  usb_setup_packet_t* sp = (usb_setup_packet_t*)g_rd.xCtrl->data_buffer;
  sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                      USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
  sp->bRequest = USB_B_REQUEST_CLEAR_FEATURE;
  sp->wValue   = 0;                  // ENDPOINT_HALT
  sp->wIndex   = ep;
  sp->wLength  = 0;
  g_rd.xCtrl->num_bytes = sizeof(usb_setup_packet_t);
  usbSubmitWait(g_rd.xCtrl, 1000, true);
}

// Soumet t et attend son callback. Au retour, t n'est JAMAIS en vol (sauf
// g_usbStuck, si l'annulation elle-meme n'a jamais ete rendue).
static int usbSubmitWait(usb_transfer_t* t, uint32_t ms, bool ctrl) {
  SemaphoreHandle_t s = (SemaphoreHandle_t)t->context;
  xSemaphoreTake(s, 0);                                   // purge d'un don tardif
  esp_err_t e = ctrl ? usb_host_transfer_submit_control(g_usbClient, t) : usb_host_transfer_submit(t);
  if (e != ESP_OK) return CC_USB;
  if (xSemaphoreTake(s, pdMS_TO_TICKS(ms)) != pdTRUE) {
    if (!ctrl) {
      usb_host_endpoint_halt(t->device_handle, t->bEndpointAddress);
      usb_host_endpoint_flush(t->device_handle, t->bEndpointAddress);
    }
    bool back = xSemaphoreTake(s, pdMS_TO_TICKS(1000)) == pdTRUE;
    if (!ctrl) usb_host_endpoint_clear(t->device_handle, t->bEndpointAddress);
    if (!back) { g_usbStuck = true; diagStep("STUCK"); }
    return CC_TIMEOUT;
  }
  switch (t->status) {
    case USB_TRANSFER_STATUS_COMPLETED: return CC_OK;
    case USB_TRANSFER_STATUS_NO_DEVICE: return CC_GONE;
    case USB_TRANSFER_STATUS_STALL:
      if (!ctrl) usbClearStall(t->bEndpointAddress);
      return CC_USB;
    default:
      return CC_USB;
  }
}

// Envoie un message PC_to_RDR (slot 0) et attend la reponse RDR_to_PC de meme
// bSeq, rangee complete dans g_ccidRsp. CC_FAILED = bmCommandStatus « echec »
// (bError dans g_rd.lastErr ; la reponse reste lisible, ex. bmICCStatus).
static int ccidXfer(uint8_t type, const uint8_t* data, size_t len,
                    uint8_t b7, uint8_t b8, uint8_t b9, uint32_t timeoutMs) {
  if (!g_rd.open) return CC_GONE;
  if (g_usbStuck) return CC_USB;
  if (10 + len > g_rd.xOut->data_buffer_size) return CC_PROTO;
  uint8_t* o = g_rd.xOut->data_buffer;
  uint8_t seq = g_rd.seq++;
  o[0] = type;
  o[1] = (uint8_t)len; o[2] = (uint8_t)(len >> 8); o[3] = (uint8_t)(len >> 16); o[4] = (uint8_t)(len >> 24);
  o[5] = 0;            // bSlot
  o[6] = seq;
  o[7] = b7; o[8] = b8; o[9] = b9;
  if (len) memcpy(o + 10, data, len);
  g_rd.xOut->num_bytes = 10 + len;
  int r = usbSubmitWait(g_rd.xOut, 2000, false);
  if (r != CC_OK) return r;

  uint32_t start = millis();
  for (int guard = 0; guard < 64; ++guard) {
    uint32_t el = millis() - start;
    if (el >= timeoutMs) return CC_TIMEOUT;
    g_rd.xIn->num_bytes = g_rd.mpsIn;                       // 1er paquet : exactement un MPS
    r = usbSubmitWait(g_rd.xIn, timeoutMs - el, false);
    if (r != CC_OK) return r;
    size_t got = g_rd.xIn->actual_num_bytes;
    if (got == 0) continue;                                 // ZLP residuel : ignore
    if (got < 10) return CC_PROTO;
    memcpy(g_ccidRsp, g_rd.xIn->data_buffer, got);
    uint32_t dwLength = le32(g_ccidRsp + 1);
    if (dwLength > 65544) return CC_PROTO;                  // en-tete aberrant (max CCID : 64 Ko + 8)
    size_t total = 10 + dwLength;
    bool   discard = total > sizeof(g_ccidRsp);             // trop long : lu puis jete
    if (got > total) got = total;
    while (got < total) {                                   // suite du message
      el = millis() - start;
      if (el >= timeoutMs) return CC_TIMEOUT;
      size_t want = usb_round_up_to_mps(total - got, g_rd.mpsIn);
      if (want > g_rd.inSize) want = g_rd.inSize;
      g_rd.xIn->num_bytes = want;
      r = usbSubmitWait(g_rd.xIn, timeoutMs - el, false);
      if (r != CC_OK) return r;
      size_t n = g_rd.xIn->actual_num_bytes;
      if (n == 0) return CC_PROTO;
      if (n > total - got) n = total - got;
      if (!discard) memcpy(g_ccidRsp + got, g_rd.xIn->data_buffer, n);
      got += n;
    }
    if (discard) return CC_PROTO;
    g_ccidRspLen = total;
    if (g_ccidRsp[6] != seq) continue;                      // reponse perimee (echange annule)
    uint8_t cmdStatus = g_ccidRsp[7] >> 6;
    if (cmdStatus == 2) { start = millis(); continue; }     // prolongation de delai
    if (cmdStatus == 1) { g_rd.lastErr = g_ccidRsp[8]; return CC_FAILED; }
    return CC_OK;
  }
  return CC_PROTO;
}

// ---------------------------------------------------------------- messages CCID
// PC_to_RDR_GetSlotStatus -> bmICCStatus (0 active, 1 presente inactive, 2 absente).
static int ccidSlotStatus(uint8_t* icc) {
  int r = ccidXfer(0x65, nullptr, 0, 0, 0, 0, CCID_TO_SLOT);
  if (r != CC_OK && r != CC_FAILED) return r;
  if (g_ccidRspLen < 10 || g_ccidRsp[0] != 0x81) return CC_PROTO;
  *icc = g_ccidRsp[7] & 0x03;
  return CC_OK;
}

// PC_to_RDR_IccPowerOn (bPowerSelect : 0 auto, 1 5 V, 2 3 V, 3 1,8 V) -> ATR.
static int ccidPowerOn(uint8_t powerSelect, uint8_t* atr, uint8_t* atrLen) {
  int r = ccidXfer(0x62, nullptr, 0, powerSelect, 0, 0, CCID_TO_POWER);
  if (r != CC_OK) return r;
  if (g_ccidRsp[0] != 0x80 || g_ccidRspLen < 12) return CC_PROTO;
  size_t n = g_ccidRspLen - 10;
  if (n > 33) n = 33;
  memcpy(atr, g_ccidRsp + 10, n);
  *atrLen = (uint8_t)n;
  g_cardPowered = true;
  return CC_OK;
}

static void ccidPowerOff() {
  ccidXfer(0x63, nullptr, 0, 0, 0, 0, CCID_TO_SLOT);
  g_cardPowered = false;
}

// PC_to_RDR_SetParameters, protocole T=0 : bmFindexDindex, bmTCCKST0 (0x02 =
// convention inverse), bGuardTimeT0, bWaitingIntegerT0, bClockStop.
static int ccidSetParamsT0(uint8_t fidi, uint8_t tcck, uint8_t guard, uint8_t wi) {
  const uint8_t p[5] = { fidi, tcck, guard, wi, 0x00 };
  return ccidXfer(0x61, p, sizeof(p), 0x00, 0, 0, CCID_TO_SLOT);
}

// PC_to_RDR_XfrBlock (TPDU ou APDU courte selon le lecteur) -> reponse brute
// (donnees + SW) pointee dans g_ccidRsp.
static int ccidXfrBlock(const uint8_t* cmd, size_t n, const uint8_t** rsp, size_t* rspLen) {
  int r = ccidXfer(0x6F, cmd, n, 0, 0, 0, CCID_TO_XFR);
  if (r != CC_OK) return r;
  if (g_ccidRsp[0] != 0x80) return CC_PROTO;
  *rsp    = g_ccidRsp + 10;
  *rspLen = g_ccidRspLen - 10;
  return CC_OK;
}

// ---------------------------------------------------------------- attache / detache
// Chaine USB (UTF-16LE) -> ASCII imprimable, sans les separateurs du protocole.
static void usbStrAscii(const usb_str_desc_t* d, char* out, size_t max) {
  size_t n = 0;
  if (d && d->bLength >= 2) {
    int cnt = (d->bLength - 2) / 2;
    for (int i = 0; i < cnt && n + 1 < max; ++i) {
      uint16_t c = d->wData[i];
      out[n++] = (c >= 0x20 && c < 0x7F && c != ';' && c != '=' && c != '|') ? (char)c : '?';
    }
  }
  out[n] = 0;
}

static void ccidFreeTransfers(CcidReader& r) {
  if (r.xOut)  usb_host_transfer_free(r.xOut);
  if (r.xIn)   usb_host_transfer_free(r.xIn);
  if (r.xCtrl) usb_host_transfer_free(r.xCtrl);
  r.xOut = r.xIn = r.xCtrl = nullptr;
}

static void ccidSetupTransfer(usb_transfer_t* t, usb_device_handle_t dev, uint8_t ep, SemaphoreHandle_t s) {
  t->device_handle    = dev;
  t->bEndpointAddress = ep;
  t->callback         = usbXferCb;
  t->context          = (void*)s;
  t->flags            = 0;          // pas de ZLP en OUT (comme libccid)
}

// Ouvre le peripherique `addr`, cherche une interface CCID et la prend. Un
// peripherique non CCID (ou un lecteur de niveau caractere) est referme et
// note dans g_diag.lastDev. Renvoie true si g_rd est pret.
static bool ccidAttach(uint8_t addr) {
  if (!g_usbHostOk || g_usbStuck) return false;
  usb_device_handle_t dev;
  if (usb_host_device_open(g_usbClient, addr, &dev) != ESP_OK) return false;
  const usb_device_desc_t* dd = nullptr;
  const usb_config_desc_t* cd = nullptr;
  if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK || !dd ||
      usb_host_get_active_config_descriptor(dev, &cd) != ESP_OK || !cd) {
    usb_host_device_close(g_usbClient, dev);
    return false;
  }

  // Parcours du descripteur de configuration. Candidat = interface (alt 0) de
  // classe 0x0B, ou 0xFF portant un descripteur de classe CCID (0x21, 54 o). Le
  // descripteur 0x21 peut se trouver n'importe ou avant l'interface suivante.
  struct Cand { bool on; uint8_t intf; const uint8_t* cls; uint8_t epIn, epOut; uint16_t mpsIn, mpsOut; };
  Cand c = {}, best = {};
  auto complete = [](const Cand& k) { return k.on && k.cls && k.epIn && k.epOut; };
  const uint8_t* p = (const uint8_t*)cd;
  int total = cd->wTotalLength;
  for (int pos = 0; pos + 2 <= total;) {
    uint8_t len = p[pos], type = p[pos + 1];
    if (len < 2 || pos + len > total) break;
    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
      if (complete(c)) { best = c; break; }
      c = {};
      uint8_t cls = p[pos + 5];
      c.on   = (p[pos + 3] == 0) && (cls == USB_CLASS_CSCID || cls == USB_CLASS_VENDOR_SPEC);
      c.intf = p[pos + 2];
    } else if (c.on && type == 0x21 && len >= 0x36) {
      c.cls = p + pos;
    } else if (c.on && type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && len >= 7) {
      uint8_t  ea  = p[pos + 2], at = p[pos + 3];
      uint16_t mps = (uint16_t)(p[pos + 4] | (p[pos + 5] << 8)) & USB_W_MAX_PACKET_SIZE_MPS_MASK;
      if ((at & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK && mps) {
        if (ea & 0x80) { if (!c.epIn)  { c.epIn  = ea; c.mpsIn  = mps; } }
        else           { if (!c.epOut) { c.epOut = ea; c.mpsOut = mps; } }
      }
    }
    pos += len;
  }
  if (!best.on && complete(c)) best = c;

  snprintf(g_diag.lastDev, sizeof(g_diag.lastDev), "%04X:%04X", dd->idVendor, dd->idProduct);
  if (!complete(best)) {
    strncat(g_diag.lastDev, "(NOT_CCID)", sizeof(g_diag.lastDev) - strlen(g_diag.lastDev) - 1);
    usb_host_device_close(g_usbClient, dev);
    return false;
  }
  const uint8_t* k = best.cls;
  uint32_t features = le32(k + 40);
  uint8_t  level = (features & 0x00040000u) ? LVL_XAPDU :
                   (features & 0x00020000u) ? LVL_APDU  :
                   (features & 0x00010000u) ? LVL_TPDU  : LVL_CHAR;
  if (level == LVL_CHAR) {                                  // niveau caractere : non gere
    strncat(g_diag.lastDev, "(CHAR)", sizeof(g_diag.lastDev) - strlen(g_diag.lastDev) - 1);
    usb_host_device_close(g_usbClient, dev);
    return false;
  }
  if (usb_host_interface_claim(g_usbClient, dev, best.intf, 0) != ESP_OK) {
    usb_host_device_close(g_usbClient, dev);
    return false;
  }

  CcidReader r = {};
  r.dev = dev; r.addr = addr; r.intf = best.intf;
  r.epIn = best.epIn; r.epOut = best.epOut; r.mpsIn = best.mpsIn; r.mpsOut = best.mpsOut;
  r.voltage   = k[5];
  r.protocols = le32(k + 6);
  r.features  = features;
  r.maxMsg    = le32(k + 44);
  r.level     = level;
  r.vid = dd->idVendor; r.pid = dd->idProduct;

  size_t sz = r.maxMsg < 512 ? 512 : r.maxMsg;              // tampons : >= dwMaxCCIDMessageLength, plafonnes
  if (sz > CCID_BUF_MAX) sz = CCID_BUF_MAX;
  r.inSize = usb_round_up_to_mps(sz, r.mpsIn);
  if (usb_host_transfer_alloc(sz, 0, &r.xOut) != ESP_OK ||
      usb_host_transfer_alloc(r.inSize, 0, &r.xIn) != ESP_OK ||
      usb_host_transfer_alloc(64, 0, &r.xCtrl) != ESP_OK) {
    ccidFreeTransfers(r);
    usb_host_interface_release(g_usbClient, dev, best.intf);
    usb_host_device_close(g_usbClient, dev);
    return false;
  }
  ccidSetupTransfer(r.xOut,  dev, r.epOut, g_semOut);
  ccidSetupTransfer(r.xIn,   dev, r.epIn,  g_semIn);
  ccidSetupTransfer(r.xCtrl, dev, 0,       g_semCtrl);

  usb_device_info_t info;
  if (usb_host_device_info(dev, &info) == ESP_OK) usbStrAscii(info.str_desc_product, r.name, sizeof(r.name));
  r.open = true;
  g_rd = r;
  g_cardPowered = false;
  g_diag.lastDev[0] = 0;
  return true;
}

// Relache l'interface, ferme le peripherique, libere les transferts (jamais en
// vol ici : les echanges sont synchrones). Ordre impose par l'IDF. L'IDF ne
// decompte un transfert termine qu'APRES le retour de son callback (tache
// usbCli) : juste apres un echange, release/close peuvent repondre
// INVALID_STATE quelques instants -> nouvelles tentatives.
static void ccidDetach() {
  if (!g_rd.open) return;
  for (int i = 0; i < 20; ++i) {
    if (usb_host_interface_release(g_usbClient, g_rd.dev, g_rd.intf) != ESP_ERR_INVALID_STATE) break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  for (int i = 0; i < 20; ++i) {
    if (usb_host_device_close(g_usbClient, g_rd.dev) != ESP_ERR_INVALID_STATE) break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (!g_usbStuck) ccidFreeTransfers(g_rd);        // bloque : on prefere une fuite a un plantage
  g_rd = {};
  g_cardPowered = false;
}

// Trame de diagnostic (commande INFO).
static void ccidInfo(char* out, size_t max) {
  char atr[2 * sizeof(g_diag.atr) + 1];
  size_t n = 0;
  for (uint8_t i = 0; i < g_diag.atrLen; ++i) n += snprintf(atr + n, sizeof(atr) - n, "%02X", g_diag.atr[i]);
  atr[n] = 0;
  const char* step = g_usbStuck ? "STUCK" : (g_diag.step[0] ? g_diag.step : "NONE");
  if (g_rd.open && !g_usbStuck) {
    snprintf(out, max, "INFO:READER=%04X:%04X;NAME=%s;LEVEL=%s;FEAT=%08lX;ATR=%s;STEP=%s;SW=%04X;MS=%lu",
             g_rd.vid, g_rd.pid, g_rd.name[0] ? g_rd.name : "?", LVL_NAMES[g_rd.level],
             (unsigned long)g_rd.features, atr[0] ? atr : "NONE", step, g_diag.sw,
             (unsigned long)g_diag.ms);
  } else {
    snprintf(out, max, "INFO:READER=NONE;LASTDEV=%s;STEP=%s",
             g_diag.lastDev[0] ? g_diag.lastDev : "NONE", step);
  }
}
