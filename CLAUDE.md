# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> Projet francophone : le code, les commentaires et la doc sont en français.
> Statut global : **mock / POC, sans aucune sécurité** (ni auth du site, ni
> appairage BLE chiffré). La sécurité est reportée en phase ultérieure (socle §8).

## Ce qu'est le projet

Chaîne de bout en bout en deux lots reliés par **un seul contrat d'interface** (le GATT) :

```
Site web (Web Bluetooth) --BLE/JSON--> ESP32-S3 (firmware) --USB HID--> machine cible
   LOT WEB/                              LOT S3/                          PC / TV / borne
```

Le périphérique **est** un injecteur de frappes (BadUSB) : ne le brancher que sur
des machines de confiance tant que la phase sécurité n'est pas faite.

## Commandes

Pas de système de build unifié, pas de framework de tests : la validation est **manuelle**.

### Firmware (`S3/hid_firmware/`) — Arduino-ESP32

Le point critique du build est le **FQBN** : `USBMode=default` correspond à
« USB-OTG (TinyUSB) », **indispensable au HID**. La valeur par défaut de la carte
est `hwcdc`, qui n'expose **pas** de HID.

Depuis l'ajout du transport Wi-Fi, le FQBN inclut aussi `FlashSize=4M`,
`PartitionScheme=huge_app` (l'app par défaut ~1,3 Mo ne suffit plus) et
`PSRAM=enabled`. **Avant de compiler, régénérer l'app embarquée** :

```bash
python S3/hid_firmware/tools/gen_web_assets.py   # écrit S3/hid_firmware/web_assets.h
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled S3/hid_firmware
arduino-cli upload  --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled -p COM7 S3/hid_firmware
```

- Prérequis : cœur **esp32 ≥ 2.0.14**, bibliothèque **ArduinoJson v7**,
  bibliothèque **WebSockets** (Markus Sattler / Links2004) pour le transport Wi-Fi.
- Via l'IDE : Board « ESP32S3 Dev Module », **USB Mode = USB-OTG (TinyUSB)**,
  USB CDC On Boot = Enabled, Upload Mode = UART0 / Hardware CDC.
- **Après l'upload : appuyer sur RESET** — sinon la carte peut rester en ROM
  (« USB JTAG/serial debug unit ») sans démarrer le firmware.
- Console série : nouveau port COM à 115200 bauds après RESET.
- Test sans le site : client BLE générique (nRF Connect), écrire du JSON sur CMD,
  s'abonner à STATUS. Exemples dans `S3/hid_firmware/README.md`.

### Site web (`WEB/`) — page statique, aucune dépendance ni build

Web Bluetooth exige un **contexte sécurisé** (HTTPS ou `localhost` ; `file://` refusé).

```bash
# Sur ce PC (localhost = contexte sécurisé)
cd WEB/Keyboard && python -m http.server 8000   # http://localhost:8000
```

```bash
# Depuis un téléphone Android (HTTPS requis, port 8443, certif auto-signé)
serve.bat            # ou : python serve.py [port]
```

`serve.py` génère un certificat auto-signé dans `WEB/ssl/` au 1ᵉʳ lancement (SAN =
localhost + IP locales), sert `WEB/` en HTTPS et interdit l'accès à `/ssl`.
Navigateurs : **Chrome/Edge** (Android ou desktop). **Exclus** : iOS/iPadOS (tous
navigateurs), Firefox, Safari. Brave a Web Bluetooth désactivé par défaut.

## Architecture (la vue d'ensemble)

**Le contrat GATT est la seule frontière entre les deux lots.** Toute modification
du protocole doit rester synchronisée entre `S3/hid_firmware/hid_firmware.ino` et
`WEB/Keyboard/app.js` (mêmes UUIDs, même schéma JSON).

**Second transport (Wi-Fi, résolution A).** À côté du BLE, le S3 expose un SoftAP
`S3-KBD` + portail captif et sert l'app en HTTP ; les commandes passent par un
**WebSocket** `ws://192.168.4.1:81/` où **1 frame texte = 1 JSON identique au BLE**,
le statut revient en frames texte (`ready`/`busy`/`err:*`). L'app choisit le
transport au chargement : Wi-Fi si le paramètre d'URL `D` (base64url de
`{"wifi":"192.168.4.1",…}`) est présent, sinon BLE. Firmware : `wifi_portal.h` +
`web_assets.h` (app embarquée). Web : façade `bleTransport`/`wifiTransport` dans
`app.js`, seul `send()` touche le transport. Le cœur (file, séquenceur, AZERTY,
USB) est partagé — les deux transports enfilent dans `g_cmdQueue`.

| Élément | UUID | Propriétés |
|---|---|---|
| Service HID-Bridge | `9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001` | — |
| CMD | `9f1d0001-…-1001` | Write / Write NR |
| STATUS | `9f1d0002-…-1001` | Notify (`ready` / `busy` / `err:…`) |

Commandes JSON UTF-8 sur CMD, champ `t` : `char` (un caractère, mode direct),
`txt` (chaîne / macro), `key` (touche nommée / média, action `tap`/`down`/`up`),
`seq` (séquence prédéfinie `n`, personnalisée `s[]`, ou `stop`), `mouse`
(déplacement `dx/dy`, molette `w`, bouton `b`+`a`). Masque modificateurs `m` :
bit0=Ctrl, 1=Shift, 2=Alt, 3=GUI, 4=AltGr.

### Invariants à ne pas casser

- **La traduction AZERTY→HID est ENTIÈREMENT dans le firmware** (piège n°1 du
  socle). Le web n'envoie que l'**intention** (`{"t":"char","v":"é"}`,
  `{"t":"key","c":"Enter"}`), **jamais** de code de scan. Toute la table de
  disposition est dans `S3/hid_firmware/keymap_azerty.h` — **pièce critique**, à
  tester en premier (un caractère absent → ignoré + `err:unmapped`, jamais de
  frappe au hasard). Les accents (circonflexe/tréma) sont des touches mortes = 2 frappes.
- **Le tempo des séquences est côté firmware** (horloge locale FreeRTOS), pour que
  la latence BLE ne déforme pas les délais.
- **Un seul thread touche l'USB HID** : la tâche `workerTask` consomme une file
  de commandes (`g_cmdQueue`). Les callbacks BLE ne font qu'enfiler. À la
  déconnexion, la file est vidée et `releaseAll()` est appelé (ne jamais laisser
  une touche collée).
- **STOP est hors-file** : détecté dans le callback d'écriture, il vide la file et
  interrompt immédiatement la séquence en cours (`g_stop`) au lieu d'attendre son
  tour.

### Firmware — fichiers

- `hid_firmware.ino` : orchestration (USB HID composite clavier+consumer+souris,
  serveur BLE Bluedroid, parseur ArduinoJson, dispatch, séquenceur, file/worker).
- `keymap_azerty.h` : table Unicode → frappe(s) HID (voir invariant ci-dessus).
- `status_led.h` : indicateur LED RGB WS2812 (GPIO48) dans une tâche dédiée à
  ~50 Hz. Le reste du code déclare un **état** (`ledSetMode`/`ledSetError`) ou une
  **impulsion** brève (`ledPulse`) ; il ne pilote jamais la LED directement.

### Web — fichiers (`WEB/Keyboard/`)

Client Web Bluetooth pensé **smartphone Android en paysage**. `app.js` définit les
onglets (AZERTY, Num, Fn/Média, Souris, Texte, Macro, Séq.), les **modificateurs
collants** (clic = one-shot armé, 2ᵉ = verrou, 3ᵉ = off), le garde-fou MTU
(`MTU_GUARD` 500 o) et le **journal** (chaque JSON émis + chaque STATUS reçu),
outil de validation. `WEB/index.html` redirige vers `Keyboard/`.

## Références

Les cahiers des charges sont dans `ressources/` : `cahier-des-charges_1.md`
(socle : vision, contrat GATT §5, pièges §7, hors-périmètre sécurité §8),
`_2.md` (lot S3), `_3.md` (lot WEB). La doc carte est dans `ressources/ESP32/`.
Le code C de référence (S3 SuperMini) est dans `ressources/ESP32/S3 Supermini/`.
