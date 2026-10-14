// ===========================================================================
//  cartes.h  —  Identification des cartes qui ne sont PAS des Carte Vitale 2
// ---------------------------------------------------------------------------
//  Appele par vitaleRead() quand l'application Vitale est absente. Uniquement
//  des donnees PUBLIQUES (lisibles sans PIN ni cle), en lecture seule :
//
//    CPS      ATR des cartes ANS (octets 5..10 = 00 12 25 00 64 80). Identite
//             tiree du 1er certificat PKCS#15 : EF.DIR -> application -> ODF
//             (5031) -> CDF -> certificat X.509 -> sujet (nom, prenom,
//             etablissement O, identifiant serialNumber, profession title).
//    CB       carte bancaire EMV : annuaire 1PAY.SYS.DDF01 (sinon AID connus) ->
//             reseaux (CB, VISA, MASTERCARD...), libelle ; puis GET PROCESSING
//             OPTIONS + READ RECORD, comme tout terminal (ni paiement, ni PIN ;
//             le compteur de transactions ATC de la carte avance de 1) ->
//             titulaire (5F20) et numero MASQUE (6 premiers + 4 derniers
//             chiffres). Le numero complet et la date d'expiration ne sont
//             JAMAIS emis ; en DEBUG les reponses sont masquees (g_dbgMask).
//    OPENPGP  GET DATA 4F (numero de serie) et 65 / 5B (nom « Nom<<Prenom »).
//    PIV      CHUID (GUID = numero) et certificat d'authentification (5FC105,
//             sinon 5FC101) -> sujet X.509. Certificat compresse : non lu.
//    ID       document d'identite ICAO (eMRTD) : reconnu seulement, les donnees
//             sont protegees (BAC/PACE, cle tiree de la MRZ). NB : CNIe et
//             passeport francais sont SANS CONTACT : invisibles ici.
//
//  Trame : CARD_READ_NOT_CV:<TYPE>[;RESEAU=..][;LIBELLE=..][;NOM=..][;PRENOM=..]
//          [;TITULAIRE=..][;ETAB=..][;IDENT=..][;PROFESSION=..][;NUM=..]
//  Valeurs en ASCII (accents replies : e/E...), sans ; = | (-> espace).
//
//  Fournis avant l'include : apduTransmit, g_apduData (apdu_t0.h), Tlv,
//  tlvNext, tlvSkipPad, readBin, g_vitBuf, appendf (vitale.h), g_diag,
//  dbg/dbgHex/g_dbgMask (debug_log.h).
// ===========================================================================
#pragma once
#include "esp_random.h"

#define CI_FIELD_MAX  56

struct CardInfo {
  char type[12];                 // CB, CPS, OPENPGP, PIV, ID
  char reseau[48];
  char libelle[CI_FIELD_MAX];
  char nom[CI_FIELD_MAX], prenom[CI_FIELD_MAX], titulaire[CI_FIELD_MAX];
  char etab[CI_FIELD_MAX], ident[CI_FIELD_MAX], profession[CI_FIELD_MAX];
  char num[40];
};
static CardInfo g_ci;
static bool     g_cardLost = false;          // echange en echec pendant l'identification

// ---------------------------------------------------------------- texte
// U+00C0..U+00FF replies en ASCII.
static const char FOLD_C0[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

static char foldCp(uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7F) {
    char c = (char)cp;
    return (c == ';' || c == '=' || c == '|') ? ' ' : c;
  }
  if (cp >= 0xC0 && cp <= 0xFF) return FOLD_C0[cp - 0xC0];
  if (cp == 0x152) return 'O';
  if (cp == 0x153) return 'o';
  return '?';
}

enum { ENC_ASCII, ENC_UTF8, ENC_LATIN1, ENC_UCS2 };

// Copie un texte de carte en ASCII imprimable, espaces de bord rognes.
static void textCopy(char* dst, size_t max, const uint8_t* v, size_t n, int enc) {
  size_t k = 0;
  for (size_t i = 0; v && i < n && k + 1 < max;) {
    uint32_t cp;
    if (enc == ENC_UCS2) {
      if (i + 1 >= n) break;
      cp = ((uint32_t)v[i] << 8) | v[i + 1];
      i += 2;
    } else if (enc == ENC_UTF8 && v[i] >= 0xC0 && v[i] < 0xE0 && i + 1 < n) {
      cp = ((uint32_t)(v[i] & 0x1F) << 6) | (v[i + 1] & 0x3F);
      i += 2;
    } else if (enc == ENC_UTF8 && v[i] >= 0xE0 && v[i] < 0xF0 && i + 2 < n) {
      cp = ((uint32_t)(v[i] & 0x0F) << 12) | ((uint32_t)(v[i + 1] & 0x3F) << 6) | (v[i + 2] & 0x3F);
      i += 3;
    } else {
      cp = v[i++];
      if (enc == ENC_ASCII && cp >= 0x80) cp = '?';
    }
    if (cp == 0 || cp == 0xFF) continue;            // bourrage
    dst[k++] = foldCp(cp);
  }
  dst[k] = 0;
  while (k && dst[k - 1] == ' ') dst[--k] = 0;
  size_t s = 0;
  while (dst[s] == ' ') ++s;
  if (s) memmove(dst, dst + s, k - s + 1);
}

static void strCopy(char* dst, size_t max, const char* src) {
  snprintf(dst, max, "%s", src);
}

static void hexCopy(char* dst, size_t max, const uint8_t* v, size_t n) {
  size_t k = 0;
  for (size_t i = 0; i < n && k + 3 <= max; ++i) k += snprintf(dst + k, max - k, "%02X", v[i]);
}

// ---------------------------------------------------------------- TLV / ASN.1
// Recherche recursive d'un tag dans des TLV (BER / DER / EMV).
static bool tlvFind(const uint8_t* p, size_t len, uint32_t tag, const uint8_t** val, size_t* vlen,
                    int depth = 0) {
  const uint8_t* end = p + len;
  for (p = tlvSkipPad(p, end); p < end; p = tlvSkipPad(p, end)) {
    Tlv t;
    const uint8_t* nx = tlvNext(p, end, &t);
    if (!nx) return false;
    if (t.tag == tag) { *val = t.val; *vlen = t.len; return true; }
    if (t.constructed && depth < 8 && tlvFind(t.val, t.len, tag, val, vlen, depth + 1)) return true;
    p = nx;
  }
  return false;
}

// Chaine X.509 -> texte (UTF8String, PrintableString, IA5, T61, BMPString).
static void x509Str(char* dst, size_t max, const Tlv& s) {
  int enc = (s.tag == 0x0C) ? ENC_UTF8 : (s.tag == 0x1E) ? ENC_UCS2 : (s.tag == 0x14) ? ENC_LATIN1 : ENC_ASCII;
  textCopy(dst, max, s.val, s.len, enc);
}

// Sujet d'un certificat X.509 (DER) -> nom, prenom, titulaire (CN),
// etablissement (O), identifiant (serialNumber), profession (title).
static bool x509Subject(const uint8_t* der, size_t len) {
  const uint8_t* end = der + len;
  Tlv cert, tbs, t;
  if (!tlvNext(der, end, &cert) || cert.tag != 0x30) return false;
  if (!tlvNext(cert.val, cert.val + cert.len, &tbs) || tbs.tag != 0x30) return false;
  const uint8_t* p = tbs.val;
  const uint8_t* e = tbs.val + tbs.len;
  const uint8_t* nx = tlvNext(p, e, &t);
  if (!nx) return false;
  if (t.tag == 0xA0) { p = nx; nx = tlvNext(p, e, &t); }      // version
  for (int i = 0; i < 4 && nx; ++i) { p = nx; nx = tlvNext(p, e, &t); }  // serie, algo, emetteur, validite -> sujet
  if (!nx || t.tag != 0x30) return false;
  bool any = false;
  const uint8_t* se = t.val + t.len;
  for (const uint8_t* q = t.val; q < se;) {                    // SET { SEQ { OID, valeur } }
    Tlv set, seq, oid, val;
    const uint8_t* qn = tlvNext(q, se, &set);
    if (!qn) break;
    if (set.tag == 0x31 && tlvNext(set.val, set.val + set.len, &seq) && seq.tag == 0x30) {
      const uint8_t* r = tlvNext(seq.val, seq.val + seq.len, &oid);
      if (r && oid.tag == 0x06 && oid.len == 3 && oid.val[0] == 0x55 && oid.val[1] == 0x04 &&
          tlvNext(r, seq.val + seq.len, &val)) {
        char* dst = nullptr;
        switch (oid.val[2]) {
          case 0x03: dst = g_ci.titulaire;  break;   // commonName
          case 0x04: dst = g_ci.nom;        break;   // surname
          case 0x2A: dst = g_ci.prenom;     break;   // givenName
          case 0x0A: dst = g_ci.etab;       break;   // organizationName
          case 0x05: dst = g_ci.ident;      break;   // serialNumber
          case 0x0C: dst = g_ci.profession; break;   // title
        }
        if (dst && !dst[0]) { x509Str(dst, CI_FIELD_MAX, val); any = true; }
      }
    }
    q = qn;
  }
  return any;
}

// ---------------------------------------------------------------- commandes
static uint16_t selectAidFci(const uint8_t* aid, uint8_t n, size_t* fciLen) {
  uint8_t c[5 + 16] = { 0x00, 0xA4, 0x04, 0x00, n };
  memcpy(c + 5, aid, n);
  uint16_t sw = apduTransmit(c, 5 + n, g_apduData, sizeof(g_apduData), fciLen);
  if (sw == 0) g_cardLost = true;
  return sw;
}

static uint16_t selectFile(uint8_t p1, const uint8_t* data, uint8_t n) {
  uint8_t c[5 + 16] = { 0x00, 0xA4, p1, 0x00, n };
  memcpy(c + 5, data, n);
  size_t l;
  uint16_t sw = apduTransmit(c, 5 + n, g_apduData, sizeof(g_apduData), &l);
  if (sw == 0) g_cardLost = true;
  return sw;
}

// Chemin PKCS#15 : absolu 3F00.. (P1=08), identifiant seul (P1=00), relatif (P1=09).
static uint16_t selectPath(const uint8_t* path, size_t n) {
  if (n < 2 || n > 16) return 0x6A82;
  if (path[0] == 0x3F && path[1] == 0x00)
    return n == 2 ? selectFile(0x00, path, 2) : selectFile(0x08, path + 2, n - 2);
  return n == 2 ? selectFile(0x00, path, 2) : selectFile(0x09, path, n);
}

// GET DATA (OpenPGP : 00 CA P1 P2 ; PIV : 00 CB 3F FF + liste de tags 5C).
static uint16_t getData(const uint8_t* cmd, size_t n, uint8_t* out, size_t outMax, size_t* outLen) {
  uint16_t sw = apduTransmit(cmd, n, out, outMax, outLen);
  if (sw == 0) g_cardLost = true;
  return sw;
}

// ---------------------------------------------------------------- CPS (PKCS#15)
static bool atrIsCps() {
  static const uint8_t SIG[] = { 0x00, 0x12, 0x25, 0x00, 0x64, 0x80 };
  return g_diag.atrLen >= 11 && g_diag.atr[0] == 0x3B && !memcmp(g_diag.atr + 5, SIG, sizeof(SIG));
}

// Lit le fichier courant : binaire, sinon enregistrements (EF.DIR).
static size_t readCurrent(uint8_t* buf, size_t max) {
  size_t len = 0;
  uint16_t sw = readBin(buf, max, &len);
  if (sw == 0x9000 && len) return len;
  if (sw == 0) { g_cardLost = true; return 0; }
  len = 0;
  for (uint8_t rec = 1; rec <= 8 && len < max; ++rec) {       // fichier a enregistrements
    const uint8_t c[5] = { 0x00, 0xB2, rec, 0x04, 0x00 };
    size_t n = 0;
    sw = apduTransmit(c, sizeof(c), g_apduData, sizeof(g_apduData), &n);
    if (sw == 0) { g_cardLost = true; return 0; }
    if (sw != 0x9000) break;
    if (n > max - len) n = max - len;
    memcpy(buf + len, g_apduData, n);
    len += n;
  }
  return len;
}

static void cpsRead() {
  static const uint8_t MF[] = { 0x3F, 0x00 }, DIR[] = { 0x2F, 0x00 }, ODF[] = { 0x50, 0x31 };
  uint8_t aid[16], appPath[16], path[16];
  size_t aidLen = 0, appLen = 0, pathLen = 0, len;
  const uint8_t* v;
  size_t vl;

  // EF.DIR -> application PKCS#15
  if (selectFile(0x00, MF, 2) == 0x9000 && selectFile(0x00, DIR, 2) == 0x9000 &&
      (len = readCurrent(g_vitBuf, sizeof(g_vitBuf))) > 0) {
    if (tlvFind(g_vitBuf, len, 0x4F, &v, &vl) && vl <= sizeof(aid)) { memcpy(aid, v, vl); aidLen = vl; }
    if (tlvFind(g_vitBuf, len, 0x51, &v, &vl) && vl <= sizeof(appPath)) { memcpy(appPath, v, vl); appLen = vl; }
  }
  if (g_cardLost) return;
  dbg("CPS : EF.DIR -> AID %u o, chemin %u o", (unsigned)aidLen, (unsigned)appLen);
  bool appOk = (aidLen && selectAidFci(aid, (uint8_t)aidLen, &len) == 0x9000) ||
               (appLen && selectPath(appPath, appLen) == 0x9000);
  if (!appOk || g_cardLost) { dbg("CPS : application PKCS#15 introuvable"); return; }

  // ODF -> CDF (certificats : A4, sinon A5 / A6)
  if (selectFile(0x00, ODF, 2) != 0x9000 || (len = readCurrent(g_vitBuf, sizeof(g_vitBuf))) == 0) {
    dbg("CPS : ODF illisible");
    return;
  }
  static const uint32_t CDF_TAGS[] = { 0xA4, 0xA5, 0xA6 };
  for (uint32_t tag : CDF_TAGS) {
    const uint8_t* cv;
    size_t cl;
    if (tlvFind(g_vitBuf, len, tag, &cv, &cl) && tlvFind(cv, cl, 0x04, &v, &vl) && vl <= sizeof(path)) {
      memcpy(path, v, vl);
      pathLen = vl;
      break;
    }
  }
  if (!pathLen || selectPath(path, pathLen) != 0x9000 || (len = readCurrent(g_vitBuf, sizeof(g_vitBuf))) == 0) {
    dbg("CPS : CDF introuvable");
    return;
  }
  // chemins des certificats (dans [1] typeAttributes de chaque objet)
  uint8_t certs[4][16];
  size_t certLen[4] = {}, nCerts = 0;
  const uint8_t* end = g_vitBuf + len;
  for (const uint8_t* p = tlvSkipPad(g_vitBuf, end); p < end && nCerts < 4; p = tlvSkipPad(p, end)) {
    Tlv o;
    const uint8_t* nx = tlvNext(p, end, &o);
    if (!nx) break;
    const uint8_t* a1;
    size_t a1l;
    if (o.tag == 0x30 && tlvFind(o.val, o.len, 0xA1, &a1, &a1l) && tlvFind(a1, a1l, 0x04, &v, &vl) &&
        vl <= 16) {
      memcpy(certs[nCerts], v, vl);
      certLen[nCerts++] = vl;
    }
    p = nx;
  }
  dbg("CPS : %u certificat(s) dans le CDF", (unsigned)nCerts);
  for (size_t i = 0; i < nCerts && !g_cardLost; ++i) {
    if (selectPath(certs[i], certLen[i]) != 0x9000) continue;
    len = readCurrent(g_vitBuf, sizeof(g_vitBuf));
    if (len && x509Subject(g_vitBuf, len)) { dbg("CPS : identite lue dans le certificat %u", (unsigned)i + 1); return; }
  }
}

// ---------------------------------------------------------------- carte bancaire (EMV)
struct EmvKnown { uint8_t len; uint8_t aid[8]; const char* net; };
static const EmvKnown EMV_KNOWN[] = {
  { 7, { 0xA0, 0x00, 0x00, 0x00, 0x42, 0x10, 0x10 }, "CB" },
  { 7, { 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10 }, "VISA" },
  { 7, { 0xA0, 0x00, 0x00, 0x00, 0x03, 0x20, 0x10 }, "VISA ELECTRON" },
  { 7, { 0xA0, 0x00, 0x00, 0x00, 0x03, 0x20, 0x20 }, "V PAY" },
  { 7, { 0xA0, 0x00, 0x00, 0x00, 0x04, 0x10, 0x10 }, "MASTERCARD" },
  { 7, { 0xA0, 0x00, 0x00, 0x00, 0x04, 0x30, 0x60 }, "MAESTRO" },
  { 6, { 0xA0, 0x00, 0x00, 0x00, 0x25, 0x01 }, "AMEX" },
  { 7, { 0xA0, 0x00, 0x00, 0x01, 0x52, 0x30, 0x10 }, "DISCOVER" },
  { 7, { 0xA0, 0x00, 0x00, 0x00, 0x65, 0x10, 0x10 }, "JCB" },
  { 8, { 0xA0, 0x00, 0x00, 0x03, 0x33, 0x01, 0x01, 0x01 }, "UNIONPAY" },
};
static const uint8_t PSE[] = { '1', 'P', 'A', 'Y', '.', 'S', 'Y', 'S', '.', 'D', 'D', 'F', '0', '1' };

struct EmvApp { uint8_t aid[16]; uint8_t len; char label[CI_FIELD_MAX]; };
static EmvApp g_emvApps[6];
static int    g_nEmvApps = 0;

static const char* emvNetwork(const uint8_t* aid, size_t n) {
  for (const EmvKnown& k : EMV_KNOWN)
    if (n >= k.len && !memcmp(aid, k.aid, k.len)) return k.net;
  for (const EmvKnown& k : EMV_KNOWN)                          // meme RID (5 octets)
    if (n >= 5 && !memcmp(aid, k.aid, 5)) return k.net;
  return "EMV";
}

static void emvAddApp(const uint8_t* aid, size_t n, const uint8_t* label, size_t ln) {
  if (!n || n > 16 || g_nEmvApps >= 6) return;
  for (int i = 0; i < g_nEmvApps; ++i)
    if (g_emvApps[i].len == n && !memcmp(g_emvApps[i].aid, aid, n)) return;
  EmvApp& a = g_emvApps[g_nEmvApps++];
  memcpy(a.aid, aid, n);
  a.len = (uint8_t)n;
  textCopy(a.label, sizeof(a.label), label, label ? ln : 0, ENC_ASCII);
}

// Applications de paiement : annuaire PSE, sinon AID connus. -1 = carte perdue.
static int emvList() {
  g_nEmvApps = 0;
  size_t l = 0;
  const uint8_t* v;
  size_t vl;
  uint16_t sw = selectAidFci(PSE, sizeof(PSE), &l);
  if (sw == 0) return -1;
  if (sw == 0x9000) {
    uint8_t sfi = 1;
    if (tlvFind(g_apduData, l, 0x88, &v, &vl) && vl >= 1) sfi = v[0];
    dbg("EMV : annuaire de paiement 1PAY.SYS.DDF01 (SFI %u)", sfi);
    for (uint8_t rec = 1; rec <= 8; ++rec) {
      const uint8_t c[5] = { 0x00, 0xB2, rec, (uint8_t)((sfi << 3) | 0x04), 0x00 };
      size_t rl = 0;
      sw = apduTransmit(c, sizeof(c), g_apduData, sizeof(g_apduData), &rl);
      if (sw == 0) { g_cardLost = true; return -1; }
      if (sw != 0x9000) break;
      const uint8_t* rv;
      size_t rvl;
      if (!tlvFind(g_apduData, rl, 0x70, &rv, &rvl)) { rv = g_apduData; rvl = rl; }
      const uint8_t* end = rv + rvl;
      for (const uint8_t* p = tlvSkipPad(rv, end); p < end; p = tlvSkipPad(p, end)) {
        Tlv e;
        const uint8_t* nx = tlvNext(p, end, &e);
        if (!nx) break;
        const uint8_t *aid, *lab;
        size_t al, ll;
        if (e.tag == 0x61 && tlvFind(e.val, e.len, 0x4F, &aid, &al)) {
          bool hl = tlvFind(e.val, e.len, 0x50, &lab, &ll);
          emvAddApp(aid, al, hl ? lab : nullptr, hl ? ll : 0);
        }
        p = nx;
      }
    }
  }
  if (g_nEmvApps == 0) {                                       // pas d'annuaire : AID connus
    for (const EmvKnown& k : EMV_KNOWN) {
      sw = selectAidFci(k.aid, k.len, &l);
      if (sw == 0) return -1;
      if (sw == 0x9000) {
        bool hl = tlvFind(g_apduData, l, 0x50, &v, &vl);
        emvAddApp(k.aid, k.len, hl ? v : nullptr, hl ? vl : 0);
      }
    }
  }
  return g_nEmvApps;
}

// Donnees du PDOL (GET PROCESSING OPTIONS) : zeros, sauf pays / devise
// (France, euro), type de terminal et nombre aleatoire.
static size_t emvPdolData(const uint8_t* pdol, size_t n, uint8_t* out, size_t max) {
  size_t k = 0;
  const uint8_t* p = pdol;
  const uint8_t* e = pdol + n;
  while (p < e) {
    uint32_t tag = *p++;
    if ((tag & 0x1F) == 0x1F) {
      while (p < e) {
        uint8_t b = *p++;
        tag = (tag << 8) | b;
        if (!(b & 0x80)) break;
      }
    }
    if (p >= e) break;
    uint8_t len = *p++;
    if (k + len > max) break;
    memset(out + k, 0, len);
    if (tag == 0x9F1A && len >= 2) { out[k + len - 2] = 0x02; out[k + len - 1] = 0x50; }   // France
    if (tag == 0x5F2A && len >= 2) { out[k + len - 2] = 0x09; out[k + len - 1] = 0x78; }   // euro
    if (tag == 0x9F35 && len >= 1) out[k] = 0x22;                                          // terminal marchand
    if (tag == 0x9F37) for (uint8_t i = 0; i < len; ++i) out[k + i] = (uint8_t)esp_random();
    k += len;
  }
  return k;
}

// Numero de carte (BCD, 5A ou piste 2 57 jusqu'au separateur D) -> masque
// 6 premiers + 4 derniers chiffres. Le numero complet n'est pas conserve.
static void emvPanMasked(const uint8_t* bcd, size_t n) {
  char d[20];
  size_t k = 0;
  for (size_t i = 0; i < n && k < 19; ++i) {
    uint8_t nib[2] = { (uint8_t)(bcd[i] >> 4), (uint8_t)(bcd[i] & 0x0F) };
    bool stop = false;
    for (uint8_t x : nib) {
      if (x > 9 || k >= 19) { stop = true; break; }
      d[k++] = (char)('0' + x);
    }
    if (stop) break;
  }
  if (k >= 12) {
    size_t o = 0;
    for (size_t i = 0; i < k; ++i) g_ci.num[o++] = (i < 6 || i >= k - 4) ? d[i] : '*';
    g_ci.num[o] = 0;
  }
  memset(d, 0, sizeof(d));
}

static void emvHolder(const uint8_t* v, size_t n) {
  char raw[CI_FIELD_MAX];
  textCopy(raw, sizeof(raw), v, n, ENC_ASCII);
  if (!raw[0] || !strcmp(raw, "/") || strstr(raw, "CARDHOLDER") || strstr(raw, "CARD HOLDER") ||
      strstr(raw, "VALUED CUSTOMER"))
    return;                                                    // nom generique
  char* sl = strchr(raw, '/');
  if (sl) {                                                    // ISO 7813 : NOM/PRENOM
    *sl = 0;
    textCopy(g_ci.nom, CI_FIELD_MAX, (const uint8_t*)raw, strlen(raw), ENC_ASCII);
    textCopy(g_ci.prenom, CI_FIELD_MAX, (const uint8_t*)sl + 1, strlen(sl + 1), ENC_ASCII);
  } else {
    strCopy(g_ci.titulaire, CI_FIELD_MAX, raw);
  }
}

// Un enregistrement : titulaire (5F20 / 9F0B), numero (5A ou 57).
static bool emvRecord(uint8_t sfi, uint8_t rec) {
  const uint8_t c[5] = { 0x00, 0xB2, rec, (uint8_t)((sfi << 3) | 0x04), 0x00 };
  size_t rl = 0;
  uint16_t sw = apduTransmit(c, sizeof(c), g_apduData, sizeof(g_apduData), &rl);
  if (sw == 0) { g_cardLost = true; return false; }
  if (sw != 0x9000) return false;
  const uint8_t* v;
  size_t vl;
  if (!g_ci.nom[0] && !g_ci.titulaire[0]) {
    if (tlvFind(g_apduData, rl, 0x9F0B, &v, &vl) || tlvFind(g_apduData, rl, 0x5F20, &v, &vl)) emvHolder(v, vl);
  }
  if (!g_ci.num[0]) {
    if (tlvFind(g_apduData, rl, 0x5A, &v, &vl) || tlvFind(g_apduData, rl, 0x57, &v, &vl)) emvPanMasked(v, vl);
  }
  memset(g_apduData, 0, rl);                                   // pas de donnee bancaire residuelle
  return true;
}

static void emvReadApp(const EmvApp& a) {
  size_t l = 0;
  if (selectAidFci(a.aid, a.len, &l) != 0x9000) return;
  const uint8_t* v;
  size_t vl;
  if (!g_ci.libelle[0] && (tlvFind(g_apduData, l, 0x9F12, &v, &vl) || tlvFind(g_apduData, l, 0x50, &v, &vl)))
    textCopy(g_ci.libelle, CI_FIELD_MAX, v, vl, ENC_ASCII);
  uint8_t pdol[64];
  size_t pdolLen = 0;
  if (tlvFind(g_apduData, l, 0x9F38, &v, &vl) && vl <= sizeof(pdol)) { memcpy(pdol, v, vl); pdolLen = vl; }

  uint8_t gpo[7 + 128] = { 0x80, 0xA8, 0x00, 0x00, 0x00, 0x83, 0x00 };
  size_t dl = emvPdolData(pdol, pdolLen, gpo + 7, 128);
  gpo[4] = (uint8_t)(dl + 2);
  gpo[6] = (uint8_t)dl;
  g_dbgMask = true;                                            // pas de donnees bancaires en DEBUG
  size_t rl = 0;
  uint16_t sw = apduTransmit(gpo, 7 + dl, g_apduData, sizeof(g_apduData), &rl);
  uint8_t afl[32];
  size_t aflLen = 0;
  if (sw == 0x9000 && rl >= 2) {
    Tlv t;
    if (g_apduData[0] == 0x80 && tlvNext(g_apduData, g_apduData + rl, &t) && t.len > 2) {
      aflLen = t.len - 2 > sizeof(afl) ? sizeof(afl) : t.len - 2;          // format 1 : AIP + AFL
      memcpy(afl, t.val + 2, aflLen);
    } else if (tlvFind(g_apduData, rl, 0x94, &v, &vl)) {                // format 2 : 77 { 82, 94 }
      aflLen = vl > sizeof(afl) ? sizeof(afl) : vl;
      memcpy(afl, v, aflLen);
    }
  } else if (sw == 0) {
    g_cardLost = true;
  }
  dbg("EMV : GET PROCESSING OPTIONS SW %04X, AFL %u entree(s)", sw, (unsigned)(aflLen / 4));
  int reads = 0;
  if (aflLen >= 4) {
    for (size_t i = 0; i + 3 < aflLen && reads < 16 && !g_cardLost; i += 4) {
      uint8_t sfi = afl[i] >> 3;
      for (uint8_t rec = afl[i + 1]; rec && rec <= afl[i + 2] && reads < 16 && !g_cardLost; ++rec, ++reads) {
        emvRecord(sfi, rec);
        if ((g_ci.nom[0] || g_ci.titulaire[0]) && g_ci.num[0]) goto done;
      }
    }
  } else if (!g_cardLost) {                                    // sans AFL : SFI 1 a 4
    for (uint8_t sfi = 1; sfi <= 4 && !g_cardLost; ++sfi)
      for (uint8_t rec = 1; rec <= 4 && !g_cardLost; ++rec) {
        if (!emvRecord(sfi, rec)) break;
        if ((g_ci.nom[0] || g_ci.titulaire[0]) && g_ci.num[0]) goto done;
      }
  }
done:
  g_dbgMask = false;
}

// -1 carte perdue, 0 pas EMV, 1 carte bancaire (g_ci rempli)
static int emvIdentify() {
  int n = emvList();
  if (n < 0) return -1;
  if (n == 0) return 0;
  strCopy(g_ci.type, sizeof(g_ci.type), "CB");
  // reseaux (CB d'abord), application internationale pour la lecture
  const EmvApp* intl = nullptr;
  size_t k = 0;
  for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < g_nEmvApps; ++i) {
      const char* net = emvNetwork(g_emvApps[i].aid, g_emvApps[i].len);
      bool isCb = !strcmp(net, "CB");
      if ((pass == 0) != isCb) continue;
      if (strstr(g_ci.reseau, net)) continue;
      k += snprintf(g_ci.reseau + k, sizeof(g_ci.reseau) - k, "%s%s", k ? "+" : "", net);
      if (k >= sizeof(g_ci.reseau)) k = sizeof(g_ci.reseau) - 1;
      if (!isCb && !intl) intl = &g_emvApps[i];
    }
  const EmvApp& app = intl ? *intl : g_emvApps[0];
  if (app.label[0]) strCopy(g_ci.libelle, CI_FIELD_MAX, app.label);
  dbg("EMV : carte bancaire, reseau(x) %s", g_ci.reseau);
  emvReadApp(app);
  return g_cardLost ? -1 : 1;
}

// ---------------------------------------------------------------- OpenPGP / PIV / ID
static int openpgpIdentify() {
  static const uint8_t AID[] = { 0xD2, 0x76, 0x00, 0x01, 0x24, 0x01 };
  size_t l;
  uint16_t sw = selectAidFci(AID, sizeof(AID), &l);
  if (sw == 0) return -1;
  if (sw != 0x9000) return 0;
  strCopy(g_ci.type, sizeof(g_ci.type), "OPENPGP");
  const uint8_t* v;
  size_t vl;
  const uint8_t aidCmd[5] = { 0x00, 0xCA, 0x00, 0x4F, 0x00 };  // AID complet : fabricant + numero de serie
  if (getData(aidCmd, sizeof(aidCmd), g_vitBuf, sizeof(g_vitBuf), &l) == 0x9000) {
    const uint8_t* a = g_vitBuf;
    size_t al = l;
    if (tlvFind(g_vitBuf, l, 0x4F, &v, &vl)) { a = v; al = vl; }
    if (al >= 14) {
      char man[5], ser[9];
      hexCopy(man, sizeof(man), a + 8, 2);
      hexCopy(ser, sizeof(ser), a + 10, 4);
      snprintf(g_ci.num, sizeof(g_ci.num), "%s-%s", man, ser);
    }
  }
  const uint8_t nameCmd[5] = { 0x00, 0xCA, 0x00, 0x65, 0x00 };  // donnees du porteur
  if (!g_cardLost && getData(nameCmd, sizeof(nameCmd), g_vitBuf, sizeof(g_vitBuf), &l) == 0x9000 &&
      tlvFind(g_vitBuf, l, 0x5B, &v, &vl)) {
    char raw[CI_FIELD_MAX];
    textCopy(raw, sizeof(raw), v, vl, ENC_LATIN1);
    char* sep = strstr(raw, "<<");                             // « Nom<<Prenom »
    if (sep) {
      *sep = 0;
      strCopy(g_ci.nom, CI_FIELD_MAX, raw);
      strCopy(g_ci.prenom, CI_FIELD_MAX, sep + 2);
    } else {
      strCopy(g_ci.titulaire, CI_FIELD_MAX, raw);
    }
    char* fields[] = { g_ci.nom, g_ci.prenom, g_ci.titulaire };
    for (char* f : fields)
      for (char* c = f; *c; ++c) if (*c == '<') *c = ' ';
  }
  dbg("OPENPGP : carte reconnue");
  return g_cardLost ? -1 : 1;
}

static int pivIdentify() {
  static const uint8_t AID[] = { 0xA0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00 };
  size_t l;
  uint16_t sw = selectAidFci(AID, sizeof(AID), &l);
  if (sw == 0) return -1;
  if (sw != 0x9000) return 0;
  strCopy(g_ci.type, sizeof(g_ci.type), "PIV");
  const uint8_t* v;
  size_t vl;
  // CHUID (5FC102) : GUID = numero de la carte
  uint8_t cmd[10] = { 0x00, 0xCB, 0x3F, 0xFF, 0x05, 0x5C, 0x03, 0x5F, 0xC1, 0x02 };
  if (getData(cmd, sizeof(cmd), g_vitBuf, sizeof(g_vitBuf), &l) == 0x9000 && tlvFind(g_vitBuf, l, 0x34, &v, &vl) &&
      vl == 16) {
    bool zero = true;
    for (size_t i = 0; i < vl; ++i) if (v[i]) zero = false;
    if (!zero) hexCopy(g_ci.num, sizeof(g_ci.num), v, vl);
  }
  // certificat d'authentification (5FC105), sinon de la carte (5FC101)
  static const uint8_t CERTS[] = { 0x05, 0x01 };
  for (uint8_t id : CERTS) {
    if (g_cardLost) break;
    cmd[9] = id;
    if (getData(cmd, sizeof(cmd), g_vitBuf, sizeof(g_vitBuf), &l) != 0x9000) continue;
    if (tlvFind(g_vitBuf, l, 0x71, &v, &vl) && vl >= 1 && (v[0] & 0x01)) { dbg("PIV : certificat compresse, non lu"); continue; }
    if (tlvFind(g_vitBuf, l, 0x70, &v, &vl) && x509Subject(v, vl)) break;
  }
  dbg("PIV : carte reconnue");
  return g_cardLost ? -1 : 1;
}

static int idIdentify() {
  static const uint8_t AID[] = { 0xA0, 0x00, 0x00, 0x02, 0x47, 0x10, 0x01 };
  size_t l;
  uint16_t sw = selectAidFci(AID, sizeof(AID), &l);
  if (sw == 0) return -1;
  if (sw != 0x9000) return 0;
  strCopy(g_ci.type, sizeof(g_ci.type), "ID");
  dbg("ID : document d'identite (eMRTD) reconnu ; donnees protegees par BAC/PACE");
  return 1;
}

// ---------------------------------------------------------------- point d'entree
// Identifie la carte presente (application Vitale absente). Ecrit la trame
// dans out et renvoie 1 ; 0 = type inconnu (out intact) ; -1 = echange perdu.
static int cardIdentify(char* out, size_t max) {
  memset(&g_ci, 0, sizeof(g_ci));
  g_cardLost = false;
  int r = 0;
  if (atrIsCps()) {
    strCopy(g_ci.type, sizeof(g_ci.type), "CPS");
    dbg("CPS : carte de professionnel de sante (ATR)");
    cpsRead();
    r = g_cardLost ? -1 : 1;
  }
  if (r == 0) r = emvIdentify();
  if (r == 0) r = openpgpIdentify();
  if (r == 0) r = pivIdentify();
  if (r == 0) r = idIdentify();
  if (r <= 0) {
    if (r == 0) dbg("CARTE : type non reconnu");
    return r;
  }
  size_t k = 0;
  out[0] = 0;
  appendf(out, max, &k, "%s%s", "CARD_READ_NOT_CV:", g_ci.type);
  const char* keys[] = { "RESEAU", "LIBELLE", "NOM", "PRENOM", "TITULAIRE", "ETAB", "IDENT", "PROFESSION", "NUM" };
  const char* vals[] = { g_ci.reseau, g_ci.libelle, g_ci.nom, g_ci.prenom, g_ci.titulaire, g_ci.etab,
                         g_ci.ident, g_ci.profession, g_ci.num };
  for (int i = 0; i < 9; ++i)
    if (vals[i][0]) appendf(out, max, &k, ";%s=%s", keys[i], vals[i]);
  memset(&g_ci, 0, sizeof(g_ci));
  return 1;
}
