// ===========================================================================
//  vitale.h  —  Lecture des donnees publiques d'une Carte Vitale 2
// ---------------------------------------------------------------------------
//  Sequence reprise du script open source Cardpeek `vitale_2.lua` (CLA 00) :
//    1. mise sous tension -> ATR (Vitale 2 : 3B 75 13 00 00 xx 09 EA 90 00, T=0) ;
//    2. SELECT de l'AID Vitale  D2 50 00 00 02 « VITALE » ;
//    3. SELECT EF 2001 (fichier de pointeurs) + READ BINARY par blocs de 256 o ;
//       E0{ A0{ 82 : cle = octets 0-1, AID = octets 4.. } 80 : nom = octets 0-7,
//       cle = octets 8-9, FID = octets 10-11 } ;
//    4. pour chaque application : SELECT AID, puis SELECT FID (P1 = 02) + lecture
//       de chacun de ses fichiers (contenu commencant par 00 ou 04 00 = brut,
//       ignore) ;
//    5. BER-TLV : chaque gabarit 65 = un beneficiaire {80 nom, 81 prenom,
//       82 date de naissance AAAAMMJJ, 90 nationalite, 93 NIR}.
//  Vitale 1 (ATR 3F 65 25 ..., format Bull B0' proprietaire) : reconnue, non lue.
//
//  Resultat (une trame) :
//    CARD_READ:NOM=..;PRENOM=..;NAISS=AAAAMMJJ;NIR=..[;NAT=..]|NOM=..   (un groupe par beneficiaire)
//    CARD_READ_NOT_CV:<code>   V1 | T1 | SW hexa du SELECT de l'AID refuse (ex. 6A82)
//    CARD_READ_ERROR           carte muette, echange en echec, aucun beneficiaire
// ===========================================================================
#pragma once

static const uint8_t AID_VITALE[] = { 0xD2, 0x50, 0x00, 0x00, 0x02, 'V', 'I', 'T', 'A', 'L', 'E' };

#define VIT_FILE_MAX   4096     // plafond d'un fichier lu
#define VIT_MAX_APPS   6
#define VIT_MAX_FILES  16       // fichiers lus au plus (toutes applications)
#define VIT_MAX_BENEF  6
#define VIT_FIELD_MAX  40

enum { VIT_OK = 0, VIT_ERROR, VIT_NOT_CV };   // resultat de vitaleRead()

static uint8_t g_vitBuf[VIT_FILE_MAX];

// ---------------------------------------------------------------- BER-TLV
struct Tlv { uint32_t tag; const uint8_t* val; size_t len; bool constructed; };

// Saute le bourrage (00 / FF) entre objets.
static const uint8_t* tlvSkipPad(const uint8_t* p, const uint8_t* end) {
  while (p < end && (*p == 0x00 || *p == 0xFF)) ++p;
  return p;
}

// Decode un objet en p. Renvoie l'adresse qui le suit, nullptr si mal forme.
static const uint8_t* tlvNext(const uint8_t* p, const uint8_t* end, Tlv* t) {
  if (p >= end) return nullptr;
  uint8_t b = *p++;
  uint32_t tag = b;
  t->constructed = (b & 0x20) != 0;
  if ((b & 0x1F) == 0x1F) {                         // tag sur plusieurs octets
    int k = 0;
    do {
      if (p >= end || ++k > 3) return nullptr;
      b = *p++;
      tag = (tag << 8) | b;
    } while (b & 0x80);
  }
  if (p >= end) return nullptr;
  size_t len = *p++;
  if (len & 0x80) {                                 // longueur 81 xx / 82 xx xx
    int nb = len & 0x7F;
    if (nb < 1 || nb > 2) return nullptr;
    len = 0;
    while (nb--) {
      if (p >= end) return nullptr;
      len = (len << 8) | *p++;
    }
  }
  if ((size_t)(end - p) < len) return nullptr;
  t->tag = tag;
  t->val = p;
  t->len = len;
  return p + len;
}

// ---------------------------------------------------------------- beneficiaires
struct Benef {
  char nom[VIT_FIELD_MAX], prenom[VIT_FIELD_MAX], naiss[VIT_FIELD_MAX];
  char nir[VIT_FIELD_MAX], nat[VIT_FIELD_MAX];
};
static Benef g_benef[VIT_MAX_BENEF];
static int   g_nBenef = 0;

// Valeur -> texte imprimable, sans les separateurs du protocole (; = |).
static void fieldCopy(char* dst, const uint8_t* v, size_t n) {
  while (n && (v[n - 1] == ' ' || v[n - 1] == 0x00 || v[n - 1] == 0xFF)) --n;
  size_t k = 0;
  for (size_t i = 0; i < n && k + 1 < VIT_FIELD_MAX; ++i) {
    uint8_t c = v[i];
    dst[k++] = (c >= 0x20 && c <= 0x7E && c != ';' && c != '=' && c != '|') ? (char)c : '?';
  }
  dst[k] = 0;
}

static void benefAdd(const uint8_t* p, size_t len) {
  if (g_nBenef >= VIT_MAX_BENEF) return;
  Benef& b = g_benef[g_nBenef];
  memset(&b, 0, sizeof(b));
  const uint8_t* end = p + len;
  bool any = false;
  for (p = tlvSkipPad(p, end); p < end; p = tlvSkipPad(p, end)) {
    Tlv t;
    const uint8_t* nx = tlvNext(p, end, &t);
    if (!nx) break;
    char* dst = nullptr;
    switch (t.tag) {
      case 0x80: dst = b.nom;    break;
      case 0x81: dst = b.prenom; break;
      case 0x82: dst = b.naiss;  break;
      case 0x90: dst = b.nat;    break;
      case 0x93: dst = b.nir;    break;
    }
    if (dst) { fieldCopy(dst, t.val, t.len); any = true; }
    p = nx;
  }
  if (any) { ++g_nBenef; dbg("VITALE : beneficiaire %d trouve (gabarit 65)", g_nBenef); }
}

// Cherche les gabarits 65 a toute profondeur (raisonnable).
static void tlvScan(const uint8_t* p, size_t len, int depth) {
  const uint8_t* end = p + len;
  for (p = tlvSkipPad(p, end); p < end; p = tlvSkipPad(p, end)) {
    Tlv t;
    const uint8_t* nx = tlvNext(p, end, &t);
    if (!nx) return;
    if (t.tag == 0x65) benefAdd(t.val, t.len);
    else if (t.constructed && depth < 4) tlvScan(t.val, t.len, depth + 1);
    p = nx;
  }
}

// ---------------------------------------------------------------- fichier de pointeurs
struct VitApp  { uint16_t key; uint8_t aid[16]; uint8_t aidLen; };
struct VitFile { uint16_t key; uint16_t fid; };
static VitApp  g_vitApps[VIT_MAX_APPS];
static VitFile g_vitFiles[VIT_MAX_FILES];
static int     g_nVitApps = 0, g_nVitFiles = 0;

static bool vitParsePointers(const uint8_t* buf, size_t len) {
  g_nVitApps = g_nVitFiles = 0;
  const uint8_t* end = buf + len;
  Tlv e0, a0;
  const uint8_t* p = tlvSkipPad(buf, end);
  if (!tlvNext(p, end, &e0) || e0.tag != 0xE0) return false;
  const uint8_t* q  = e0.val;
  const uint8_t* qe = e0.val + e0.len;
  q = tlvNext(tlvSkipPad(q, qe), qe, &a0);
  if (!q || a0.tag != 0xA0) return false;
  // Entrees 82 : les applications
  const uint8_t* ae = a0.val + a0.len;
  for (const uint8_t* r = tlvSkipPad(a0.val, ae); r < ae; r = tlvSkipPad(r, ae)) {
    Tlv x;
    const uint8_t* nx = tlvNext(r, ae, &x);
    if (!nx) break;
    if (x.tag == 0x82 && x.len > 4 && x.len - 4 <= sizeof(g_vitApps[0].aid) && g_nVitApps < VIT_MAX_APPS) {
      VitApp& a = g_vitApps[g_nVitApps++];
      a.key    = (uint16_t)((x.val[0] << 8) | x.val[1]);
      a.aidLen = (uint8_t)(x.len - 4);
      memcpy(a.aid, x.val + 4, a.aidLen);
    }
    r = nx;
  }
  // Enregistrements 80 (apres A0) : les fichiers
  for (q = tlvSkipPad(q, qe); q < qe; q = tlvSkipPad(q, qe)) {
    Tlv x;
    const uint8_t* nx = tlvNext(q, qe, &x);
    if (!nx) break;
    if (x.tag == 0x80 && x.len >= 12 && g_nVitFiles < VIT_MAX_FILES) {
      VitFile& f = g_vitFiles[g_nVitFiles++];
      f.key = (uint16_t)((x.val[8] << 8) | x.val[9]);
      f.fid = (uint16_t)((x.val[10] << 8) | x.val[11]);
    }
    q = nx;
  }
  for (int i = 0; i < g_nVitApps; ++i) dbgHex("VITALE pointeurs : application", g_vitApps[i].aid, g_vitApps[i].aidLen);
  for (int i = 0; i < g_nVitFiles; ++i)
    dbg("VITALE pointeurs : fichier %04X (cle %04X)", g_vitFiles[i].fid, g_vitFiles[i].key);
  return g_nVitApps > 0 && g_nVitFiles > 0;
}

// ---------------------------------------------------------------- commandes carte
static uint16_t selectAid(const uint8_t* aid, uint8_t n) {
  uint8_t c[5 + 16] = { 0x00, 0xA4, 0x04, 0x00, n };
  memcpy(c + 5, aid, n);
  size_t l;
  return apduTransmit(c, 5 + n, g_apduData, sizeof(g_apduData), &l);
}

static uint16_t selectFid(uint16_t fid) {
  const uint8_t c[7] = { 0x00, 0xA4, 0x02, 0x00, 0x02, (uint8_t)(fid >> 8), (uint8_t)fid };
  size_t l;
  return apduTransmit(c, sizeof(c), g_apduData, sizeof(g_apduData), &l);
}

// READ BINARY du fichier courant par blocs de 256 o (Le = 00), jusqu'a un bloc
// court, 6282 (fin de fichier), 6B00 (au-dela de la fin) ou un autre SW. Comme
// Cardpeek : 9000 des que des donnees ont ete lues. 0 = echec de l'echange.
static uint16_t readBin(uint8_t* buf, size_t max, size_t* outLen) {
  size_t   total = 0;
  uint16_t off = 0, sw = 0;
  for (;;) {
    const uint8_t c[5] = { 0x00, 0xB0, (uint8_t)((off >> 8) & 0x7F), (uint8_t)off, 0x00 };
    size_t n = 0;
    sw = apduTransmit(c, sizeof(c), g_apduData, sizeof(g_apduData), &n);
    if (sw == 0) { *outLen = total; return 0; }
    size_t k = n < max - total ? n : max - total;
    memcpy(buf + total, g_apduData, k);
    total += k;
    if (sw != 0x9000 || n != 256 || total >= max) break;
    off += 256;
    if (off >= 0x8000) break;
  }
  *outLen = total;
  return total ? 0x9000 : sw;
}

// ---------------------------------------------------------------- lecture
// ta1 : Fi/Di proposes (0x11 = valeurs par defaut). ta2 present = MODE
// SPECIFIQUE : la carte parle d'emblee avec les parametres de TA1 (si le bit
// 0x10 de TA2 est a 0), sans PPS possible. tc2 = WI (T=0, defaut 10).
struct AtrInfo { bool t0, t1, hasTa1, hasTa2; uint8_t ta1, ta2, tc1, tc2; };

static void atrParse(const uint8_t* a, size_t n, AtrInfo* r) {
  r->t0 = r->t1 = r->hasTa1 = r->hasTa2 = false;
  r->ta1 = 0x11;
  r->ta2 = 0;
  r->tc1 = 0;
  r->tc2 = 0x0A;
  if (n < 2) return;
  size_t  i = 2;
  uint8_t y = a[1] >> 4;
  bool    anyTd = false;
  for (int level = 1; level < 8; ++level) {
    if (y & 0x1) {                                            // TAi
      if (level == 1 && i < n) { r->ta1 = a[i]; r->hasTa1 = true; }
      if (level == 2 && i < n) { r->ta2 = a[i]; r->hasTa2 = true; }
      ++i;
    }
    if (y & 0x2) ++i;                                         // TBi
    if (y & 0x4) {                                            // TCi
      if (level == 1 && i < n) r->tc1 = a[i];
      if (level == 2 && i < n && a[i]) r->tc2 = a[i];
      ++i;
    }
    if (!(y & 0x8) || i >= n) break;                          // pas de TDi
    uint8_t td = a[i++];
    anyTd = true;
    if ((td & 0x0F) == 0) r->t0 = true;
    if ((td & 0x0F) == 1) r->t1 = true;
    y = td >> 4;
  }
  if (!anyTd) r->t0 = true;                                   // sans TD1 : T=0 implicite
}

// Mise sous tension : tension automatique si le lecteur sait la choisir (puis
// 5 V en secours), sinon la plus basse d'abord (3 V puis 5 V, ISO 7816-3).
static bool cardPowerUp() {
  uint8_t sel[3];
  int n = 0;
  if (g_rd.features & CCID_F_AUTO_VOLTAGE) { sel[n++] = 0; sel[n++] = 1; }
  else {
    if (g_rd.voltage & 0x02) sel[n++] = 2;
    if ((g_rd.voltage & 0x01) || n == 0) sel[n++] = 1;
  }
  static const char* const VNAME[] = { "auto", "5V", "3V", "1.8V" };
  for (int i = 0; i < n; ++i) {
    uint8_t len = 0;
    dbg("CARTE : mise sous tension (%s)", VNAME[sel[i] & 3]);
    int r = ccidPowerOn(sel[i], g_diag.atr, &len);
    if (r == CC_OK) {
      g_diag.atrLen = len;
      dbgHex("CARTE ATR", g_diag.atr, len);
      return true;
    }
    if (r != CC_FAILED) return false;               // erreur d'echange : inutile d'insister
    ccidPowerOff();
  }
  return false;
}

static void appendf(char* out, size_t max, size_t* k, const char* fmt, const char* a, const char* b) {
  if (*k >= max) return;
  int w = snprintf(out + *k, max - *k, fmt, a, b);
  if (w > 0) *k += (size_t)w;
  if (*k >= max) *k = max - 1;
}

// Lit la carte presente et ecrit la trame resultat dans out. Renvoie VIT_*.
static int vitaleRead(char* out, size_t max) {
  g_diag.step[0] = 0;
  g_diag.sw      = 0;
  g_diag.atrLen  = 0;
  snprintf(out, max, "CARD_READ_ERROR");

  if (!cardPowerUp()) { diagStep("POWERON"); return VIT_ERROR; }
  const uint8_t* a  = g_diag.atr;
  size_t         an = g_diag.atrLen;
  if (an >= 3 && a[0] == 0x3F && a[1] == 0x65 && a[2] == 0x25) {
    snprintf(out, max, "CARD_READ_NOT_CV:V1");
    return VIT_NOT_CV;
  }
  AtrInfo atr;
  atrParse(a, an, &atr);
  dbg("CARTE : ATR T=0 %s, T=1 %s, TC1=%u, convention %s", atr.t0 ? "oui" : "non", atr.t1 ? "oui" : "non",
      atr.tc1, (an && a[0] == 0x3F) ? "inverse" : "directe");
  if (g_rd.level == LVL_TPDU) {
    if (!atr.t0) { snprintf(out, max, "CARD_READ_NOT_CV:T1"); return VIT_NOT_CV; }
    // Lecteur TPDU sans negociation automatique : parametres T=0 explicites.
    //  - mode specifique (TA2 present, bit 0x10 a 0) : la carte parle DEJA
    //    avec le Fi/Di de TA1 -> on l'impose au lecteur (sinon erreurs de
    //    parite : cas vecu avec une carte bancaire 3B FF 13 ... TA2=00) ;
    //  - mode negociable : vitesse par defaut (0x11), pas de PPS.
    // Au mieux (erreur ignoree).
    if (!(g_rd.features & CCID_F_AUTO_NEGOT)) {
      bool    specific = atr.hasTa2 && !(atr.ta2 & 0x10);
      uint8_t fidi     = specific ? atr.ta1 : 0x11;
      dbg("CARTE : mode %s, SetParameters Fi/Di=0x%02X WI=%u", atr.hasTa2 ? "specifique" : "negociable",
          fidi, atr.tc2);
      ccidSetParamsT0(fidi, a[0] == 0x3F ? 0x02 : 0x00, atr.tc1, atr.tc2);
    }
  }

  uint16_t sw = selectAid(AID_VITALE, sizeof(AID_VITALE));
  if (sw == 0) { diagStep("SELAID"); return VIT_ERROR; }
  if (sw != 0x9000) {
    dbg("VITALE : application Vitale absente (SW %04X) -> pas une Carte Vitale 2", sw);
    snprintf(out, max, "CARD_READ_NOT_CV:%04X", sw);
    return VIT_NOT_CV;
  }
  dbg("VITALE : application Vitale selectionnee");

  size_t len = 0;
  if (selectFid(0x2001) != 0x9000) { diagStep("SEL2001"); return VIT_ERROR; }
  if (readBin(g_vitBuf, sizeof(g_vitBuf), &len) != 0x9000) { diagStep("RD2001"); return VIT_ERROR; }
  dbg("VITALE : fichier de pointeurs 2001 lu (%u o)", (unsigned)len);
  if (!vitParsePointers(g_vitBuf, len)) { diagStep("PTR"); return VIT_ERROR; }

  g_nBenef = 0;
  int nRead = 0;
  for (int i = 0; i < g_nVitApps && nRead < VIT_MAX_FILES; ++i) {
    const VitApp& app = g_vitApps[i];
    sw = selectAid(app.aid, app.aidLen);
    if (sw == 0) { diagStep("SELAPP"); return VIT_ERROR; }
    if (sw != 0x9000) continue;
    for (int j = 0; j < g_nVitFiles && nRead < VIT_MAX_FILES; ++j) {
      if (g_vitFiles[j].key != app.key) continue;
      sw = selectFid(g_vitFiles[j].fid);
      if (sw == 0) { diagStep("SELEF"); return VIT_ERROR; }
      if (sw != 0x9000) continue;
      ++nRead;
      sw = readBin(g_vitBuf, sizeof(g_vitBuf), &len);
      if (sw == 0) { diagStep("READ"); return VIT_ERROR; }
      dbg("VITALE : fichier %04X lu : %u o (SW %04X)", g_vitFiles[j].fid, (unsigned)len, sw);
      if (sw != 0x9000 || len == 0) continue;
      if (g_vitBuf[0] == 0x00 || (len >= 2 && g_vitBuf[0] == 0x04 && g_vitBuf[1] == 0x00)) continue;
      tlvScan(g_vitBuf, len, 0);
    }
  }
  if (g_nBenef == 0) { diagStep("NO65"); return VIT_ERROR; }

  size_t k = 0;
  appendf(out, max, &k, "%s%s", "CARD_READ:", "");
  for (int i = 0; i < g_nBenef; ++i) {
    const Benef& b = g_benef[i];
    if (i) appendf(out, max, &k, "%s%s", "|", "");
    bool first = true;
    const char* keys[] = { "NOM", "PRENOM", "NAISS", "NIR", "NAT" };
    const char* vals[] = { b.nom, b.prenom, b.naiss, b.nir, b.nat };
    for (int f = 0; f < 5; ++f) {
      if (!vals[f][0]) continue;
      appendf(out, max, &k, first ? "%s=%s" : ";%s=%s", keys[f], vals[f]);
      first = false;
    }
  }
  return VIT_OK;
}
