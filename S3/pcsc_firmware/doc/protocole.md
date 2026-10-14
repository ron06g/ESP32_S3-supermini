# Protocole du port COM

Port **« COM »** de la carte N8R2 : pont USB-série CH343 sur UART0 (GPIO43/44),
**115200 bauds, 8N1**, sans contrôle de flux. Implémentation : `com_proto.h`
(réception, tramage) et `pcsc_firmware.ino` (traitement).

## Trame

```
STX (0x02)  TEXTE  CR (0x0D)  LF (0x0A)  EOT (0x04)
```

Le même format sert dans les deux sens.

- **Émission (module → hôte)** :
  - une trame est écrite d'un seul bloc, sous mutex : deux trames ne se mélangent jamais ;
  - le texte ne contient que des caractères 0x20..0x7E ; tout autre octet devient `?`.
- **Réception (hôte → module)** :
  - `STX` ouvre une trame neuve ; un reste partiel est jeté (resynchronisation) ;
  - `CR`, `LF`, `EOT` ou `ETX` la ferment. Le `LF` et l'`EOT` qui suivent un `CR` donnent des trames vides, ignorées ;
  - une **ligne nue** terminée par Entrée (sans `STX`) est aussi acceptée, pour tester depuis un terminal ;
  - espaces de début et de fin rognés, casse indifférente ;
  - 64 caractères au plus ; au-delà, la réponse est `UNKNOWN_CMD`.
- **Bruit hors trame** : à chaque reset **matériel** (bouton RESET, mise sous
  tension, flash), la ROM de l'ESP32 écrit une dizaine de lignes de log sur UART0,
  avant la bannière. L'hôte doit **ignorer tout ce qui n'est pas encadré STX…EOT**.
  `RESTART` ne redémarre pas l'ESP32 et ne produit donc pas ce log.
- **DTR/RTS** : sur le pont CH343, ces lignes pilotent EN/BOOT (auto-reset).
  L'hôte doit ouvrir le port sans les basculer ; `tools/pcsc_term.py` les force à
  inactives avant l'ouverture.

## Événements (spontanés)

| Moment | Trame(s) |
|---|---|
| Démarrage, `RESTART` | `CARD READER v1.0` |
| Lecteur énuméré | `READER_OK` (dès l'énumération) |
| Pas de lecteur 2,5 s après le démarrage ou `RESTART` | `READER_KO` |
| Lecteur branché / débranché | `READER_OK` / `READER_KO` (débranché avec une carte : `CARD_OUT` d'abord) |
| Sans lecteur : nouvelle recherche toutes les **30 s** | rien tant qu'aucun lecteur n'apparaît ; `READER_OK` s'il est trouvé |
| Carte insérée | `CARD_IN`, puis lecture automatique → **une** trame de résultat |
| Carte retirée | `CARD_OUT` (le résultat en cache est effacé) |

## Commandes

| Commande | Réponse |
|---|---|
| `RESTART` | réinitialise le **lecteur USB** (pas l'ESP32) puis rejoue la séquence de démarrage : `CARD READER v1.0`, `READER_OK`/`READER_KO`, et `CARD_IN` + résultat si une carte est présente |
| `IS_READER` | `READER_OK` / `READER_KO` |
| `IS_CARD` | `CARD_IN` / `CARD_OUT` (`CARD_OUT` sans lecteur) |
| `READ_CARD` | **relecture forcée** (coupure puis remise sous tension de la carte) → `NO_CARD` / `CARD_READ_ERROR` / `CARD_READ_NOT_CV:<CODE>` / `CARD_READ:…` |
| `CARD_STATE` | **dernier résultat en cache**, sans relire (mêmes réponses) |
| `INFO` | diagnostic (hors spécification initiale), voir plus bas |
| `DEBUG` / `DEBUG ON` / `DEBUG OFF` | bascule le **mode DEBUG** → `DEBUG_ON` / `DEBUG_OFF`, voir plus bas |
| autre | `UNKNOWN_CMD` |

**Ordre des réponses** :
- toutes les commandes passent par la tâche qui pilote le lecteur, donc les
  réponses restent dans l'ordre des événements ;
- une commande reçue pendant une lecture est traitée juste après celle-ci. Mesuré :
  0,5 s pour une Vitale 1 et 0,08 s pour une carte bancaire ; une lecture complète
  de Vitale 2 n'a pas encore été mesurée ;
- si l'hôte envoie trop de commandes, la file attend : aucune n'est perdue.

**Détails** :
- `READ_CARD` sur une carte qui vient d'être insérée renvoie `CARD_IN` puis le
  résultat de la lecture automatique, sans relire une deuxième fois.
- `RESTART` coupe **logiquement** le port USB, ce qui force la ré-énumération du
  lecteur. En dernier recours, si la pile USB est absente ou bloquée
  (`STEP=USBHOST` / `STUCK`), il redémarre l'ESP32 : le log ROM précède alors la
  bannière.

## Résultats de lecture

| Trame | Sens |
|---|---|
| `CARD_READ:NOM=…;PRENOM=…;NAISS=AAAAMMJJ;NIR=…` | Carte Vitale 2 lue. Un groupe par bénéficiaire, séparés par `\|` ; `;NAT=…` en plus si la nationalité est présente. Format **provisoire** : un transfert Base64 des données brutes est prévu |
| `CARD_READ_NOT_CV:V1` | Carte Vitale 1 (ATR `3F 65 25 …`) : reconnue, **non lue** (format propriétaire Bull B0', non documenté publiquement) |
| `CARD_READ_NOT_CV:T1` | carte uniquement T=1 sur un lecteur de niveau TPDU (une Vitale est en T=0) |
| `CARD_READ_NOT_CV:CB;…` | **carte bancaire** (EMV) : réseaux, libellé, titulaire, numéro **masqué** |
| `CARD_READ_NOT_CV:CPS;…` | **Carte de Professionnel de Santé** : identité, établissement, identifiant, profession (certificat) |
| `CARD_READ_NOT_CV:OPENPGP;…` | **carte OpenPGP** : nom du porteur, numéro de série |
| `CARD_READ_NOT_CV:PIV;…` | **carte PIV** : titulaire et établissement (certificat), GUID |
| `CARD_READ_NOT_CV:ID` | **document d'identité** à puce (ICAO) : reconnu seulement, données protégées |
| `CARD_READ_NOT_CV:<SW>` | autre carte : elle refuse le `SELECT` de l'application Vitale, p. ex. `6A82` |
| `CARD_READ_ERROR` | carte muette, échange en échec, carte retirée pendant la lecture, ou application Vitale sans bénéficiaire lisible |
| `NO_CARD` | (réponse à `READ_CARD` / `CARD_STATE`) aucune carte |

Les valeurs ne contiennent jamais `;`, `=` ni `|` : ces caractères sont remplacés
par `?` ou par une espace.

### Cartes reconnues autres que la Vitale

La trame `CARD_READ_NOT_CV` commence toujours par le **type**, puis donne les champs
connus sous la forme `;CLÉ=valeur`. Un hôte qui ne lit que le code avant le
premier `;` reste compatible.

```
CARD_READ_NOT_CV:CB;RESEAU=CB+VISA;LIBELLE=VISA DEBIT;NOM=MARTIN;PRENOM=CAMILLE;NUM=497010******1234
CARD_READ_NOT_CV:CPS;NOM=DURAND;PRENOM=ALICE;ETAB=CABINET MEDICAL;IDENT=810000000000;PROFESSION=MEDECIN
CARD_READ_NOT_CV:OPENPGP;NOM=MARTIN;PRENOM=CAMILLE;NUM=0005-0000ABCD
CARD_READ_NOT_CV:PIV;TITULAIRE=CAMILLE MARTIN;ETAB=DEMO CORP;NUM=0123456789ABCDEF0123456789ABCDEF
CARD_READ_NOT_CV:ID
```

(Exemples fictifs.)

| Champ | Contenu | Cartes |
|---|---|---|
| `RESEAU` | réseaux de paiement présents, séparés par `+` : `CB`, `VISA`, `VISA ELECTRON`, `V PAY`, `MASTERCARD`, `MAESTRO`, `AMEX`, `DISCOVER`, `JCB`, `UNIONPAY`, sinon `EMV` | CB |
| `LIBELLE` | libellé de l'application (« VISA DEBIT », nom de la banque…) | CB |
| `NOM`, `PRENOM` | nom et prénom du porteur, si la carte les sépare | CB, CPS, OPENPGP, PIV |
| `TITULAIRE` | nom complet en un seul champ (nom commun du certificat, ou nom EMV sans séparateur) | CB, CPS, OPENPGP, PIV |
| `ETAB` | établissement / organisation (champ `O` du certificat) | CPS, PIV |
| `IDENT` | identifiant du porteur (champ `serialNumber` du certificat, p. ex. RPPS / ADELI) | CPS |
| `PROFESSION` | profession / titre (champ `title` du certificat) | CPS |
| `NUM` | numéro de la carte | voir ci-dessous |

Le numéro (`NUM`) selon la carte :

| Carte | Numéro |
|---|---|
| CB | **masqué** : 6 premiers et 4 derniers chiffres |
| OPENPGP | fabricant + numéro de série |
| PIV | GUID |

Un champ absent de la carte n'apparaît pas.

**Uniquement des données publiques**, lisibles sans code PIN ni clé, en lecture seule :

- **CB** :
  - le module fait ce que fait tout terminal pour lire la carte
    (`GET PROCESSING OPTIONS` + `READ RECORD`). Il n'y a **ni paiement, ni code PIN** ;
    le compteur de transactions de la carte (ATC) avance de 1 ;
  - le numéro n'est **jamais** émis en entier, ni la date d'expiration. Un numéro
    complet avec sa date permettrait de copier la carte ; le masquage 6 + 4 est celui
    des tickets de caisse (PCI DSS) ;
  - beaucoup de cartes récentes n'enregistrent plus le nom du titulaire, ou un nom
    générique (« CARDHOLDER ») : le champ est alors absent ;
  - en mode DEBUG, les réponses de ces lectures sont **masquées**.
- **CPS** : reconnue par son ATR. L'identité vient du premier certificat lisible de la
  structure PKCS#15 (sujet X.509).
- **ID** : le nom est protégé par BAC/PACE, une clé tirée de la bande MRZ du document.
  La **CNIe et le passeport français sont sans contact** : un lecteur à contact ne les
  voit pas du tout.
- **Limite des lecteurs TPDU** (Gemalto, CLOUD 2700 R) : une carte qui ne parle que
  le protocole T=1, comme beaucoup de cartes OpenPGP, donne `NOT_CV:T1` avant toute
  identification.

## `INFO` (diagnostic)

Avec un lecteur :

```
INFO:READER=VVVV:PPPP;NAME=<produit>;LEVEL=TPDU|APDU|XAPDU;FEAT=<dwFeatures>;ATR=<hex>;STEP=<étape>;SW=<SW>;MS=<durée>
```

Sans lecteur (ou pile USB bloquée) :

```
INFO:READER=NONE;LASTDEV=<VID:PID(ENUM|NOT_CCID|CHAR)>;STEP=<étape>
```

| Champ | Sens |
|---|---|
| `LEVEL` | niveau d'échange CCID du lecteur |
| `FEAT` | `dwFeatures` du descripteur CCID |
| `ATR` | ATR de la dernière carte lue |
| `SW` | dernier mot d'état de la carte |
| `MS` | durée de la dernière lecture |
| `LASTDEV` | dernier appareil USB vu sans être retenu, voir ci-dessous |
| `STEP` | dernière étape en échec, voir ci-dessous |

Valeurs de `LASTDEV` :

| Valeur | Sens |
|---|---|
| `(ENUM)` | l'appareil a répondu (descripteur lu) : il est alimenté et câblé |
| `(NOT_CCID)` | l'appareil n'est pas un lecteur de carte |
| `(CHAR)` | lecteur de niveau « caractère », non pris en charge |
| `NONE` | **aucun appareil n'a jamais répondu** : vérifier l'alimentation (VBUS), voir [materiel.md](materiel.md) |

Valeurs de `STEP` :

| STEP | Échec |
|---|---|
| `NONE` | aucun |
| `USBHOST` | installation de la pile USB Host |
| `STUCK` | un transfert USB n'est jamais revenu ; `RESTART` redémarre alors l'ESP32 |
| `POWERON` | mise sous tension / ATR |
| `SELAID` | échange du `SELECT` de l'application Vitale |
| `SEL2001` / `RD2001` | sélection / lecture du fichier de pointeurs EF 2001 |
| `PTR` | fichier de pointeurs illisible |
| `SELAPP` / `SELEF` / `READ` | sélection d'une application, d'un fichier, ou lecture |
| `NO65` | aucun bénéficiaire (gabarit BER-TLV `65`) trouvé |

## Mode DEBUG

`DEBUG` (ou `DEBUG ON` / `DEBUG OFF`) active un journal détaillé, utile pour
comprendre un lecteur non détecté ou une carte illisible.

- Il **n'est pas mémorisé** : un redémarrage le coupe.
- Chaque ligne est une trame normale, préfixée `DBG:` et horodatée en secondes
  depuis le démarrage du module :

  ```
  <STX>DBG:298.083 USB : NEW_DEV adresse 1<CR><LF><EOT>
  ```

- Un hôte peut simplement ignorer les trames `DBG:`.

| Ligne | Ce qu'elle montre |
|---|---|
| `PORT …` | état **électrique** du port USB (registre HPRT) : `alim`, `connecte`, `actif`, `vitesse`, niveaux `D+` / `D-`. Journalisée à chaque changement, et toutes les 10 s avec `ETAT` |
| `ETAT …` | lecteur, carte, délai de grâce, recherche, prochaine recherche, état de la LED |
| `PHY …` | routage du PHY USB interne : `phy_sel=0` = PHY interne ; `sw_hw_sel=1 sw_sel=1` = branché sur l'OTG |
| `DIAG : aucun appareil ELECTRIQUEMENT connecte` | rien ne tire D+ ou D- : le lecteur n'est pas alimenté (VBUS) ou pas branché |
| `ENUM : appareil qui repond VVVV:PPPP` | un appareil a répondu (descripteur lu), **avant** même la fin de l'énumération |
| `USB : NEW_DEV / DEV_GONE` | événements de la pile USB Host |
| `ATTACH …`, `CFG […]`, `INTF` / `EP`, `CCID …` | descripteurs du lecteur décodés, et motif d'un éventuel refus |
| `LECTEUR PRET …` | lecteur retenu (interface, endpoints, tampons) |
| `SLOT : bmICCStatus …` | changement d'état du logement de carte |
| `CCID> / CCID<` | messages CCID échangés, en hexa. La scrutation toutes les 250 ms n'est pas journalisée |
| `CCID 0x.. : echec bError=…` | erreur renvoyée par le lecteur (`ICC_MUTE`, `XFR_PARITY_ERROR`…) |
| `APDU> / APDU<` | commandes et réponses de la carte, en hexa |
| `CARTE …`, `VITALE …`, `LECTURE …` | étapes de la lecture : tension, ATR, mode, `SetParameters`, application, fichiers |
| `RECHERCHE …`, `PORT : coupure…` | recherche périodique sans lecteur |
| `XFER ep=…` | transfert USB en échec (statut, délai, annulation) |
| `DBG:…IDF:` | erreurs de la pile USB Host de l'ESP-IDF (les bibliothèques précompilées du cœur ne contiennent que le niveau ERREUR) |

**Attention** : en mode DEBUG, les échanges avec la carte (donc nom, NIR…) passent
en clair sur le port COM. Ne pas versionner de journal contenant des données
d'une Vitale 2.

Capture conseillée :

```bat
python S3\pcsc_firmware\tools\pcsc_term.py COMx --listen 300 --log debug.txt "DEBUG ON"
```

Des exemples réels sont dans [journaux/](journaux/).

## Outil `tools/pcsc_term.py`

Application graphique (et exe autonome) pour ouvrir le port, lancer toutes les
commandes, suivre les trames et voir l'état du lecteur. Elle a aussi un mode démo
sans matériel et un mode ligne de commande pour les scripts.

Tout est décrit dans [terminal.md](terminal.md).
