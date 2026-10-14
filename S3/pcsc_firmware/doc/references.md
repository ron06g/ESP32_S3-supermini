# Références

## Carte Vitale

- **Cardpeek, script `vitale_2.lua`** (L1L1, GPL v3) : seule source publique trouvée
  pour la séquence de lecture d'une Vitale 2. Elle a servi de modèle à `vitale.h`.
  <https://github.com/L1L1/cardpeek/blob/master/dot_cardpeek_dir/scripts/vitale_2.lua>
  - AID application Vitale : `D2 50 00 00 02 56 49 54 41 4C 45` (« VITALE ») ;
  - AID « MF Vitale » : `D2 50 00 00 02 4D 46 5F 56 49 54 41 4C 45` ;
  - fichier de pointeurs : EF `2001` sous l'application Vitale, `E0{ A0{ 82… } 80… }` ;
  - lecture `READ BINARY` par blocs de 256 octets, CLA `00` ;
  - gabarit `65` = bénéficiaire : `80` nom, `81` prénom, `82` date de naissance,
    `90` nationalité, `93` NIR ;
  - gabarit `66` = carte : `80` date de début de validité. Il n'est pas exploité ici.
- **Vitale 1** : format propriétaire (masque Bull B0', CLA `BC`), **non documenté
  publiquement**. La documentation officielle et les API (« Lecture Vitale ») passent
  par le **GIE SESAM-Vitale**. C'est pourquoi le firmware la reconnaît sans la lire.

## ATR (liste pcsc-tools de Ludovic Rousseau)

<https://raw.githubusercontent.com/LudovicRousseau/pcsc-tools/master/smartcard_list.txt>

| ATR | Carte |
|---|---|
| `3B 75 13 00 00 xx 09 EA 90 00` (xx = 43…4C) | Carte Vitale 2 : T=0 implicite, TA1 = 13, mode négociable |
| `3F 65 25 00 A[34] 09 6A 90 00` | Sesam Vitale (Vitale 1) : convention inverse, T=0. **Carte de l'utilisateur** : `3F 65 25 00 A4 09 6A 90 00` |
| `3F 65 25 00 2[2C] 09 [F6]9 90 00`, `3F 65 25 00 52 09 6A 90 00` | autres Vitale 1 |
| `3B FF 13 00 00 10 00 31 01 F1 56 40 11 00 19 00 00 00 00 00 00 00` | Revolut Visa / BVG (carte bancaire de la recette) : TA2 = 00, **mode spécifique** |

## Lecteurs (base du pilote libccid, Ludovic Rousseau)

- Liste des lecteurs : <https://ccid.apdu.fr/ccid/supported.html>
- Gemalto GemPC Twin, 08E6:3437 : <https://ccid.apdu.fr/ccid/readers/GemPCTwin.txt>
- Identiv CLOUD 2700 R, 04E6:5810 : <https://ccid.apdu.fr/ccid/readers/Identive_CLOUD_2700_R.txt>

| | GemPC Twin | CLOUD 2700 R |
|---|---|---|
| Endpoints | bulk IN / bulk OUT / interruption IN | idem |
| `bVoltageSupport` | 0x07 | 0x07 |
| `dwProtocols` | T=0, T=1 | T=0, T=1 |
| Horloge | 4,8 MHz relevé en DEBUG (libccid indique 4 MHz pour une révision antérieure) | 4,8 / 16 MHz |
| `dwFeatures` | 0x00010230 : **TPDU**, horloge et débit automatiques, NAD | libccid : 0x000404BA (**APDU étendu**, paramètres d'après l'ATR, tension, horloge, débit, PPS et IFSD automatiques). **Relevé sur l'exemplaire de l'utilisateur : 0x000100BA** (TPDU, mêmes automatismes sauf IFSD) |
| `dwMaxCCIDMessageLength` | 271 | 271 |

## Normes

- **USB CCID** (USB-IF, *Smart Card CCID*, rév. 1.1) : messages `PC_to_RDR_*` /
  `RDR_to_PC_*`, en-tête de 10 octets, `bStatus` / `bError`, descripteur de classe
  `0x21` (54 octets), `dwFeatures`.
- **ISO/IEC 7816-3** : ATR (TS, T0, TAi/TBi/TCi/TDi), conventions directe et inverse,
  modes négociable et spécifique (TA2), PPS, T=0, classes de tension.
- **ISO/IEC 7816-4** : `SELECT` (A4), `READ BINARY` (B0), `GET RESPONSE` (C0), mots
  d'état (`9000`, `61xx`, `6Cxx`, `6A82`, `6282`, `6B00`).
- **EMV Books 1 et 3** (cartes de paiement) :
  - annuaire `1PAY.SYS.DDF01` (PSE, cartes à contact) et AID enregistrés (RID
    `A0 00 00 00 42` CB, `…03` Visa, `…04` Mastercard / Maestro, `…25` Amex) ;
  - `GET PROCESSING OPTIONS` (`80 A8`, PDOL `9F38`, AFL `94`), `READ RECORD` ;
  - tags `5F20` / `9F0B` (titulaire), `5A` / `57` (numéro, émis masqué),
    `50` / `9F12` (libellé).
- **PCI DSS** : un numéro de carte ne s'affiche que masqué (au plus 6 premiers + 4
  derniers chiffres).
- **OpenPGP Card** v3.4 (GnuPG e.V.) : AID `D2 76 00 01 24 01`, DO `4F` (AID complet,
  fabricant + numéro de série), DO `65` / `5B` (nom « Nom<<Prénom », ISO 8859-1).
- **NIST SP 800-73-4** (PIV) : AID `A0 00 00 03 08 00 00 10 00 01 00`, `GET DATA`
  (`00 CB 3F FF`, tag `5C`), CHUID `5FC102` (GUID `34`), certificats `5FC105` /
  `5FC101` (`70` certificat, `71` CertInfo).
- **ICAO Doc 9303** (documents de voyage) : application eMRTD
  `A0 00 00 02 47 10 01`. Données protégées par BAC / PACE.
- **PKCS#15 / ISO 7816-15** (structure des cartes à certificats) : EF.DIR `2F00`,
  ODF `5031`, CDF ; **X.509 / RFC 5280** pour le sujet des certificats.
- **OpenSC**, `card-iasecc.c` : ATR des cartes CPS « IAS/ECC CPx »
  `3B:AC:00:40:2A:00:12:25:00:64:80:00:03:10:00:90:00`, masque
  `FF:00:00:00:00:FF:FF:FF:FF:FF:FF:00:00:00:FF:FF:FF`. Lues en PKCS#15 standard
  (`pkcs15-iasecc.c`).
  <https://github.com/OpenSC/OpenSC/blob/master/src/libopensc/card-iasecc.c>

## ESP32-S3, ESP-IDF, Arduino

- Cœur Arduino-ESP32 **3.3.11**, ESP-IDF **5.5.5** : la pile USB Host est précompilée.
  - API : `include/usb/include/usb/usb_host.h` ;
  - `CONFIG_USB_HOST_HUBS_SUPPORTED=1` ;
  - `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=1` ;
  - `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=256` ;
  - `CONFIG_LOG_MAXIMUM_LEVEL=1` : ERREUR seulement.
- ESP-IDF v5.5, `components/usb/hub.c` : `hub_root_stop` / `hub_root_start`, utilisés
  par `usb_host_lib_set_root_port_power()`.
  <https://github.com/espressif/esp-idf/blob/v5.5.1/components/usb/hub.c>
- ESP-IDF v5.5, `components/usb/enum.c` : le filtre d'énumération est appelé dès la
  lecture du descripteur de périphérique ; `bConfigurationValue` est prérempli.
- Registre HPRT du contrôleur DWC OTG : `soc/usb_dwc_struct.h` (`USB_DWC.hprt_reg`,
  base 0x60080000 + 0x440). On y lit `prtconnsts`, `prtena`, `prtlnsts` (D+ / D-),
  `prtpwr` et `prtspd`.
- **EspUsbHost** (tanakamasayuki) : une DevKitC-1 n'alimente pas un périphérique sur sa
  prise OTG.
  <https://github.com/tanakamasayuki/EspUsbHost>

## Documents du projet

- Plan initial validé : [../Ressources/cahier_des_charges.md](../Ressources/cahier_des_charges.md).
  Il date d'avant la LED, la coupure de la carte après lecture, les files séparées,
  le mode DEBUG et la recherche toutes les 30 s.
- Historique et décisions : [historique.md](historique.md).
