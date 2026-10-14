// ===========================================================================
//  apdu_t0.h  —  Couche APDU au-dessus de PC_to_RDR_XfrBlock (cartes T=0)
// ---------------------------------------------------------------------------
//  Un seul chemin pour les lecteurs de niveau TPDU et APDU (un lecteur APDU
//  renvoie parfois lui aussi 61xx) :
//    - cas 1 (4 octets)       -> P3 = 00 ajoute (forme TPDU) ;
//    - cas 4 (Lc + donnees + Le) -> envoye en cas 3 (Le retire, regle T=0) ;
//    - 61xx -> GET RESPONSE 00 C0 00 00 xx, en boucle, donnees concatenees ;
//    - 6Cxx -> la commande (forme 5 octets) est renvoyee avec P3 = xx.
//  Les octets de procedure T=0 eux-memes sont geres par le lecteur.
// ===========================================================================
#pragma once

#define APDU_RESP_MAX  512

static uint8_t g_apduData[APDU_RESP_MAX];   // donnees de reponse (sans le SW)

// Transmet une APDU courte. Renvoie le SW final, ou 0 si l'echange avec le
// lecteur a echoue (carte retiree, lecteur parti, delai depasse...). Les
// donnees cumulees (sans SW) sont dans out[0..*outLen).
static uint16_t apduTransmit(const uint8_t* cmd, size_t n, uint8_t* out, size_t outMax, size_t* outLen) {
  uint8_t tx[5 + 255];
  size_t  txLen = n;
  *outLen = 0;
  if (n < 4 || n > sizeof(tx) + 1) return 0;
  memcpy(tx, cmd, n > sizeof(tx) ? sizeof(tx) : n);
  if (n == 4) { tx[4] = 0x00; txLen = 5; }                       // cas 1
  else if (n > 5 && n == 6u + cmd[4]) txLen = n - 1;              // cas 4 -> cas 3

  for (int i = 0; i < 16; ++i) {
    const uint8_t* r;
    size_t rl;
    dbgHex("APDU>", tx, txLen);
    if (ccidXfrBlock(tx, txLen, &r, &rl) != CC_OK || rl < 2) {
      dbg("APDU : pas de reponse exploitable de la carte");
      return 0;
    }
    dbgHex("APDU<", r, rl);
    uint8_t sw1 = r[rl - 2], sw2 = r[rl - 1];
    size_t dl = rl - 2, room = outMax - *outLen;
    if (dl > room) dl = room;
    memcpy(out + *outLen, r, dl);
    *outLen += dl;
    if (sw1 == 0x61) {                                            // donnees disponibles
      tx[0] = 0x00; tx[1] = 0xC0; tx[2] = 0x00; tx[3] = 0x00; tx[4] = sw2;
      txLen = 5;
      continue;
    }
    if (sw1 == 0x6C && txLen == 5) { tx[4] = sw2; continue; }     // mauvaise longueur Le
    g_diag.sw = (uint16_t)((sw1 << 8) | sw2);
    return g_diag.sw;
  }
  return 0;
}
