# Architecture du firmware

```
Lecteur CCID --USB (hôte, IDF)--> ESP32-S3 N8R2 --UART0 / CH343--> PC / automate
  (prise « USB »)                  usb_ccid / apdu_t0 / vitale         (prise « COM », STX…EOT)
```

## Fichiers

| Fichier | Rôle |
|---|---|
| `pcsc_firmware.ino` | orchestration : `setup()` (UART0, logs IDF coupés), tâche `ccid` (état du lecteur et de la carte, scrutation toutes les 250 ms, commandes, `RESTART`, recherche toutes les 30 s, surveillance DEBUG du port) |
| `com_proto.h` | port COM : tâche `com` (réception, tramage, table des commandes), `comSend()` (une trame = un `write`, sous mutex) |
| `debug_log.h` | mode DEBUG : `dbg()` / `dbgHex()` (trames `DBG:` horodatées), capture des logs ESP-IDF (`esp_log_set_vprintf`) |
| `usb_ccid.h` | USB Host IDF (tâches `usbLib` / `usbCli`, filtre d'énumération), attache / détache, descripteur CCID, échanges bulk synchrones, messages CCID, diagnostic du port (HPRT, PHY) |
| `apdu_t0.h` | APDU au-dessus de `XfrBlock` (cartes T=0) : cas 4 → cas 3, `61xx` → GET RESPONSE, `6Cxx` → Le corrigé |
| `vitale.h` | ATR (protocoles, TA1/TA2/TC1/TC2), mise sous tension, séquence Vitale 2 (AID, pointeurs EF 2001, fichiers), BER-TLV, trame résultat |
| `cartes.h` | identification des autres cartes et de leurs données publiques : CB (EMV), CPS (PKCS#15 / X.509), OpenPGP, PIV, ID (ICAO). Voir plus bas |
| `status_led.h` | LED d'état : tâche `led` (~50 Hz) ; le reste du code ne fait que `ledSet(état)` / `ledFlash()` |
| `tools/pcsc_term.py` | terminal de test (pyserial), hors firmware |

Arduino ne compile que les fichiers de la racine du sketch, donc pas `doc/`, `tools/`
ni `Ressources/`.

## Tâches : un seul propriétaire par ressource

| Tâche | Cœur | Prio | Rôle |
|---|---|---|---|
| `usbLib` | 0 | 5 | `usb_host_lib_handle_events()` : énumération, pile USB Host |
| `usbCli` | 0 | 5 | `usb_host_client_handle_events()`. Le callback client ne fait que poster `NEW_DEV` / `DEV_GONE` dans `g_usbEvtQueue` ; les callbacks de transfert (même contexte) ne font que libérer un sémaphore |
| `ccid` | 1 | 4 | **seul propriétaire du lecteur** : événements USB, puis commandes, scrutation, lecture, recherche, journal du port |
| `com` | 1 | 3 | lecture de `Serial0`, découpage des trames, postage dans `g_cmdQueue` (bloquant si pleine : aucune commande perdue) |
| `led` | 0 | 1 | animation de la LED |

Règles :
- **Deux files distinctes**, `g_usbEvtQueue` (USB) et `g_cmdQueue` (COM) : un flot de
  commandes ne peut pas évincer un débranchement du lecteur. La tâche `ccid` traite
  les événements USB **avant** chaque commande.
- **Aucun appel bloquant** dans un callback USB.
- **Ordre d'initialisation** : `usb_host_install` → `usb_host_client_register` →
  démarrage des tâches. Aucun `NEW_DEV` n'est perdu si le lecteur est déjà branché.
  Filet de sécurité : `usb_host_device_addr_list_fill()` en fin de délai de grâce ou
  de recherche.

## USB Host et CCID (`usb_ccid.h`)

### Attache d'un appareil

1. `usb_host_device_open`, puis lecture du descripteur de configuration actif.
2. Choix de l'interface :
   - candidate : classe **0x0B** (CCID), ou 0xFF portant un descripteur de classe `0x21` de 54 octets ;
   - le descripteur `0x21` est cherché n'importe où avant l'interface suivante ;
   - il faut aussi un bulk IN et un bulk OUT.
3. Champs lus dans le descripteur CCID :
   - octet 5 : `bVoltageSupport` ;
   - octet 6 : `dwProtocols` ;
   - octet 40 : `dwFeatures`. Niveau d'échange par ses bits 16 à 18 : TPDU 0x10000, APDU 0x20000, APDU étendu 0x40000. Un lecteur de niveau « caractère » est refusé ;
   - octet 44 : `dwMaxCCIDMessageLength`.
4. `usb_host_interface_claim`, puis allocation des transferts : taille au moins
   `dwMaxCCIDMessageLength` (512 o au minimum), plafonnée à 2 Ko, arrondie au MPS
   pour l'IN.
5. Un appareil non CCID est refermé aussitôt et noté dans `INFO LASTDEV`.

### Détache

1. `usb_host_interface_release`, avec nouvelles tentatives : l'IDF ne décompte un
   transfert terminé qu'après le retour de son callback.
2. `usb_host_device_close`.
3. `usb_host_transfer_free`.

### Échange synchrone `ccidXfer()`

- **Délai** : sémaphore par transfert, purgé avant chaque soumission. Le champ
  `timeout_ms` des transferts n'est **pas** pris en charge par l'IDF.
- **Délai dépassé** : halt → flush → attente du callback `CANCELED` → clear. Un
  transfert n'est jamais laissé en vol. S'il ne revient jamais : `g_usbStuck`
  (`READER_KO`, LED de panne, et `RESTART` redémarre l'ESP32).
- **Réception** : un IN d'**exactement un MPS** d'abord, puis le reste arrondi au MPS.
  On n'attend jamais un paquet de longueur nulle (ZLP) que le lecteur n'enverrait pas.
  - complétions de 0 octet ignorées ;
  - message trop long : lu jusqu'au bout, puis jeté.
- **Contrôle de la réponse** :
  - `bSeq` vérifié : une réponse périmée est jetée ;
  - `bmCommandStatus = 2` (prolongation de délai) : on relit, et le délai repart.
- **STALL** : remise à zéro côté hôte, puis `CLEAR_FEATURE(ENDPOINT_HALT)`.

### Messages CCID utilisés (en-tête de 10 octets, slot 0)

| Message | Code | Octets 7-9 | Usage |
|---|---|---|---|
| `PC_to_RDR_IccPowerOn` | 0x62 | `bPowerSelect` (0 auto, 1 5 V, 2 3 V, 3 1,8 V) | mise sous tension → ATR |
| `PC_to_RDR_IccPowerOff` | 0x63 | — | coupure (après chaque lecture) |
| `PC_to_RDR_GetSlotStatus` | 0x65 | — | scrutation toutes les 250 ms |
| `PC_to_RDR_SetParameters` | 0x61 | `bProtocolNum` = 0 (T=0) | 5 octets : Fi/Di, `bmTCCKST0` (0x02 = convention inverse), garde (TC1), WI (TC2), clock stop |
| `PC_to_RDR_XfrBlock` | 0x6F | `bBWI` = 0, `wLevelParameter` = 0 | APDU courte / TPDU |
| `RDR_to_PC_DataBlock` | 0x80 | octet 7 `bStatus`, octet 8 `bError` | ATR, réponse APDU |
| `RDR_to_PC_SlotStatus` | 0x81 | idem | `bmICCStatus` = `bStatus & 3` (0 carte active, 1 présente inactive, 2 absente) |
| `RDR_to_PC_Parameters` | 0x82 | idem | écho des paramètres |

`bStatus >> 6` donne `bmCommandStatus` : 0 OK, 1 échec, 2 prolongation de délai.

Codes `bError` rencontrés en recette :

| Code | Nom | Cas |
|---|---|---|
| 0xFE | `ICC_MUTE` | Vitale 1 alimentée en 3 V |
| 0xFD | `XFR_PARITY_ERROR` | carte en mode spécifique laissée à la vitesse par défaut |

### Carte : tension et paramètres

- **Tension** :
  - automatique (0) si `dwFeatures & 0x08`, puis 5 V en secours ;
  - sinon 3 V puis 5 V, selon `bVoltageSupport`, la plus basse d'abord comme le
    veut l'ISO 7816-3, avec `IccPowerOff` entre deux essais.
- **SetParameters**, seulement si le lecteur est TPDU et sans négociation
  automatique (`dwFeatures & 0x40`) :
  - **mode spécifique** (TA2 présent dans l'ATR, bit 0x10 à 0) : Fi/Di = **TA1**.
    La carte parle d'emblée à cette vitesse : la laisser à la vitesse par défaut
    donne des erreurs de parité (vu en recette) ;
  - **mode négociable** : Fi/Di 0x11 (vitesse par défaut), sans PPS ;
  - garde = TC1, WI = TC2 (10 par défaut), convention donnée par TS (`3F` = inverse).
- **La carte est coupée après chaque lecture** : l'état de repos est « présente
  inactive ». `READ_CARD` la rallume.

### `RESTART` et recherche périodique

- `usbPortCycle()` : `usb_host_lib_set_root_port_power(false)`, 500 ms, puis `(true)`.
  C'est une coupure **logique** (VBUS est câblé en dur sur la carte) qui force la
  ré-énumération ; dans l'IDF 5.5 : `hub_root_stop` / `hub_root_start`.
- `RESTART` : `IccPowerOff`, détache, `usbPortCycle()`, remise à zéro, bannière,
  puis délai de grâce de 2,5 s.
- **Recherche** : sans lecteur (fin de délai de grâce, débranchement, recherche
  infructueuse), elle est programmée **30 s** plus tard. Elle fait un
  `usbPortCycle()` puis attend 2,5 s l'énumération (LED bleue rapide), avec repli
  sur `usb_host_device_addr_list_fill()`.

## APDU (`apdu_t0.h`)

- Cas 1 (4 octets) : `P3 = 00` ajouté (forme TPDU).
- Cas 4 : envoyé en cas 3, Le retiré (règle T=0).
- `61xx` → `00 C0 00 00 xx` (GET RESPONSE), en boucle, données concaténées.
- `6Cxx` → la commande (forme 5 octets) est renvoyée avec `P3 = xx`.
- Un seul chemin pour les lecteurs TPDU et APDU.

## Séquence Vitale 2 (`vitale.h`)

Reprise du script open source Cardpeek `vitale_2.lua`, avec CLA `00` (voir
[references.md](references.md)).

1. Mise sous tension et analyse de l'ATR :
   - ATR `3F 65 25 …` → `NOT_CV:V1` ;
   - carte T=1 seule sur un lecteur TPDU → `NOT_CV:T1`.
2. `SELECT` de l'AID `D2 50 00 00 02 56 49 54 41 4C 45` (« VITALE ») :
   `00 A4 04 00 0B …`. Si le SW n'est pas `9000`, ce n'est pas une Vitale 2.
   `cardIdentify()` (`cartes.h`, section suivante) cherche alors un autre type de
   carte. Rien de reconnu → `NOT_CV:<SW du SELECT Vitale>`.
3. `SELECT` de l'EF `2001` (`00 A4 02 00 02 20 01`), puis `READ BINARY` `00 B0 hi lo 00`
   par blocs de 256 octets :
   - arrêt sur un bloc court, `6282`, `6B00` ou un autre SW ;
   - 4 Ko au plus par fichier.
4. Fichier de pointeurs `E0{ A0{ 82… } 80… }` :
   - entrées `82` : clé = octets 0-1, AID = octets 4 à la fin ;
   - enregistrements `80` : nom = octets 0-7, clé = octets 8-9, FID = octets 10-11.
5. Pour chaque application : `SELECT` de l'AID, puis `SELECT` et lecture de chacun de
   ses fichiers (16 au plus). Les contenus qui commencent par `00` ou `04 00` sont bruts
   et ignorés.
6. BER-TLV : chaque gabarit `65` est un bénéficiaire.

   | Tag | Champ | Clé dans `CARD_READ` |
   |---|---|---|
   | `80` | nom | `NOM` |
   | `81` | prénom | `PRENOM` |
   | `82` | date de naissance (AAAAMMJJ) | `NAISS` |
   | `90` | nationalité | `NAT` |
   | `93` | NIR | `NIR` |

   Au plus 6 bénéficiaires, 40 caractères par champ.

## Autres cartes (`cartes.h`)

`cardIdentify()` est appelée quand l'application Vitale est absente. Elle essaie
dans cet ordre, en lecture seule et sans code PIN :

| Ordre | Type | Reconnaissance | Données publiques lues |
|---|---|---|---|
| 1 | `CPS` | ATR des cartes de l'ANS : octets 5 à 10 = `00 12 25 00 64 80` (comme OpenSC, pilote IAS/ECC « CPx ») | PKCS#15 : `SELECT` MF → EF.DIR (`2F00`, lecture binaire ou par enregistrements) → AID ou chemin de l'application → ODF (`5031`) → CDF (`A4`, sinon `A5` / `A6`) → chemins des certificats (`[1]` → `04`) → certificat → **sujet X.509** |
| 2 | `CB` | annuaire de paiement EMV `1PAY.SYS.DDF01` : SFI (tag `88`), `READ RECORD` → applications (`61` { `4F` AID, `50` libellé }). À défaut, `SELECT` des AID connus | application internationale si elle existe (sinon CB) : `SELECT` (FCI : `9F12` / `50` libellé, `9F38` PDOL) → `GET PROCESSING OPTIONS` → AFL (`80` ou `77` / `94`) → `READ RECORD` → `9F0B` / `5F20` titulaire, `5A` / `57` numéro **masqué**. Sans AFL : SFI 1 à 4, enregistrements 1 à 4 |
| 3 | `OPENPGP` | AID `D2 76 00 01 24 01` | `GET DATA 4F` (AID complet : fabricant, numéro de série), `GET DATA 65` → `5B` nom « Nom<<Prénom » (ISO 8859-1) |
| 4 | `PIV` | AID `A0 00 00 03 08 00 00 10 00 01 00` | `GET DATA 5FC102` (CHUID → `34` GUID), certificat `5FC105` (sinon `5FC101`) → `70` → sujet X.509. Un certificat compressé (`71` bit 0) est ignoré |
| 5 | `ID` | AID ICAO eMRTD `A0 00 00 02 47 10 01` | aucune : BAC / PACE nécessaire |

Applications EMV reconnues :

| Réseau | AID |
|---|---|
| CB | `A0 00 00 00 42 10 10` |
| Visa | `A0 00 00 00 03 10 10` |
| Visa Electron | `…03 20 10` |
| V PAY | `…03 20 20` |
| Mastercard | `A0 00 00 00 04 10 10` |
| Maestro | `…04 30 60` |
| American Express | `A0 00 00 00 25 01` |
| Discover | `A0 00 00 01 52 30 10` |
| JCB | `A0 00 00 00 65 10 10` |
| UnionPay | `A0 00 00 03 33 01 01 01` |

Au-delà de cette liste, une application est rattachée à son réseau par ses 5
premiers octets (RID).

- **PDOL** (données demandées par la carte pour `GET PROCESSING OPTIONS`) : tout à
  zéro, sauf :

  | Tag | Valeur |
  |---|---|
  | `9F1A` (pays) | France, `0250` |
  | `5F2A` (devise) | euro, `0978` |
  | `9F35` (type de terminal) | `22` |
  | `9F37` (nombre imprévisible) | aléatoire |

  Il n'y a **pas** de `GENERATE AC` : aucune transaction n'est faite.
- **Sujet X.509** : chaque `SET { SEQUENCE { OID, valeur } }` du sujet est lu.

  | OID | Attribut | Champ |
  |---|---|---|
  | 2.5.4.3 | CN | `TITULAIRE` |
  | 2.5.4.4 | SN | `NOM` |
  | 2.5.4.42 | GN | `PRENOM` |
  | 2.5.4.10 | O | `ETAB` |
  | 2.5.4.5 | serialNumber | `IDENT` |
  | 2.5.4.12 | title | `PROFESSION` |

  Chaînes acceptées : UTF8String, PrintableString, IA5String, T61String, BMPString.
- **Texte** : accents repliés en ASCII (é → e…), et `;` `=` `|` remplacés par une
  espace (`textCopy`).
- **Confidentialité** :
  - le numéro de carte bancaire n'est conservé que masqué ;
  - les tampons des enregistrements EMV sont effacés après lecture ;
  - `g_dbgMask` masque les réponses (`APDU<` / `CCID<`) dans le journal DEBUG
    pendant `GET PROCESSING OPTIONS` et `READ RECORD`.
- **Tampons** : un certificat ou un CHUID (jusqu'à 4 Ko) est lu dans `g_vitBuf`,
  et les réponses chaînées (`61xx`) sont concaténées par `apduTransmit`.
