# Compilation et flash

## En bref

```bat
S3\pcsc_firmware_compile.bat          REM compile -> S3\firmware\pcsc_firmware_vYYMM.dd\
S3\pcsc_firmware_flash.bat [COMx]     REM televerse la derniere version (port auto-detecte)
```

Emplacements imposés par la demande initiale :

| Quoi | Où |
|---|---|
| Sources du firmware | `S3\pcsc_firmware\` |
| Scripts `.bat` | `S3\` |
| Versions compilées | `S3\firmware\pcsc_firmware_vYYMM.dd\` |

Les `.bin` sont ignorés par git (`*.bin`) et le dossier de compilation intermédiaire
est `S3\build\pcsc_firmware\` (ignoré, `build/`).

## Chaîne d'outils

- **arduino-cli** fourni par l'Arduino IDE :
  `%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`.
  Il n'est pas sur le PATH. Les scripts le cherchent dans `ARDUINO_CLI`, puis dans
  le PATH, puis à cet emplacement.
- Cœur **esp32:esp32 3.3.11** (ESP-IDF 5.5.5). La pile USB Host (`usb/usb_host.h`,
  `libusb.a`) y est précompilée, avec la gestion des hubs et le filtre
  d'énumération. **Aucune bibliothèque externe** n'est nécessaire.

## FQBN

```
esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,CPUFreq=80,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled,DebugLevel=none
```

| Option | Raison |
|---|---|
| `USBMode=default` + `CDCOnBoot=default` | le cœur ne démarre rien sur l'USB natif. Le sketch n'appelle **jamais** `USB.begin()` et ne référence ni `USB`, ni `USBSerial`, ni `HWCDCSerial` : le contrôleur OTG reste libre pour `usb_host_install()`. Le choix `USBMode=default` ou `hwcdc` est indifférent tant que CDC On Boot est désactivé |
| `CPUFreq=80` | sobriété ; pas en dessous, la PLL est nécessaire à l'USB |
| `FlashSize=8M`, `PartitionScheme=default_8MB` | N8R2 (8 Mo). **Ne pas flasher sur une SuperMini 4 Mo** |
| `PSRAM=disabled` | inutile ici |
| `DebugLevel=none` | UART0 est le port protocole. Les logs ESP-IDF sont en plus coupés au démarrage, puis re-tramés `DBG:…IDF:` en mode DEBUG |

Taille mesurée : environ 371 Ko d'application (11 % de la partition de 3 Mo) et
33 Ko de RAM statique.

## `pcsc_firmware_compile.bat`

1. Compile avec `--output-dir S3\build\pcsc_firmware`.
2. Publie trois fichiers dans `S3\firmware\pcsc_firmware_vYYMM.dd\` :
   - `pcsc_firmware_vYYMM.dd.bin` : application, à 0x10000 ;
   - `….bootloader.bin` : à 0x0 ;
   - `….partitions.bin` : à 0x8000.

Une deuxième compilation le même jour remplace la version du jour.

## `pcsc_firmware_flash.bat [COMx …] [dossier|fichier.bin]`

- **Firmware** : par défaut, le plus récent `pcsc_firmware_v*`. Il est copié dans
  `%TEMP%\pcsc_flash`, parce que le flasher du cœur écrit des `*_flashed.bin` à
  côté des binaires.
- **Commande** : `arduino-cli upload --input-file …`, avec le même FQBN. Le
  bootloader, la table de partitions et l'application sont écrits ; la NVS ne l'est
  pas (ce firmware ne s'en sert pas).
- **Détection du port** : uniquement les **ponts USB-série**, filtrés par VID **et**
  PID :

  | Puce | VID | PID |
  |---|---|---|
  | WCH CH340 / CH341 / CH342-CH344 / CH9102 | 1A86 | 7522, 7523, 5523, 55D2–55D9 |
  | Silicon Labs CP210x | 10C4 | EA60, EA70 |
  | FTDI | 0403 | 6001, 6010, 6011, 6014, 6015 |

  Le filtre écarte :
  - les ports Espressif (VID 303A), c'est-à-dire les modules S3-KBD du firmware
    HID, qu'il ne faut pas écraser par erreur ;
  - **COM3**, un écran USB à puce WCH (PID 5722) que le seul VID aurait laissé passer.
- **Plusieurs ponts branchés** : un menu propose les ports (`T` = tous). Sinon,
  forcer le port : `pcsc_firmware_flash.bat COM12`.
- **Redémarrage** : la carte repart seule (auto-reset par RTS du CH343). Sinon,
  appuyer sur RESET.

Si le téléversement échoue, mettre la carte en mode BOOT (maintenir BOOT, appuyer
puis relâcher RESET, relâcher BOOT), puis relancer.

## Vérification après flash

```bat
python S3\pcsc_firmware\tools\pcsc_term.py COM12 INFO IS_READER
```

On attend :
- `CARD READER v1.0` au démarrage, précédé du log ROM (`~~`) ;
- puis `READER_OK` ou `READER_KO` ;
- puis la réponse à `INFO`.
