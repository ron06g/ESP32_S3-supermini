# Plan — firmware `pcsc_firmware` (ESP32-S3 N8R2 : lecteur PC/SC USB → Carte Vitale → port COM)

## Contexte

Nouveau firmware **indépendant du firmware HID**, dédié à la carte ESP32-S3-WROOM-1 **N8R2** (2 USB-C) :
- prise **« USB »** (USB natif GPIO19/20) en **USB Host** : on y branche un lecteur de carte à puce **CCID** (classe 0x0B, « PC/SC ») via un adaptateur OTG ;
- prise **« COM »** (pont CH343 sur UART0, GPIO43/44) = port COM « debug » avec un protocole texte simple `<STX>…<CR><LF><EOT>`.

But : **preuve de faisabilité**. On lit les données publiques d'une Carte Vitale 2 et on les annonce sur le port COM. Plus tard : transfert Base64 des données brutes.
BLE et Wi-Fi ne sont jamais initialisés, CPU à 80 MHz (chaleur, sécurité).

Ce qui a été vérifié pendant l'exploration :
- Le cœur **esp32 3.3.11** (ESP-IDF 5.5.5) embarque la pile USB Host : `usb/usb_host.h` + `libusb.a`, avec la gestion des hubs activée. Espressif ne fournit **aucun pilote CCID** : on écrit un pilote CCID minimal.
- APDU Vitale 2, d'après le script open source **Cardpeek `vitale_2.lua`** :
  - CLA `00` ;
  - AID Vitale = `D2 50 00 00 02 56 49 54 41 4C 45` (« VITALE ») ;
  - fichier de pointeurs EF `2001` ;
  - READ BINARY par blocs de 256 octets ;
  - données en BER-TLV : `65` = bénéficiaire {`80` nom, `81` prénom, `82` date de naissance AAAAMMJJ, `90` nationalité, `93` NIR}.
- ATR (liste pcsc-tools) : Vitale 2 = `3B 75 13 00 00 xx 09 EA 90 00`, donc **T=0** ; Vitale 1 = `3F 65 25 …` (convention inverse, format Bull B0' propriétaire).

## Décisions validées avec l'utilisateur

- **Vitale 1** : reconnue par son ATR → `CARD_READ_NOT_CV:V1` (non lue en v1.0).
- **RESTART** : réinitialise le **lecteur USB**, sans redémarrer l'ESP32 :
  - coupure du port racine USB, ce qui force une ré-énumération ;
  - remise à zéro de l'état, puis envoi de la bannière ;
  - pas de log ROM parasite sur le COM.
- **Commande de diagnostic `INFO`** ajoutée, hors spécification.

## Prérequis matériels (à signaler à l'utilisateur, point bloquant n°1)

- **VBUS** : sur les DevKit, la prise USB native n'alimente généralement **pas** le périphérique branché (diode sur VBUS). Il faut :
  - soit ponter le cavalier à souder « USB-OTG » (ou « IN-OUT ») s'il existe ;
  - soit utiliser un câble OTG en Y ou un hub USB alimenté.

  Vérifier au multimètre qu'on mesure 5 V sur la prise A femelle de l'adaptateur. Avec le cavalier ponté, ne jamais brancher la prise « USB » sur un PC.
- **Flash** uniquement par la prise **« COM »** : la prise « USB » est devenue hôte.
- L'application côté hôte doit ouvrir le COM **sans basculer DTR/RTS** (sinon auto-reset ESP32). Le script de test les force à OFF.
- Le reset matériel (bouton RESET ou mise sous tension) affiche le log ROM de l'ESP32 sur UART0. L'hôte ignore tout ce qui n'est pas encadré STX…EOT.

## Fichiers à créer

```
S3/pcsc_firmware/
  pcsc_firmware.ino   orchestration : setup (UART0, logs coupés), création des tâches, file unique g_ccidQueue
  com_proto.h         port COM : tramage RX/TX, parseur de commandes, envoi atomique d'une trame
  usb_ccid.h          USB Host (install, tâches lib/client), attache/détache, descripteur CCID, échanges bulk synchrones, messages CCID
  apdu_t0.h           couche APDU commune (T=0) : cas 4→3, 61xx→GET RESPONSE, 6Cxx→renvoi avec Le corrigé
  vitale.h            logique carte : ATR, sélection Vitale 2, fichier pointeurs, lecture des EF, parseur BER-TLV, construction du résultat
  README.md           protocole, matériel (VBUS !), compilation/flash, procédure de test
  tools/pcsc_term.py  terminal de test (pyserial) : envoie des commandes tramées, affiche les trames reçues (DTR/RTS à OFF)
S3/pcsc_firmware_compile.bat
S3/pcsc_firmware_flash.bat
S3/firmware/pcsc_firmware_vYYMM.dd/   (produit par le script de compilation, *.bin ignorés par git)
```

Mise à jour de `CLAUDE.md` : courte section « Second firmware `S3/pcsc_firmware/` », indépendant du HID, avec son FQBN, ses scripts et son protocole.

## Build

FQBN :
`esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,CPUFreq=80,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled,DebugLevel=none`

- `USBMode=default` + `CDCOnBoot=default` : le sketch n'appelle **jamais** `USB.begin()` et ne référence ni `USB`, ni `USBSerial`, ni `HWCDCSerial`. Le contrôleur OTG reste donc libre pour `usb_host_install()` (vérifié : `main.cpp` ne démarre rien sans *on-boot*).
- CPU 80 MHz : on ne descend pas à 40 MHz, la PLL est nécessaire à l'USB.
- Radios : aucune bibliothèque BT/Wi-Fi n'est incluse. `initArduino` libère la mémoire du contrôleur BT si BT n'est pas utilisé.
- Dans `setup()` : `esp_log_level_set("*", ESP_LOG_NONE)` + `Serial0.setDebugOutput(false)`. Sinon les logs d'erreur de l'IDF (pile USB) sortiraient sur le port protocole.

## Protocole COM (UART0, `Serial0`, 115200 8N1)

**Trame** : `STX(0x02) TEXTE CR LF EOT(0x04)`, écrite d'un seul `write()` sous mutex.
- Le contenu est assaini : uniquement 0x20–0x7E, sans `;`, `=` ni `|` dans les valeurs. Tout autre octet devient `?`.

**Réception** :
- `STX` ouvre une trame ; `CR`, `LF`, `EOT` ou `ETX` la ferment.
- Les lignes nues (sans STX) sont aussi acceptées, pour tester depuis un terminal.
- Espaces rognés, insensible à la casse, 64 caractères max.

**Événements spontanés** :

| Moment | Trame(s) |
|---|---|
| Démarrage et RESTART | `CARD READER v1.0` |
| Lecteur énuméré (ou absent après un délai de grâce de 2,5 s) | `READER_OK` / `READER_KO` |
| Lecteur branché ou débranché | `READER_OK` / `READER_KO` (débranché avec une carte : `CARD_OUT` avant) |
| Carte insérée | `CARD_IN`, puis lecture automatique → une trame `CARD_READ:…`, `CARD_READ_ERROR` ou `CARD_READ_NOT_CV:<CODE>` |
| Carte retirée | `CARD_OUT` (le résultat en cache est effacé) |

**Commandes** :

| Commande | Réponse |
|---|---|
| `RESTART` | bannière puis séquence de démarrage (voir plus bas) |
| `IS_READER` | `READER_OK` / `READER_KO` |
| `IS_CARD` | `CARD_IN` / `CARD_OUT` (`CARD_OUT` si pas de lecteur) |
| `READ_CARD` | **force** une relecture (coupure puis remise sous tension de la carte) → `NO_CARD` / `CARD_READ_ERROR` / `CARD_READ_NOT_CV:<CODE>` / `CARD_READ:…` |
| `CARD_STATE` | dernier résultat **en cache**, sans relire (mêmes réponses) |
| `INFO` (diagnostic) | `INFO:READER=VVVV:PPPP;NAME=<produit>;LEVEL=TPDU\|APDU\|XAPDU\|CHAR;ATR=<hex>;STEP=<dernière étape en échec>;SW=<dernier SW>;MS=<durée de lecture>` |
| autre | `UNKNOWN_CMD` |

**Format provisoire de `CARD_READ`** :
- un groupe par bénéficiaire, groupes séparés par `|` ;
- champs présents seulement s'ils existent sur la carte ;
- dates brutes AAAAMMJJ.

Exemple : `CARD_READ:NOM=DUPONT;PRENOM=JEAN;NAISS=19800115;NIR=180017512345678|NOM=…`

**Codes `CARD_READ_NOT_CV`** :
- `V1` : ATR de Vitale 1 ;
- `T1` : carte uniquement T=1 sur un lecteur de niveau TPDU (une Vitale est en T=0) ;
- `<SW hexa>` : SELECT de l'AID Vitale refusé, par exemple `6A82`.

**Cas de `CARD_READ_ERROR`** : carte muette, erreur ou timeout USB/CCID, retrait pendant la lecture, ou AID Vitale présent mais aucun bénéficiaire (`65`) trouvé.

Ordre garanti : toutes les commandes passent par la file du worker `ccid`, donc les réponses s'intercalent correctement avec les événements. Une commande reçue pendant une lecture est traitée juste après celle-ci.

## Architecture firmware

**Tâches** (un seul propriétaire par ressource, comme dans le firmware HID) :
- `usbLib` : boucle `usb_host_lib_handle_events()`.
- `usbCli` : boucle `usb_host_client_handle_events()`.
  - Le callback client poste seulement `NEW_DEV(addr)` / `DEV_GONE(hdl)` dans `g_ccidQueue`.
  - Les callbacks de transfert (exécutés dans ce contexte) libèrent un sémaphore par transfert.
  - Aucun appel bloquant depuis un callback.
- `ccid` : **seul propriétaire du lecteur**. Il traite `g_ccidQueue` (événements USB + commandes COM) avec un timeout de 250 ms qui sert de période de scrutation.
- `com` : lit `Serial0`, parse les trames et poste les commandes dans `g_ccidQueue`.

**Ordre d'initialisation** : `usb_host_install` → `usb_host_client_register` → démarrage de `usbLib`/`usbCli`. Ainsi, aucun `NEW_DEV` n'est perdu si le lecteur est déjà branché. Filet de sécurité : `usb_host_device_addr_list_fill()`.

**`usb_ccid.h` — attache** (dans la tâche `ccid`) :
1. `usb_host_device_open` puis lecture du descripteur de configuration actif.
2. Recherche de l'interface de classe `0x0B` (repli : `0xFF` portant un descripteur `0x21` de longueur `0x36`). Le descripteur `0x21` est cherché **n'importe où** entre cette interface et la suivante.
3. Lecture de `bVoltageSupport`, `dwProtocols`, `dwFeatures` et `dwMaxCCIDMessageLength`.
   - Niveau d'échange (bits 16-18 de `dwFeatures`) : TPDU, APDU court ou APDU étendu.
   - Un lecteur en niveau caractère est refusé : `READER_KO`, et `INFO` affiche `LEVEL=CHAR`.
4. Recherche des endpoints bulk OUT/IN, `usb_host_interface_claim`, allocation des transferts (taille ≥ `dwMaxCCIDMessageLength`, plafonnée à 2 Ko, arrondie au MPS).
5. VID:PID et nom produit relevés via `usb_host_device_info` pour `INFO`.
6. Un périphérique non CCID est fermé aussitôt.

**Détache (`DEV_GONE`)** :
1. Attendre ou annuler les transferts en vol : halt → flush → attente du callback `CANCELED` → clear.
2. `usb_host_interface_release` puis `usb_host_device_close`, puis `usb_host_transfer_free`.

**Échange synchrone `ccidXfer()`** :
- Le sémaphore est purgé avant chaque soumission.
- Envoi : OUT de 10 + n octets.
- Réception : IN d'**exactement un MPS**, puis, si 10 + `dwLength` dépasse ce qui a été reçu, IN de `usb_round_up_to_mps(reste)`. Cela évite le blocage sur ZLP ; les complétions de 0 octet sont ignorées.
- `bSeq` vérifié (réponses périmées jetées).
- Prolongation de délai (`bmCommandStatus = 2`) : on relit et le délai repart.
- Timeout 5 s par attente via le sémaphore (`timeout_ms` n'est pas géré par l'IDF), puis annulation halt/flush/clear.
- En cas de STALL réel : `CLEAR_FEATURE(ENDPOINT_HALT)` par transfert de contrôle.

**Messages CCID** :
- `GetSlotStatus` 0x65 : scrutation de présence via `bmICCStatus` (0 = active, 1 = présente inactive, 2 = absente).
- `IccPowerOn` 0x62 → ATR. Choix de la tension :
  - auto si `dwFeatures & 0x08` ;
  - sinon 3 V puis 5 V, selon `bVoltageSupport`, avec `IccPowerOff` entre deux essais et nouvel essai sur `bError` 0xFE/0x07.
- `IccPowerOff` 0x63.
- `XfrBlock` 0x6F.
- `SetParameters` 0x61, pour T=0, seulement si le lecteur est TPDU et sans `0x40` :
  - Fi/Di `0x11` (vitesse par défaut, sans PPS) ;
  - `bmTCCKST0` = `0x02` si TS = `3F` ;
  - temps de garde = TC1, WI = `0x0A`.

**Détection de présence** :
- La carte reste alimentée après lecture, comme sous PC/SC.
- `bmICCStatus = 1` alors qu'on la croit alimentée signifie qu'elle a été échangée : `CARD_OUT` puis `CARD_IN` et relecture.
- Le lecteur débranché est géré via `DEV_GONE`.

**RESTART** (dans la tâche `ccid`) :
1. `IccPowerOff` (au mieux) puis détache.
2. `usb_host_lib_set_root_port_power(false)`, 500 ms, puis `(true)`.
3. Remise à zéro de l'état et envoi de `CARD READER v1.0`.
4. Nouveau délai de grâce de 2,5 s → `READER_OK` ou `READER_KO`, puis `CARD_IN` + lecture si une carte est présente.

Limite : VBUS est câblé en dur sur la carte, donc la coupure est **logique** (déconnexion + ré-énumération) et non électrique.

**`apdu_t0.h`** :
- Une commande de cas 4 est envoyée en cas 3 (Le retiré).
- `61xx` → `00 C0 00 00 xx`, en boucle, avec concaténation des données.
- `6Cxx` → renvoi de la commande avec P3 = xx.
- Même chemin pour les lecteurs TPDU et APDU (les lecteurs APDU renvoient parfois eux aussi `61xx`).

**`vitale.h` — séquence de lecture** :
1. Mise sous tension et analyse de l'ATR (TS, T0, TA1/TC1, protocoles via TDi, octets historiques).
   - ATR commençant par `3F 65 25` → `NOT_CV:V1`.
   - Carte uniquement T=1 sur un lecteur TPDU → `NOT_CV:T1`.
   - Si le lecteur est TPDU (et sans `0x40`) : `SetParameters`.
2. `SELECT` de l'AID Vitale : `00 A4 04 00 0B D2 50 00 00 02 56 49 54 41 4C 45`. Un SW autre que `9000`/`61xx` → `NOT_CV:<SW>`.
3. `SELECT` EF `2001` (`00 A4 02 00 02 20 01`), puis `readBin()` :
   - `00 B0 hi lo 00` par pas de 256 octets ;
   - arrêt sur bloc court, `6282`, `6B00` ou SW ≠ `9000` ;
   - plafond de 4 Ko par fichier.
4. Analyse des pointeurs, qui suit exactement Cardpeek :
   - `E0{ A0{ 82… } 80… }` ;
   - entrée `82` : clé = octets 0-1, AID = octets 4 à la fin ;
   - enregistrement `80` : nom = octets 0-7, clé = octets 8-9, FID = octets 10-11.
5. Pour chaque application : `SELECT` de l'AID, puis pour chaque fichier `SELECT` du FID (P1 = 02) et `readBin()`.
   - Les contenus commençant par `00` ou `04 00` sont ignorés.
   - Au plus 16 fichiers.
6. Parseur BER-TLV récursif (tags sur plusieurs octets, longueurs `81`/`82`) : on collecte chaque `65` et ses enfants `80/81/82/90/93`.
7. Construction de la chaîne de résultat ; on note la durée, l'étape et le SW pour `INFO`.

## Scripts `.bat` (copiés des scripts HID et adaptés, mêmes conventions)

**`S3/pcsc_firmware_compile.bat`** :
- même recherche de `arduino-cli` (`:find_cli`), **sans** l'étape de régénération web ;
- FQBN N8R2 ci-dessus, `--output-dir S3\build\pcsc_firmware` ;
- publie `S3\firmware\pcsc_firmware_vYYMM.dd\pcsc_firmware_vYYMM.dd{,.bootloader,.partitions}.bin`.

**`S3/pcsc_firmware_flash.bat [COMx …] [dossier|fichier.bin]`** :
- prend la dernière version `pcsc_firmware_v*` et la copie dans `%TEMP%\pcsc_flash` ;
- `arduino-cli upload --input-file`, même FQBN.
- **Détection de port = ponts USB-série** (VID `1A86` WCH/CH343, `10C4` CP210x, `0403` FTDI) et **pas** `303A`, pour ne pas flasher par erreur un module S3-KBD branché.
- Menu si plusieurs ports ; `COMx` forcé sinon.
- Auto-reset par DTR/RTS du CH343 ; RESET manuel seulement si la carte ne redémarre pas.

## Vérification

1. **Compilation** avec `S3\pcsc_firmware_compile.bat` (faite par moi) : 0 erreur, binaires publiés dans `S3\firmware\pcsc_firmware_v2610.14\`, taille relevée.
2. **Flash** avec `pcsc_firmware_flash.bat COMx`, seulement après confirmation du port de la N8R2 par l'utilisateur (piège déjà vécu : flash du mauvais module).
3. **Sans lecteur**, avec `python S3/pcsc_firmware/tools/pcsc_term.py COMx` :
   - bannière, puis `READER_KO` après 2,5 s ;
   - `IS_READER` → `READER_KO`, `IS_CARD` → `CARD_OUT`, `READ_CARD` → `NO_CARD` ;
   - `INFO` répond ;
   - `RESTART` → bannière + `READER_KO` ;
   - une commande inconnue → `UNKNOWN_CMD`.
4. **Avec lecteur** (VBUS vérifié) :
   - `READER_OK` ; `INFO` affiche VID:PID et `LEVEL` ;
   - carte bancaire → `CARD_IN` + `CARD_READ_NOT_CV:6A82` (ou un autre SW) ;
   - **Carte Vitale 2** → `CARD_IN` + `CARD_READ:NOM=…` ;
   - `CARD_STATE` renvoie le cache ; `READ_CARD` relit ;
   - retrait → `CARD_OUT` ; débranchement du lecteur → `READER_KO` ; rebranchement → `READER_OK` ;
   - `RESTART` avec la carte insérée → bannière, `READER_OK`, `CARD_IN`, `CARD_READ:…`.
5. En cas d'échec, `INFO` (`STEP`, `SW`, `LEVEL`, `ATR`) oriente le diagnostic. Contrôle croisé possible : lire la même carte sur le PC (pyscard) avec les mêmes APDU.

## Limites connues de la v1.0 (preuve de faisabilité)

- Vitale 1 non décodée.
- T=1 en niveau TPDU non géré.
- Vitesse carte par défaut, sans PPS : lecture de quelques secondes.
- Lecteurs dont le descripteur de configuration dépasse 256 octets refusés (`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE`).
- Échange de carte en moins de 250 ms non vu si la carte n'était pas alimentée.
- Hub USB supporté par la pile mais non testé.
- Pas de commit sans demande explicite.