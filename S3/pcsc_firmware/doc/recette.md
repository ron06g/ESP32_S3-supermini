# Recette

## Procédure

Outil : `tools/pcsc_term.py` (voir [protocole.md](protocole.md)). Port de la N8R2 :
COM12 sur le PC de développement.

1. **Sans lecteur** :
   - `RESTART` → `CARD READER v1.0`, puis `READER_KO` 2,5 s plus tard ;
   - `IS_READER` → `READER_KO`, `IS_CARD` → `CARD_OUT`, `READ_CARD` → `NO_CARD` ;
   - `FOO` → `UNKNOWN_CMD` ;
   - LED : rouge fixe, puis bleu rapide environ 3 s toutes les 30 s (recherche).
2. **Lecteur branché**, VBUS vérifié, voir [materiel.md](materiel.md) :
   - `READER_OK`, LED verte qui respire ;
   - `INFO` donne le VID:PID et le niveau.
3. **Carte bancaire** → `CARD_IN`, puis `CARD_READ_NOT_CV:6A82` (ou un autre SW),
   LED violette.
4. **Carte Vitale 2** → `CARD_IN`, puis `CARD_READ:NOM=…`, LED verte fixe.
   - `CARD_STATE` renvoie le cache ; `READ_CARD` relit.
5. **Retrait** → `CARD_OUT`.
6. **Lecteur débranché** → `READER_KO`, recherche toutes les 30 s ; rebranché →
   `READER_OK`.
7. **`RESTART` avec une carte insérée** → bannière, `READER_OK`, `CARD_IN`, résultat.
8. **Démarrage avec le lecteur déjà branché** → `READER_OK` sans intervention.

En cas d'échec, regarder `INFO` (`STEP`, `SW`, `LEVEL`, `ATR`, `LASTDEV`), puis
passer en mode `DEBUG`.

**Contrôle croisé possible** : même lecteur et même carte sur le PC (pyscard), avec
les APDU de [architecture.md](architecture.md).

## Résultats du 2026-10-14

Matériel :
- ESP32-S3-WROOM-1 N8R2 sur COM12 (CH343) ;
- firmware du jour (`pcsc_firmware_v2610.14`), avec mode DEBUG.

Journaux complets : [journaux/](journaux/).

| # | Essai | Résultat |
|---|---|---|
| 1 | Protocole série : bannière, `INFO`, `IS_READER`, `IS_CARD` | OK |
| 2 | Premier essai de l'utilisateur : « aucun lecteur détecté » | `INFO … LASTDEV=NONE`. En mode DEBUG, le port reste **électriquement vide** pendant 5 min (`connecte=0 D+=0 D-=0`, 0 appareil). Ce n'est **pas** un défaut logiciel : rien d'alimenté n'était branché (cause côté utilisateur non identifiée à ce jour). Journaux 1 et 2 |
| 3 | Branchement du **Gemalto GemPC Twin** (08E6:3437) | D+ monte (`connecte=1 vitesse=FS`), descripteur lu, `NEW_DEV`, interface CCID trouvée, `LECTEUR PRET` puis `READER_OK`, **le tout en moins de 0,3 s**. Journal 2 |
| 4 | Carte de l'utilisateur : **Vitale 1**, ATR `3F 65 25 00 A4 09 6A 90 00` | muette en 3 V (`ICC_MUTE`), répond en 5 V (0,41 s) → `CARD_READ_NOT_CV:V1` (lecture complète en 0,49 s). Journal 3 |
| 5 | **Carte bancaire** (famille Revolut / Visa), ATR `3B FF 13 00 00 10 00 31 01 F1 56 40 11 00 19 00 …` | **Avant correctif** : `SELECT` → `XFR_PARITY_ERROR` → `CARD_READ_ERROR`. **Après correctif** : `SetParameters Fi/Di=0x13` → `SELECT` → `6A82` → `CARD_READ_NOT_CV:6A82` (0,08 s). Journaux 3 et 4 |
| 6 | Débranchement puis rebranchement du lecteur | `DEV_GONE` → `READER_KO` → recherche toutes les 30 s (coupure du port `off=ESP_OK on=ESP_OK`) → rebranché : `NEW_DEV` → `READER_OK`. Journal 3 |
| 7 | Démarrage (après flash) avec le lecteur déjà branché | `READER_OK` sans intervention. Journal 4 |
| 8 | **Identiv CLOUD 2700 R** (04E6:5810), branché par l'utilisateur à la place du Gemalto | détecté (`READER_OK`), `INFO` : `LEVEL=TPDU`, `FEAT=000100BA`. **Carte bancaire** → `CARD_READ_NOT_CV:6A82` en 75 ms (mode spécifique, `SetParameters` d'après TA1) |
| 9 | Terminal graphique `pcsc_term` sur COM12 | connexion, synchronisation (`IS_READER`, `IS_CARD`, `CARD_STATE`, `INFO`), panneau « Prêt », onglet Lecteur USB : OK. Exe autonome : démarre, interface identique |

Mesures complémentaires :
- **PHY** : `usb_wrap.otg_conf=0x001D5000` (pad_en=1, phy_sel=0 : PHY interne) et
  `rtc.usb_conf=0x00180000` (sw_hw_sel=1, sw_sel=1 : PHY sur l'OTG). C'est conforme.
- **Glitch** : de brefs `D+=0 D-=1` apparaissent à l'insertion ou au retrait d'une
  carte. C'est un état transitoire des lignes pendant l'activité du lecteur, sans
  conséquence.

### Défaut trouvé et corrigé

**Carte en mode spécifique sur lecteur TPDU** (essai 5). L'ATR de la carte
bancaire contient TA2 = `00` : c'est le *mode spécifique*. La carte communique
d'emblée avec le Fi/Di de TA1 (`0x13`) et n'accepte pas de PPS. Le firmware
imposait au lecteur la vitesse par défaut (`0x11`) par `SetParameters`, d'où les
erreurs de parité.

Correctif (`vitale.h`) : en mode spécifique, `SetParameters` reprend le Fi/Di de TA1.
L'analyse de l'ATR lit maintenant aussi TA2 et TC2.

### Lecteurs

| Lecteur | VID:PID | Niveau | `dwFeatures` | Message max | État |
|---|---|---|---|---|---|
| Gemalto (Gemplus) GemPC Twin « USB SmartCard Reader » | 08E6:3437 | TPDU | 0x00010230 | 271 o | **testé, OK** |
| Identiv CLOUD 2700 R | 04E6:5810 | **TPDU** (relevé) | **0x000100BA** (relevé) | 271 o | **testé, OK**. La base libccid indique APDU étendu 0x000404BA : autre révision de firmware. Le firmware du module lit le niveau réel dans le descripteur |

Gemalto, valeurs relevées en mode DEBUG :

| Élément | Valeur |
|---|---|
| Descripteur de configuration | 93 octets |
| Interface | n° 0, classe 0B |
| Endpoints | `01` bulk OUT (MPS 64), `82` bulk IN (MPS 64), `83` interruption IN (MPS 8) |
| CCID | version 1.01, tensions 0x07 (5 / 3 / 1,8 V), protocoles T=0 et T=1, horloge 4,8 MHz |
| Identité USB | fabricant « Gemalto », n° de série `E66B5793` |

Sur le PC, les deux lecteurs se présentent en « Microsoft Usbccid (WUDF) », classe
`0B`, non composites.

### Reste à valider

- [ ] **Lecture d'une Carte Vitale 2** (avec photo) : c'est le but de la preuve de
      faisabilité. La carte de l'utilisateur est une Vitale 1.
- [x] Lecteur Identiv CLOUD 2700 R : détecté, carte bancaire refusée proprement (TPDU, tension automatique).
- [ ] `RESTART` avec une carte insérée.
- [ ] Durée d'une lecture complète de Vitale 2, à vitesse par défaut et sans PPS.
- [ ] Hub USB entre l'ESP32 et le lecteur.

## Règles pour les journaux

- Le mode DEBUG affiche les APDU en clair. Un journal de **Carte Vitale 2 lue**
  contient nom, prénom, date de naissance et NIR : il ne doit **pas** être versionné
  tel quel. Il faut d'abord masquer les lignes `APDU<` / `CCID<` des fichiers lus et
  la trame `CARD_READ:`.
- Les journaux de [journaux/](journaux/) n'en contiennent aucune : Vitale 1 non lue,
  carte bancaire refusée au premier `SELECT`.
