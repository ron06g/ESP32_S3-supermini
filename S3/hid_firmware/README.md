# Lot S3 — Firmware « HID-Bridge » (mock)

Firmware ESP32-S3 qui est **en même temps** un clavier USB HID (AZERTY, +touches
média) et un serveur BLE recevant les commandes du site web.

> Réalisation du contrat GATT du socle (§5) et de la table AZERTY (spec S3 §6).
> **Mock, aucune sécurité** — voir l'avertissement BadUSB plus bas.

## Fichiers

| Fichier | Rôle |
|---|---|
| `hid_firmware.ino` | Orchestration : USB HID, serveur BLE, parseur JSON, séquenceur |
| `keymap_azerty.h` | **Table AZERTY** (caractère Unicode → touche HID + modificateurs) — pièce critique |

## Choix d'implémentation

Le socle décrit une cible ESP-IDF (TinyUSB/NimBLE/cJSON) *à titre indicatif*. Ce
mock est réalisé sous **Arduino-ESP32**, plus rapide à valider et aligné sur le
code de référence fourni. Les équivalents utilisés :

- USB HID composite (clavier + Consumer Control + souris) → `USBHIDKeyboard` +
  `USBHIDConsumerControl` + `USBHIDMouse` (TinyUSB) ;
- serveur GATT → pile BLE intégrée (`BLEDevice.h`, Bluedroid) ;
- parseur JSON → **ArduinoJson v7** ;
- séquenceur autonome → tâche FreeRTOS + file de commandes (horloge locale).

## Prérequis

1. **Arduino IDE** (ou arduino-cli) avec le **cœur esp32 ≥ 2.0.14**
   (Gestionnaire de cartes → « esp32 » par Espressif).
2. Bibliothèque **ArduinoJson** (v7) via *Croquis → Inclure une bibliothèque →
   Gérer les bibliothèques → « ArduinoJson »*.

## Réglages carte (menu *Outils*)

| Réglage | Valeur |
|---|---|
| Board | **ESP32S3 Dev Module** |
| **USB Mode** | **USB-OTG (TinyUSB)** ← indispensable pour le HID |
| USB CDC On Boot | **Enabled** (logs `Serial` sur l'USB natif) |
| Upload Mode | UART0 / Hardware CDC |
| Flash Size | selon la carte (SuperMini N8R2 = 8 MB) |
| PSRAM | selon la carte (N8R2 → *QSPI PSRAM*) |

> **SuperMini mono-USB** : le port sert de HID vers la cible ; le flash et les
> logs passent par le **même** port (CDC/JTAG) ou par le **mode BOOT** (maintenir
> BOOT, appuyer/relâcher RESET, relâcher BOOT, puis téléverser). Une carte
> double-USB garde un port libre pour la console — plus confortable.

## Compiler / téléverser

- **Arduino IDE** : ouvrir `hid_firmware.ino`, choisir le port, *Téléverser*.
- **arduino-cli** (procédure vérifiée) — l'IDE fournit un `arduino-cli` dans
  `…\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe` :

```bash
# FQBN : USBMode=default = « USB-OTG (TinyUSB) » (INDISPENSABLE au HID).
# Attention : la valeur par défaut de la carte est hwcdc, qui n'a PAS de HID.
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc S3/hid_firmware
arduino-cli upload  --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc -p COM7 S3/hid_firmware
```

> **Après l'upload : appuyer sur RESET.** Sur ces cartes, le « hard reset via RTS »
> qui suit l'upload ne relance pas toujours l'application ; la carte reste alors en
> ROM (on voit *USB JTAG/serial debug unit*). Un appui sur **RESET** fait démarrer
> le firmware, qui bascule l'USB natif en TinyUSB : apparaissent alors un **clavier
> HID**, un **contrôle consommateur HID** et un **port série CDC** (nouveau COM).

Au démarrage, le moniteur série (nouveau COM, 115200) affiche l'annonce BLE
`S3-KBD` et l'état. Le PID énuméré est `1001` (dérivé par le cœur selon les
interfaces) : sans importance pour le mock.

## Tester le lot seul (sans le site web)

Avec un client BLE générique (**nRF Connect**, LightBlue…) :

1. Se connecter à `S3-KBD`, ouvrir le service `9f1d0000-…-1001`.
2. Écrire sur **CMD** (`…-0001`) des commandes JSON (en octets/UTF-8) :
   - `{"t":"char","v":"a"}` → tape **a** sur une cible FR (si **q** apparaît, la
     cible n'est pas en AZERTY : c'est le piège n°1).
   - `{"t":"txt","v":"café à 5 €"}` → reproduit la chaîne accentuée.
   - `{"t":"key","c":"Enter","a":"tap"}` → valide une ligne.
   - `{"t":"char","v":"c","m":1}` → **Ctrl+C**.
   - Séquence tempo (socle §5.3) : `{"t":"seq","s":[{"tap":"1"},{"wait":1000},{"tap":"2"},{"wait":1000},{"rep":3,"every":4000,"tap":"3"}]}`.
   - Arrêt : `{"t":"seq","n":"stop"}`.
3. S'abonner à **STATUS** (`…-0002`) pour voir `ready` / `busy` / `err:…`.

## Contrat GATT (rappel)

| Élément | UUID |
|---|---|
| Service HID-Bridge | `9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001` |
| CMD (Write / Write NR) | `9f1d0001-5b8e-4a4a-9c2a-2b7f3e6a1001` |
| STATUS (Notify) | `9f1d0002-5b8e-4a4a-9c2a-2b7f3e6a1001` |

Format des commandes : **JSON UTF-8** (socle §5.2). La traduction AZERTY→HID est
**entièrement côté firmware** ; le web n'envoie que l'intention (`char`/`key`).

## Limites connues (mock)

- **Protocole boot / BIOS** : la classe `USBHIDKeyboard` fournit le rapport clavier
  standard 8 octets et fonctionne sous OS. Le fonctionnement *dès le BIOS/UEFI*
  (recette §6.1) dépend de l'hôte ; si un firmware de carte-mère l'exige
  strictement, il faudra un descripteur TinyUSB avec sous-classe *boot* explicite.
- **Bluedroid + USB** cohabitent mais sont gourmands en RAM ; en cas d'instabilité
  mémoire, basculer la pile BLE sur **NimBLE-Arduino** (plus légère, recommandée
  par le socle) est l'évolution naturelle.
- Une seule connexion cliente à la fois (conforme au mock).
- Majuscules accentuées et touches mortes exotiques hors couverture (spec S3 §6) :
  caractère absent de la table → ignoré + `err:unmapped`, jamais de frappe au hasard.

## ⚠️ BadUSB

Ce périphérique **est** un injecteur de frappes. Tant que la phase sécurité n'est
pas faite (appairage chiffré, liste blanche, authentification du site), ne le
brancher **que sur des machines de confiance**.
