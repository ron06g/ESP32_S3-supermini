# Lot S3 — Firmware « HID-Bridge » (mock)

Firmware ESP32-S3 qui est **en même temps** un clavier USB HID (AZERTY, +touches
média) et un serveur BLE recevant les commandes du site web.

> Réalisation du contrat GATT du socle (§5) et de la table AZERTY (spec S3 §6).
> **Mock, aucune sécurité** — voir l'avertissement BadUSB plus bas.

## Fichiers

| Fichier | Rôle |
|---|---|
| `hid_firmware.ino` | Orchestration : USB HID, serveur BLE, **transport Wi-Fi**, parseur JSON, séquenceur |
| `keymap_azerty.h` | **Table AZERTY** (caractère Unicode → touche HID + modificateurs) — pièce critique |
| `status_led.h` | Indicateur LED RGB WS2812 (tâche dédiée) |
| `wifi_portal.h` | **Transport Wi-Fi** : SoftAP + portail captif + serveur HTTP + WebSocket |
| `config.h` | **Paramètres persistants** (NVS / `Preferences`) : flags HID clavier / souris / COM / GPIO / appairage, rôle, MAC du pair |
| `com_port.h` | **Port COM** : réutilise l'unique CDC (interface 0) en mode protocole (1 ligne JSON = 1 commande, 1 STATUS = 1 ligne). Pas de 2ᵉ CDC (budget d'endpoints S3) |
| `gpio_panel.h` | **Panneau GPIO** : table `GPIO_TABLE[]` (BOOT + sorties 4–7 + entrées 8–11), scrutation anti-rebond |
| `ble_link.h` | **Appairage BLE ↔ BLE** : scan, bind/unbind, tâche `link` (le maître est client GATT de l'esclave) |
| `web_assets.h` | App `WEB/Keyboard/` embarquée (gzip, **généré** — ne pas éditer à la main) |
| `tools/gen_web_assets.py` | Génère `web_assets.h` depuis `WEB/Keyboard/` |

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
3. Bibliothèque **WebSockets** de *Markus Sattler* (dépôt Links2004) — Gestionnaire
   de bibliothèques → « WebSockets ». (`WiFi` / `WebServer` / `DNSServer` sont
   fournis par le cœur esp32, rien à installer.)

## Réglages carte (menu *Outils*)

| Réglage | Valeur |
|---|---|
| Board | **ESP32S3 Dev Module** |
| **USB Mode** | **USB-OTG (TinyUSB)** ← indispensable pour le HID |
| USB CDC On Boot | **Disabled** — le sketch crée lui-même la console USB (`Console`, CDC 0) et appelle `USB.begin()` **après** avoir construit les interfaces choisies en NVS. Avec *Enabled*, le cœur appelle `USB.begin()` avant `setup()` : plus aucun HID |
| Upload Mode | UART0 / Hardware CDC |
| Flash Size | **4 MB** — carte en main = SuperMini **N4R2** ; `FlashSize=4M`. ⚠️ `8M` fait **boucler le boot** (`spi_flash: Detected size(4096k) smaller than … header(8192k)`) |
| Partition Scheme | **Huge APP (3 MB No OTA / 1 MB SPIFFS)** — requis pour loger BLE + Wi-Fi + USB + app (tient dans 4 Mo) |
| PSRAM | **QSPI PSRAM** (N4R2, `PSRAM=enabled`) — utile pour la RAM avec le Wi-Fi |

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
# CDCOnBoot=default (= Disabled) : INDISPENSABLE aussi (interfaces USB choisies en NVS).
# Attention : la valeur par défaut de la carte est hwcdc, qui n'a PAS de HID.
# Depuis l'ajout du Wi-Fi : FlashSize=4M + PartitionScheme=huge_app (l'app par
# défaut ~1,3 Mo ne suffit plus) + PSRAM=enabled (déporte les allocs WiFi/LWIP).
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled S3/hid_firmware
arduino-cli upload  --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled -p COM7 S3/hid_firmware
```

> **Le plus simple (Windows)** : `S3\hid_firmware_compile.bat` (régénère `web_assets.h`
> puis compile) et `S3\hid_firmware_flash.bat [COMx]` (compile + téléverse, port
> auto-détecté). Ils localisent l'arduino-cli de l'IDE et ciblent `FlashSize=4M`.

> **Après l'upload : appuyer sur RESET.** Sur ces cartes, le « hard reset via RTS »
> qui suit l'upload ne relance pas toujours l'application ; la carte reste alors en
> ROM (on voit *USB JTAG/serial debug unit*). Un appui sur **RESET** fait démarrer
> le firmware, qui bascule l'USB natif en TinyUSB : apparaissent alors un **clavier
> HID**, un **contrôle consommateur HID** et un **port série CDC** (nouveau COM).

Au démarrage, le moniteur série (nouveau COM, 115200) affiche l'annonce BLE
`S3-KBD` et l'état. Le PID énuméré est `1001` (dérivé par le cœur selon les
interfaces) : sans importance pour le mock.

## Transport Wi-Fi (résolution A) — évolution

Second transport, à côté du BLE, compatible **iPhone** (cf.
`ressources/cahier-des-charges_4.md`). Le S3 ouvre un **SoftAP** `S3-KBD` (WPA2,
clé ≥ 8 car., IP fixe `192.168.4.1`, sans internet), répond aux sondes de
**portail captif** et **sert lui-même l'app web** ; les commandes passent par un
**WebSocket** (`ws://192.168.4.1:81/`) rejouant le **même protocole JSON** qu'en
BLE. Le cœur (table AZERTY, séquenceur, USB HID) est inchangé : les deux
transports enfilent dans la même file, un seul thread touche l'USB.

**Avant chaque compilation, (re)générer l'app embarquée** depuis `WEB/Keyboard/` :

```bash
python S3/hid_firmware/tools/gen_web_assets.py   # écrit web_assets.h (gzip + routage)
```

Test depuis un téléphone : rejoindre le réseau **`S3-KBD`** (clé `apikey00` par
défaut — **mock, à changer**), accepter le portail captif ; la télécommande
s'ouvre en Wi-Fi sur `http://192.168.4.1/Keyboard/`. Le BLE reste utilisable, sans
redémarrage. Contrainte de plateforme : une page **HTTPS** ne pourrait pas piloter
`ws://192.168.4.1` (contenu mixte) — d'où l'app servie **en HTTP par le S3**
(résolution A), seule option compatible iOS et hors-ligne.

## Paramètres, port COM, GPIO, appairage — évolution

Tout est piloté par le **même protocole JSON** (BLE, WebSocket ou port COM). Depuis
l'étoile multi-esclaves, **le STATUS est en JSON** (`{"id":n,…}`, `id` 0 = maître,
1..3 = esclave) et chaque commande accepte un `id` optionnel de **routage** (0 =
maître/local par défaut) :

| Commande | Effet | Réponse STATUS |
|---|---|---|
| `{"t":"ping","n":12}` / `…,"id":1}` | test de liaison vers le maître / l'esclave 1 | `{"id":0,"ev":"pong","n":12}` |
| `{"t":"cfg","a":"get"}` | lire la config + table des esclaves | `{"id":0,"ev":"cfg","hid_kb":1,…,"role":1,"mac":"…","self":0,"slaves":[{"id":1,"mac":"…","up":1,"rssi":-62}]}` |
| `{"t":"cfg","a":"set","hid_kb":true,…,"gpio":true,"pair":true}` | sauver en NVS puis **redémarrer** | `{"id":0,"ev":"cfg","saved":true}` |
| `{"t":"pair","a":"scan"}` | scan BLE 4 s des modules HID-Bridge | `{"id":0,"ev":"scan","mac":"…","rssi":-60,"name":"…"}` ×N puis `{"…,"done":true}` |
| `{"t":"pair","a":"bind","mac":"aa:bb:…"}` | **ajouter** ce module comme esclave (id auto, plus petit libre 1..3) | `{"id":0,"ev":"pair","ok":true}` + reboot, sinon `err:pair`/`err:full` |
| `{"t":"pair","a":"slave","mac":"<maître>","id":n}` | (reçu du maître) devenir **esclave** id `n` | `pair` ok + reboot |
| `{"t":"pair","a":"unbind","id":n}` | (maître) libérer l'esclave `n` (`id`=0 ou absent = **tous**) | `pair` ok + reboot |
| `{"t":"pair","a":"reset"}` | **retour au mode standard** — à envoyer sur le **port COM** d'un esclave orphelin | `pair` ok + reboot |
| **5 appuis sur BOOT** (< 3 s) | désappairage physique : maître → toute l'étoile, esclave → `reset` | `pair` ok + reboot |
| `{"t":"gpio","p":"4","a":"tgl","id":1}` / `{"t":"gpio","a":"read","id":0}` | sortie/lecture des GPIO de la carte `id` (`read` sans `p` = tout) | `{"id":1,"ev":"gpio","p":"4","v":1}` (aussi spontané sur entrée) |
| — | événements du lien (maître, par esclave) | `{"id":1,"ev":"link","up":true}`, `{"…,"up":false}`, `{"…,"rssi":-62}` (2 s) |

Erreurs : `{"id":n,"err":"nolink"}` (esclave `n` injoignable), `err:id` (id inconnu),
`err:full` (table pleine), `err:nohid`, `err:gpio`, `err:pair`. Frames STATUS ≤ **384 o**.

**Désappairage physique** : **5 appuis sur BOOT** (GPIO0) en moins de 3 s (tâche
`bootResetTask` autonome, chaque appui = impulsion LED violette) — un maître désappaire
**toute l'étoile**, un esclave fait `reset`.

**Rôles** (persistants en NVS) :

- **standard** : BLE + Wi-Fi + USB local (id 0) ;
- **maître** : injecte **localement** clavier/souris/COM + ses propres GPIO (id 0), garde
  BLE + Wi-Fi pour le téléphone, et **route par `id`** vers ses esclaves (une tâche `linkN`
  par esclave : reconnexion 3 s, RSSI 2 s ; tâche `relay` rediffuse leurs STATUS). STOP est
  diffusé hors-file à tous les esclaves. Total connexions BLE ≤ 3 (piloter par COM/Wi-Fi) ;
- **esclave** : **GPIO uniquement** (+ COM). Pas de Wi-Fi, BLE réservé à son maître
  (whitelist + vérif MAC), se signale avec son `selfId`. LED : ambre qui respire (sans
  maître), vert doux (lié).

**Port COM** : l'ESP32-S3 (USB-OTG FS, 6 endpoints) ne peut PAS exposer clavier + souris +
**deux** CDC — le composite est refusé par l'hôte (Windows code 10). Le port COM **réutilise
donc l'unique CDC** : quand `serial` est actif, ce CDC parle le protocole (1 ligne = 1 commande,
STATUS en lignes) et les logs de debug sont tus pour garder le flux propre ; quand `serial` est
inactif, le même CDC est la **console** de debug. Vitesse nominale (USB CDC l'ignore : 9600 8N1
côté hôte convient). Le **numéro de série USB** dérive des flags (`S3KBD-KMS`, `x` = désactivé)
pour que Windows ré-énumère proprement chaque combinaison.

**GPIO** : `gpio_panel.h`, table unique `GPIO_TABLE[]` — `BOOT` (GPIO0, bouton intégré),
sorties `4 5 6 7`, entrées `8 9 10 11` (pull-up, **1 = actif = niveau bas**). Le **maître**
scrute désormais **ses propres** GPIO (id 0) ; ceux d'un esclave s'adressent par `id`.

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
3. S'abonner à **STATUS** (`…-0002`) : les frames sont du **JSON**, ex.
   `{"id":0,"st":"ready"}`, `{"id":0,"st":"busy"}`, `{"id":0,"err":"unmapped"}`.
4. Nouveautés : `{"t":"ping","n":1}` → `{"id":0,"ev":"pong","n":1}` ; `{"t":"cfg","a":"get"}`
   → `{"id":0,"ev":"cfg",…}` ; `{"t":"gpio","p":"4","a":"tgl"}` → `{"id":0,"ev":"gpio","p":"4","v":1}`.
   Routage étoile : ajouter `"id":1` pour viser l'esclave 1. Sur le **port COM** (PuTTY, une
   ligne par commande) : `{"t":"pair","a":"reset"}` libère un esclave.

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
- **Total des connexions BLE simultanées ≤ 3** (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS`) :
  un maître à 3 esclaves n'a plus de connexion libre pour un téléphone → piloter le maître
  par **COM ou Wi-Fi** (qui ne consomment pas de connexion BLE). Un module déjà connecté à un
  téléphone n'annonce plus : il n'apparaît pas dans un scan d'appairage.
- Les frames STATUS font jusqu'à **384 octets** (le `cfg` embarque la table `slaves[]`) :
  un MTU BLE élevé est requis (Android/Chrome, Windows et nRF Connect négocient 517 ; log `[BLE] MTU=`).
- Majuscules accentuées et touches mortes exotiques hors couverture (spec S3 §6) :
  caractère absent de la table → ignoré + `err:unmapped`, jamais de frappe au hasard.

## ⚠️ BadUSB

Ce périphérique **est** un injecteur de frappes. Tant que la phase sécurité n'est
pas faite (appairage chiffré, liste blanche, authentification du site), ne le
brancher **que sur des machines de confiance**.
