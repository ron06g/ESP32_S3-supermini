# pcsc_firmware — lecteur de Carte Vitale sur ESP32-S3 N8R2

Firmware **indépendant** du firmware HID (`S3/hid_firmware`), dédié à la carte
**ESP32-S3-WROOM-1 N8R2** à deux prises USB-C :

| Prise | Rôle |
|---|---|
| **USB** (USB natif, GPIO19/20) | **USB Host** : on y branche un lecteur de carte à puce **CCID** (« PC/SC », classe USB 0x0B) via un adaptateur OTG |
| **COM** (pont CH343 sur UART0) | port COM « debug » : protocole texte `<STX>…<CR><LF><EOT>` à **115200 8N1**, et flash du firmware |

Le module lit les **données publiques d'une Carte Vitale 2** (nom, prénom, date
de naissance, NIR) et les annonce sur le port COM. C'est une **preuve de
faisabilité** : le transfert Base64 des données brutes viendra ensuite.

Sobriété :
- BLE et Wi-Fi ne sont **jamais initialisés** ;
- le CPU tourne à **80 MHz** (pas moins : la PLL est nécessaire à l'USB) ;
- la carte à puce est **coupée après chaque lecture**.

Une **LED RGB** (WS2812, GPIO48) affiche l'état du lecteur et de la carte (voir plus bas).

## Matériel — à vérifier AVANT tout test

1. **Alimentation du lecteur (VBUS).** Sur ces cartes DevKit, la prise USB native
   n'alimente généralement **pas** le périphérique branché : une diode sur VBUS
   bloque le 5 V vers l'extérieur. Trois solutions :
   - ponter le cavalier à souder **« USB-OTG »** (parfois « IN-OUT ») au dos de
     la carte, s'il existe ;
   - utiliser un câble OTG en Y avec une alimentation externe ;
   - utiliser un hub USB alimenté.

   **Contrôle au multimètre** : il faut mesurer 5 V entre VBUS et GND sur la
   prise A femelle de l'adaptateur OTG. Avec le cavalier ponté, ne jamais
   brancher la prise « USB » sur un PC (deux sources 5 V).
2. **Adaptateur OTG** : USB-C mâle vers USB-A femelle, sur la prise **« USB »**.
3. **Flash et dialogue** : uniquement par la prise **« COM »**. La prise « USB »
   est devenue hôte.
4. **DTR/RTS** : sur la prise COM, ces lignes pilotent EN/BOOT (auto-reset).
   L'application hôte doit ouvrir le port **sans les basculer**. `tools/pcsc_term.py`
   les force à inactives.
5. **Log ROM** : à chaque reset matériel (RESET, mise sous tension), la ROM de
   l'ESP32 écrit une dizaine de lignes sur UART0, avant la bannière. L'hôte doit
   **ignorer tout ce qui n'est pas encadré STX…EOT**. La commande `RESTART`, elle,
   ne redémarre pas l'ESP32 et ne produit donc pas ce log.

## Compilation et flash

```bat
S3\pcsc_firmware_compile.bat          REM -> S3\firmware\pcsc_firmware_vYYMM.dd\
S3\pcsc_firmware_flash.bat [COMx]     REM dernier firmware, port auto-détecté
```

- **FQBN** : `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,CPUFreq=80,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled,DebugLevel=none`
- `USBMode=default` + `CDCOnBoot=default` : le sketch n'appelle **jamais**
  `USB.begin()`. Le contrôleur USB-OTG reste donc libre pour `usb_host_install()`.
- **8 Mo de flash** : ne pas flasher ce firmware sur une SuperMini 4 Mo, elle
  bouclerait au démarrage.
- Le script de flash ne détecte que les **ponts USB-série** (VID 1A86 WCH/CH34x,
  10C4 CP210x, 0403 FTDI). Il écarte les ports Espressif (303A), ceux des
  modules S3-KBD, pour ne pas les écraser par erreur.
- Prérequis : cœur **esp32 3.3.x** (ESP-IDF 5.5, pile USB Host incluse). Aucune
  bibliothèque externe.

## Protocole COM

### Trame

```
STX (0x02)  TEXTE  CR (0x0D)  LF (0x0A)  EOT (0x04)
```

- **Émission** : une trame est écrite d'un seul bloc. Le texte ne contient que
  des caractères 0x20..0x7E ; tout autre octet devient `?`.
- **Réception** : `STX` ouvre une trame (un reste partiel est jeté). `CR`, `LF`,
  `EOT` ou `ETX` la ferment. Une ligne nue terminée par Entrée est aussi
  acceptée (test depuis un terminal). Les espaces sont rognés, la casse est
  indifférente, et une commande fait au plus 64 caractères.

### Événements (spontanés)

| Moment | Trame(s) |
|---|---|
| Démarrage, `RESTART` | `CARD READER v1.0` |
| Lecteur énuméré | `READER_OK` (dès l'énumération) |
| Pas de lecteur 2,5 s après démarrage / `RESTART` | `READER_KO` |
| Lecteur branché / débranché | `READER_OK` / `READER_KO` (débranché avec une carte : `CARD_OUT` d'abord) |
| Carte insérée | `CARD_IN`, puis lecture automatique → une trame de résultat |
| Carte retirée | `CARD_OUT` (le résultat en cache est effacé) |

### Commandes

| Commande | Réponse |
|---|---|
| `RESTART` | réinitialise le **lecteur USB** (pas l'ESP32), puis la séquence de démarrage : `CARD READER v1.0`, `READER_OK`/`READER_KO`, `CARD_IN` + résultat si une carte est présente |
| `IS_READER` | `READER_OK` / `READER_KO` |
| `IS_CARD` | `CARD_IN` / `CARD_OUT` (`CARD_OUT` sans lecteur) |
| `READ_CARD` | **relecture forcée** (coupure puis remise sous tension de la carte) → `NO_CARD` / `CARD_READ_ERROR` / `CARD_READ_NOT_CV:<CODE>` / `CARD_READ:…` |
| `CARD_STATE` | **dernier résultat en cache**, sans relire (mêmes réponses) |
| `INFO` | diagnostic (hors spécification initiale), voir plus bas |
| autre | `UNKNOWN_CMD` |

Toutes les commandes sont traitées par la tâche qui pilote le lecteur. Les
réponses restent donc dans l'ordre des événements. Une commande reçue pendant
une lecture est traitée juste après celle-ci, qui dure quelques secondes.

### Résultats de lecture

| Trame | Sens |
|---|---|
| `CARD_READ:NOM=…;PRENOM=…;NAISS=AAAAMMJJ;NIR=…` | Carte Vitale 2 lue. Un groupe par bénéficiaire, séparés par `\|` ; `;NAT=…` en plus si présent. Format **provisoire** |
| `CARD_READ_NOT_CV:V1` | Carte Vitale 1 (ATR `3F 65 25 …`) : reconnue, **non lue** en v1.0 (format Bull B0' propriétaire) |
| `CARD_READ_NOT_CV:T1` | carte uniquement T=1 sur un lecteur de niveau TPDU (une Vitale est en T=0) |
| `CARD_READ_NOT_CV:<SW>` | carte qui refuse le SELECT de l'application Vitale, p. ex. `6A82` (carte bancaire…) |
| `CARD_READ_ERROR` | carte muette, échange en échec, carte retirée pendant la lecture, ou application Vitale sans bénéficiaire lisible |
| `NO_CARD` | (réponse à `READ_CARD` / `CARD_STATE`) aucune carte |

Les valeurs ne contiennent jamais `;`, `=` ou `|` : ils sont remplacés par `?`.

### `INFO` (diagnostic)

Avec un lecteur :

```
INFO:READER=VVVV:PPPP;NAME=<produit>;LEVEL=TPDU|APDU|XAPDU;FEAT=<dwFeatures>;ATR=<hex>;STEP=<étape>;SW=<SW>;MS=<durée>
```

Sans lecteur :

```
INFO:READER=NONE;LASTDEV=<VID:PID(NOT_CCID|CHAR)>;STEP=<étape>
```

- `LEVEL` : niveau d'échange CCID du lecteur. Les lecteurs de niveau
  « caractère » sont refusés : `LASTDEV=…(CHAR)`.
- `SW` : dernier mot d'état de la carte.
- `MS` : durée de la dernière lecture.
- `STEP` : dernière étape en échec.

| STEP | Échec |
|---|---|
| `NONE` | aucun |
| `USBHOST` | installation de la pile USB Host |
| `STUCK` | un transfert USB n'est jamais revenu : `RESTART` redémarre alors l'ESP32 |
| `POWERON` | mise sous tension / ATR |
| `SELAID` | échange du SELECT de l'application Vitale |
| `SEL2001` / `RD2001` | sélection / lecture du fichier de pointeurs EF 2001 |
| `PTR` | fichier de pointeurs illisible |
| `SELAPP` / `SELEF` / `READ` | sélection d'une application, d'un fichier, ou lecture |
| `NO65` | aucun bénéficiaire (gabarit `65`) trouvé |

## LED d'état (WS2812, GPIO48)

100 % = `LED_MAX` (64/255, volontairement doux) ; réglable dans `status_led.h`.

| État | LED |
|---|---|
| Démarrage, `RESTART` (délai de grâce) | **bleu**, respiration lente |
| Lecteur détecté, pas de carte (et après `CARD_OUT`) | **vert**, respiration lente |
| Aucun lecteur | **rouge** fixe 50 % |
| Périphérique USB branché mais pas un lecteur CCID (ou lecteur de niveau caractère) | **orange** fixe 50 % |
| `CARD_IN` / carte lue avec succès | **vert** fixe 100 % |
| Lecture en cours | **vert** 50 %, clignotement rapide (5 Hz) |
| `CARD_READ_ERROR` | **rouge** fixe 50 % |
| `CARD_READ_NOT_CV` | **violet** fixe 50 % (rouge + bleu) |
| Pile USB en panne (`STEP=USBHOST` / `STUCK`) | **rouge** 100 %, clignotement rapide (verrouillé jusqu'au redémarrage) |

En plus :
- un **éclair blanc** bref à chaque commande reçue sur le port COM ;
- au démarrage, un **auto-test** rouge, vert, bleu (prouve les trois canaux).

## Test (outil `tools/pcsc_term.py`, pyserial)

```bat
python S3\pcsc_firmware\tools\pcsc_term.py COMx                      REM interactif
python S3\pcsc_firmware\tools\pcsc_term.py COMx IS_READER IS_CARD INFO
python S3\pcsc_firmware\tools\pcsc_term.py COMx --wait 8 READ_CARD
```

1. **Sans lecteur** :
   - `RESTART` → `CARD READER v1.0`, puis `READER_KO` 2,5 s plus tard ;
   - `IS_READER` → `READER_KO`, `IS_CARD` → `CARD_OUT`, `READ_CARD` → `NO_CARD` ;
   - `FOO` → `UNKNOWN_CMD`.
2. **Lecteur branché** (VBUS vérifié) : `READER_OK`. `INFO` donne le VID:PID et le niveau.
3. **Carte bancaire** → `CARD_IN` puis `CARD_READ_NOT_CV:6A82` (ou un autre SW).
4. **Carte Vitale 2** → `CARD_IN` puis `CARD_READ:NOM=…`.
   - `CARD_STATE` renvoie le cache ; `READ_CARD` relit.
5. **Retrait** → `CARD_OUT`. Lecteur débranché → `READER_KO`, rebranché → `READER_OK`.

En cas d'échec, `INFO` (`STEP`, `SW`, `LEVEL`, `ATR`) oriente le diagnostic.

**Contrôle croisé** : le même lecteur et la même carte sur un PC, avec pyscard et
les mêmes APDU (voir `vitale.h`).

## Architecture

| Fichier | Rôle |
|---|---|
| `pcsc_firmware.ino` | orchestration : setup (UART0, logs IDF coupés), tâche `ccid` (état lecteur/carte, scrutation 250 ms, commandes, `RESTART`) |
| `status_led.h` | LED d'état : tâche `led` (~50 Hz) ; le reste du code ne fait que `ledSet(état)` / `ledFlash()` |
| `com_proto.h` | port COM : tâche `com` (réception, tramage), `comSend()` (une trame = un `write`, sous mutex) |
| `usb_ccid.h` | USB Host IDF (tâches `usbLib`/`usbCli`), attache/détache, descripteur CCID, échanges bulk synchrones, messages CCID |
| `apdu_t0.h` | APDU sur `XfrBlock` (T=0) : cas 4 → cas 3, `61xx` → GET RESPONSE, `6Cxx` → Le corrigé |
| `vitale.h` | ATR, séquence Vitale 2 (AID, pointeurs EF 2001, fichiers), BER-TLV, trame résultat |

Règles de fonctionnement :

- **Un seul propriétaire par ressource** :
  - le lecteur n'est piloté que par la tâche `ccid` ;
  - les callbacks USB ne font que poster dans sa file ou libérer un sémaphore.
- **Deux files distinctes** : `g_usbEvtQueue` (événements USB) et `g_cmdQueue`
  (commandes COM). Un hôte trop bavard ne peut pas évincer un débranchement du
  lecteur. File de commandes pleine : la tâche `com` attend, rien n'est perdu.
- **Transferts synchrones** :
  - délai géré par sémaphore (le `timeout_ms` des transferts n'est pas pris en charge par l'IDF) ;
  - annulation par halt → flush → clear ;
  - un transfert n'est jamais laissé en vol.
- **Réception CCID** : un paquet (MPS) d'abord, puis le reste. On n'attend jamais
  un paquet de longueur nulle (ZLP) que le lecteur n'enverrait pas.
  - `bSeq` vérifié ;
  - prolongation de délai prise en compte ;
  - un message trop long est lu jusqu'au bout puis jeté.
- **Tension** :
  - automatique si le lecteur sait la choisir ;
  - sinon 3 V puis 5 V.
- **Paramètres T=0 (lecteur TPDU)** : `SetParameters` à la vitesse par défaut
  (Fi/Di 0x11, sans PPS), sauf si le lecteur négocie seul.
- **La carte est coupée après chaque lecture.** L'état de repos est « présente
  inactive ». `READ_CARD` la rallume pour relire.
- **`RESTART`** coupe logiquement le port racine USB (VBUS est câblé en dur sur
  la carte), ce qui force la ré-énumération du lecteur.

Séquence Vitale 2 : d'après le script open source Cardpeek `vitale_2.lua` (CLA `00`).

1. `SELECT` de l'AID `D2 50 00 00 02 56 49 54 41 4C 45` (« VITALE »).
2. EF `2001` (pointeurs) : `E0{ A0{ 82… } 80… }`, qui liste les applications et leurs fichiers.
3. `READ BINARY` de chaque fichier par blocs de 256 octets.
4. Gabarit BER-TLV `65` = bénéficiaire :

   | Tag | Champ |
   |---|---|
   | `80` | nom |
   | `81` | prénom |
   | `82` | date de naissance |
   | `90` | nationalité |
   | `93` | NIR |

## Limites connues (v1.0)

- Vitale 1 reconnue mais non décodée.
- T=1 non géré avec un lecteur de niveau TPDU ; il l'est avec un lecteur APDU.
- Vitesse carte par défaut (sans PPS) : une lecture prend quelques secondes.
- Lecteur dont le descripteur de configuration dépasse 256 octets : refusé à
  l'énumération (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`).
- Un échange de carte plus rapide que 250 ms (scrutation) n'est pas vu.
- Hub USB : pris en charge par la pile IDF, non testé.
