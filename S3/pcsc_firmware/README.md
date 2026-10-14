# pcsc_firmware — lecteur de Carte Vitale sur ESP32-S3 N8R2

Firmware **indépendant** du firmware HID (`S3/hid_firmware`), dédié à la carte
**ESP32-S3-WROOM-1 N8R2** à deux prises USB-C.

| Prise | Rôle |
|---|---|
| **USB** (USB natif, GPIO19/20) | **USB Host** : un lecteur de carte à puce **CCID** (« PC/SC », classe USB 0x0B) y est branché via un adaptateur OTG |
| **COM** (pont CH343 sur UART0) | port COM : protocole texte `<STX>…<CR><LF><EOT>` à **115200 8N1**, et flash du firmware |

Le module lit les **données publiques d'une Carte Vitale 2** (nom, prénom, date
de naissance, NIR) et les annonce sur le port COM. C'est une **preuve de
faisabilité** : le transfert Base64 des données brutes viendra ensuite.

- **Sobriété** : BLE et Wi-Fi ne sont jamais initialisés, le CPU tourne à 80 MHz,
  et la carte est coupée après chaque lecture.
- **LED RGB** (GPIO48) : elle affiche l'état du lecteur et de la carte.
- **Sans lecteur** : nouvelle recherche toutes les 30 s.
- **Mode `DEBUG`** : journal détaillé sur le port COM.

> **Premier point à vérifier : l'alimentation du lecteur.** La prise USB native de
> la N8R2 ne fournit généralement pas le 5 V. Si le lecteur n'est pas détecté, voir
> [doc/materiel.md](doc/materiel.md) : en mode `DEBUG`, `PORT … connecte=0 D+=0`
> signifie que le lecteur n'est pas alimenté.

## Démarrage rapide

```bat
S3\pcsc_firmware_compile.bat                                   REM compile -> S3\firmware\pcsc_firmware_vYYMM.dd\
S3\pcsc_firmware_flash.bat COM12                               REM flashe la derniere version
python S3\pcsc_firmware\tools\pcsc_term.py COM12 INFO IS_READER
python S3\pcsc_firmware\tools\pcsc_term.py COM12 --listen 300 --log debug.txt "DEBUG ON"
```

## Protocole (résumé)

| Sens | Trames |
|---|---|
| Événements | `CARD READER v1.0`, `READER_OK` / `READER_KO`, `CARD_IN`, `CARD_OUT`, `CARD_READ:…`, `CARD_READ_ERROR`, `CARD_READ_NOT_CV:<CODE>` |
| Commandes | `RESTART`, `IS_READER`, `IS_CARD`, `READ_CARD` (relecture forcée), `CARD_STATE` (cache), `INFO` (diagnostic), `DEBUG` / `DEBUG ON` / `DEBUG OFF` |

Codes de `CARD_READ_NOT_CV` :
- `V1` : Vitale 1, reconnue mais non lue ;
- `T1` : carte T=1 sur un lecteur TPDU ;
- `<SW>` : la carte n'a pas l'application Vitale, p. ex. `6A82`.

Détail complet : [doc/protocole.md](doc/protocole.md).

## Documentation

| Document | Contenu |
|---|---|
| [doc/protocole.md](doc/protocole.md) | trame, événements, commandes, résultats, `INFO`, mode `DEBUG`, outil `pcsc_term.py` |
| [doc/materiel.md](doc/materiel.md) | carte N8R2, alimentation du lecteur (VBUS), port COM (DTR/RTS), sobriété, **LED** |
| [doc/build_flash.md](doc/build_flash.md) | chaîne d'outils, FQBN, scripts `.bat`, détection du port, vérification |
| [doc/architecture.md](doc/architecture.md) | tâches, USB Host / CCID, APDU T=0, séquence Vitale 2, `RESTART`, recherche toutes les 30 s |
| [doc/recette.md](doc/recette.md) | procédure de test, **résultats du 2026-10-14**, lecteurs testés, reste à valider |
| [doc/journaux/](doc/journaux/) | journaux `DEBUG` réels de la recette, sans donnée personnelle |
| [doc/references.md](doc/references.md) | sources : Cardpeek (Vitale 2), ATR pcsc-tools, descripteurs libccid, normes, ESP-IDF |
| [doc/historique.md](doc/historique.md) | demande initiale, décisions, chronologie, points ouverts |
| [Ressources/cahier_des_charges.md](Ressources/cahier_des_charges.md) | plan initial validé (antérieur à la LED, au mode DEBUG et à la recherche toutes les 30 s) |

## Arborescence

```
S3/
  pcsc_firmware_compile.bat      compilation -> S3/firmware/pcsc_firmware_vYYMM.dd/
  pcsc_firmware_flash.bat        flash de la derniere version (ponts USB-serie uniquement)
  firmware/pcsc_firmware_v*/     versions compilees (.bin, ignores par git)
  pcsc_firmware/
    pcsc_firmware.ino            orchestration, tache ccid, commandes, RESTART, recherche 30 s
    com_proto.h                  port COM : tramage, commandes, comSend()
    debug_log.h                  mode DEBUG : trames DBG:, logs ESP-IDF
    usb_ccid.h                   USB Host + pilote CCID minimal, diagnostic du port
    apdu_t0.h                    couche APDU (T=0)
    vitale.h                     ATR, sequence Vitale 2, BER-TLV, resultat
    status_led.h                 LED d'etat WS2812
    tools/pcsc_term.py           terminal de test (pyserial)
    doc/                         documentation (ce sommaire)
    Ressources/                  cahier des charges initial
```

## État

| Statut | Points |
|---|---|
| Validé sur matériel le 2026-10-14 | protocole, détection du lecteur Gemalto GemPC Twin (branchement, débranchement, recherche toutes les 30 s), reconnaissance d'une Vitale 1, refus d'une carte bancaire (`NOT_CV:6A82`) |
| **Reste à valider** | lecture d'une **Carte Vitale 2**, lecteur Identiv CLOUD 2700 R. Voir [doc/recette.md](doc/recette.md) |
