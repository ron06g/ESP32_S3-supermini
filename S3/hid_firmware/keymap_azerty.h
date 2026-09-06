// ===========================================================================
//  keymap_azerty.h  —  Table de disposition AZERTY (FR) — PIECE CRITIQUE
// ---------------------------------------------------------------------------
//  Un clavier USB envoie des CODES DE POSITION (usages HID page 0x07), pas des
//  caracteres. Le caractere obtenu depend de la disposition configuree SUR LA
//  CIBLE. Ce fichier suppose la cible en *francais AZERTY* (Windows/Linux FR).
//
//  Role : traduire un point de code Unicode -> 1 ou 2 "frappes" (touche HID +
//  modificateurs a maintenir). Les accents circonflexe/trema passent par des
//  "touches mortes" (2 frappes : la touche morte, puis la lettre de base).
//
//  Reference : socle §7 (piege AZERTY), spec S3 §6.
// ===========================================================================
#pragma once
#include <stdint.h>

// --- Bits de modificateurs, tels qu'attendus dans l'octet 0 du rapport HID ---
#define KM_NONE   0x00
#define KM_CTRL   0x01   // Ctrl gauche
#define KM_SHIFT  0x02   // Shift gauche
#define KM_ALT    0x04   // Alt gauche
#define KM_GUI    0x08   // GUI / Win / Cmd gauche
#define KM_ALTGR  0x40   // Alt droit (AltGr)

// Une frappe physique : usage HID (page 0x07) + modificateurs a maintenir.
typedef struct { uint8_t key; uint8_t mod; } km_stroke_t;

// Un caractere -> 1 ou 2 frappes (2 = touche morte + lettre).
typedef struct {
  uint32_t    cp;      // point de code Unicode
  km_stroke_t s[2];
  uint8_t     n;       // nombre de frappes utilisees (1 ou 2)
} km_entry_t;

// ---------------------------------------------------------------------------
//  Position HID de chaque lettre a..z sur un clavier FR AZERTY.
//  (l'index 0 = 'a', 25 = 'z'). Ce sont des POSITIONS, pas les usages US.
// ---------------------------------------------------------------------------
static const uint8_t KM_LETTER_POS[26] = {
  /* a */ 0x14, /* b */ 0x05, /* c */ 0x06, /* d */ 0x07, /* e */ 0x08,
  /* f */ 0x09, /* g */ 0x0A, /* h */ 0x0B, /* i */ 0x0C, /* j */ 0x0D,
  /* k */ 0x0E, /* l */ 0x0F, /* m */ 0x33, /* n */ 0x11, /* o */ 0x12,
  /* p */ 0x13, /* q */ 0x04, /* r */ 0x15, /* s */ 0x16, /* t */ 0x17,
  /* u */ 0x18, /* v */ 0x19, /* w */ 0x1D, /* x */ 0x1B, /* y */ 0x1C,
  /* z */ 0x1A
};

// Position HID de chaque chiffre 0..9 (rangee haute, obtenue avec Shift en FR).
static const uint8_t KM_DIGIT_POS[10] = {
  /* 0 */ 0x27, /* 1 */ 0x1E, /* 2 */ 0x1F, /* 3 */ 0x20, /* 4 */ 0x21,
  /* 5 */ 0x22, /* 6 */ 0x23, /* 7 */ 0x24, /* 8 */ 0x25, /* 9 */ 0x26
};

// ---------------------------------------------------------------------------
//  Table des symboles, ponctuation, accents et touches mortes.
//  Les lettres a-z / A-Z et les chiffres sont traites par calcul (voir plus
//  bas), pas dans cette table.
// ---------------------------------------------------------------------------
static const km_entry_t KM_TABLE[] = {
  // --- Espace ---
  { 0x0020, {{0x2C,KM_NONE},{0,0}}, 1 },   // ' '

  // --- Rangee des chiffres : symboles directs / AltGr ---
  { 0x0026, {{0x1E,KM_NONE},{0,0}}, 1 },   // &
  { 0x00E9, {{0x1F,KM_NONE},{0,0}}, 1 },   // é
  { 0x007E, {{0x1F,KM_ALTGR},{0x2C,KM_NONE}}, 2 }, // ~  (AltGr+2 morte + espace)
  { 0x0022, {{0x20,KM_NONE},{0,0}}, 1 },   // "
  { 0x0023, {{0x20,KM_ALTGR},{0,0}}, 1 },  // #
  { 0x0027, {{0x21,KM_NONE},{0,0}}, 1 },   // '
  { 0x007B, {{0x21,KM_ALTGR},{0,0}}, 1 },  // {
  { 0x0028, {{0x22,KM_NONE},{0,0}}, 1 },   // (
  { 0x005B, {{0x22,KM_ALTGR},{0,0}}, 1 },  // [
  { 0x002D, {{0x23,KM_NONE},{0,0}}, 1 },   // -
  { 0x007C, {{0x23,KM_ALTGR},{0,0}}, 1 },  // |
  { 0x00E8, {{0x24,KM_NONE},{0,0}}, 1 },   // è
  { 0x0060, {{0x24,KM_ALTGR},{0x2C,KM_NONE}}, 2 }, // `  (AltGr+7 morte + espace)
  { 0x005F, {{0x25,KM_NONE},{0,0}}, 1 },   // _
  { 0x005C, {{0x25,KM_ALTGR},{0,0}}, 1 },  // (backslash)
  { 0x00E7, {{0x26,KM_NONE},{0,0}}, 1 },   // ç
  { 0x00E0, {{0x27,KM_NONE},{0,0}}, 1 },   // à
  { 0x0040, {{0x27,KM_ALTGR},{0,0}}, 1 },  // @
  { 0x0029, {{0x2D,KM_NONE},{0,0}}, 1 },   // )
  { 0x00B0, {{0x2D,KM_SHIFT},{0,0}}, 1 },  // °
  { 0x005D, {{0x2D,KM_ALTGR},{0,0}}, 1 },  // ]
  { 0x003D, {{0x2E,KM_NONE},{0,0}}, 1 },   // =
  { 0x002B, {{0x2E,KM_SHIFT},{0,0}}, 1 },  // +
  { 0x007D, {{0x2E,KM_ALTGR},{0,0}}, 1 },  // }

  // --- Rangee du haut : extras ---
  { 0x20AC, {{0x08,KM_ALTGR},{0,0}}, 1 },  // €  (AltGr+e)
  { 0x005E, {{0x2F,KM_NONE},{0x2C,KM_NONE}}, 2 },  // ^  (morte + espace)
  { 0x00A8, {{0x2F,KM_SHIFT},{0x2C,KM_NONE}}, 2 }, // ¨  (morte + espace)
  { 0x0024, {{0x30,KM_NONE},{0,0}}, 1 },   // $
  { 0x00A3, {{0x30,KM_SHIFT},{0,0}}, 1 },  // £
  { 0x00A4, {{0x30,KM_ALTGR},{0,0}}, 1 },  // ¤

  // --- Rangee du milieu : extras ---
  { 0x00F9, {{0x34,KM_NONE},{0,0}}, 1 },   // ù
  { 0x0025, {{0x34,KM_SHIFT},{0,0}}, 1 },  // %
  { 0x002A, {{0x31,KM_NONE},{0,0}}, 1 },   // *
  { 0x00B5, {{0x31,KM_SHIFT},{0,0}}, 1 },  // µ

  // --- Rangee du bas : ponctuation ---
  { 0x002C, {{0x10,KM_NONE},{0,0}}, 1 },   // ,
  { 0x003F, {{0x10,KM_SHIFT},{0,0}}, 1 },  // ?
  { 0x003B, {{0x36,KM_NONE},{0,0}}, 1 },   // ;
  { 0x002E, {{0x36,KM_SHIFT},{0,0}}, 1 },  // .
  { 0x003A, {{0x37,KM_NONE},{0,0}}, 1 },   // :
  { 0x002F, {{0x37,KM_SHIFT},{0,0}}, 1 },  // /
  { 0x0021, {{0x38,KM_NONE},{0,0}}, 1 },   // !
  { 0x00A7, {{0x38,KM_SHIFT},{0,0}}, 1 },  // §
  { 0x003C, {{0x64,KM_NONE},{0,0}}, 1 },   // <  (touche ISO < >)
  { 0x003E, {{0x64,KM_SHIFT},{0,0}}, 1 },  // >

  // --- Accents circonflexe (touche morte ^ = 0x2F, puis lettre) ---
  { 0x00E2, {{0x2F,KM_NONE},{0x14,KM_NONE}}, 2 },  // â
  { 0x00EA, {{0x2F,KM_NONE},{0x08,KM_NONE}}, 2 },  // ê
  { 0x00EE, {{0x2F,KM_NONE},{0x0C,KM_NONE}}, 2 },  // î
  { 0x00F4, {{0x2F,KM_NONE},{0x12,KM_NONE}}, 2 },  // ô
  { 0x00FB, {{0x2F,KM_NONE},{0x18,KM_NONE}}, 2 },  // û

  // --- Accents trema (touche morte ¨ = Shift+0x2F, puis lettre) ---
  { 0x00E4, {{0x2F,KM_SHIFT},{0x14,KM_NONE}}, 2 }, // ä
  { 0x00EB, {{0x2F,KM_SHIFT},{0x08,KM_NONE}}, 2 }, // ë
  { 0x00EF, {{0x2F,KM_SHIFT},{0x0C,KM_NONE}}, 2 }, // ï
  { 0x00F6, {{0x2F,KM_SHIFT},{0x12,KM_NONE}}, 2 }, // ö
  { 0x00FC, {{0x2F,KM_SHIFT},{0x18,KM_NONE}}, 2 }, // ü
  { 0x00FF, {{0x2F,KM_SHIFT},{0x1C,KM_NONE}}, 2 }, // ÿ
};

static const size_t KM_TABLE_LEN = sizeof(KM_TABLE) / sizeof(KM_TABLE[0]);

// ---------------------------------------------------------------------------
//  km_lookup : point de code Unicode -> frappes.
//  Retourne true si trouve, remplit out[] (jusqu'a 2) et *n.
// ---------------------------------------------------------------------------
static inline bool km_lookup(uint32_t cp, km_stroke_t out[2], uint8_t* n) {
  // Lettres minuscules a..z
  if (cp >= 'a' && cp <= 'z') {
    out[0].key = KM_LETTER_POS[cp - 'a']; out[0].mod = KM_NONE;
    *n = 1; return true;
  }
  // Lettres majuscules A..Z (position + Shift)
  if (cp >= 'A' && cp <= 'Z') {
    out[0].key = KM_LETTER_POS[cp - 'A']; out[0].mod = KM_SHIFT;
    *n = 1; return true;
  }
  // Chiffres 0..9 (rangee haute + Shift en AZERTY)
  if (cp >= '0' && cp <= '9') {
    out[0].key = KM_DIGIT_POS[cp - '0']; out[0].mod = KM_SHIFT;
    *n = 1; return true;
  }
  // Reste : table
  for (size_t i = 0; i < KM_TABLE_LEN; i++) {
    if (KM_TABLE[i].cp == cp) {
      out[0] = KM_TABLE[i].s[0];
      out[1] = KM_TABLE[i].s[1];
      *n = KM_TABLE[i].n;
      return true;
    }
  }
  return false;
}
