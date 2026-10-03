# Lot S3 — Firmware « HID-Bridge »

Firmware ESP32-S3 qui est **en même temps** un clavier USB HID (AZERTY, +touches
média) et un serveur BLE recevant les commandes du site web.

> Réalisation du contrat GATT du socle (§5) et de la table AZERTY (spec S3 §6).
> Liaisons BLE chiffrées (LESC) + authentification MITM — voir la section Sécurité plus bas.

## Fichiers

| Fichier | Rôle |
|---|---|
| `hid_firmware.ino` | Orchestration : USB HID, serveur BLE, **transport Wi-Fi**, parseur JSON, séquenceur |
| `keymap_azerty.h` | **Table AZERTY** (caractère Unicode → touche HID + modificateurs) — pièce critique |
| `status_led.h` | Indicateur LED RGB WS2812 (tâche dédiée) |
| `wifi_portal.h` | **Transport Wi-Fi** : SoftAP + portail captif + serveur HTTP + WebSocket |
| `config.h` | **Paramètres persistants** (NVS / `Preferences`) : flags HID clavier / souris / COM / GPIO / appairage, rôle, MAC du pair, **passkey LESC**, **clé WPA2**, **nom convivial** du module + des esclaves, **fréquence CPU** et **puissances BLE / Wi-Fi** |
| `board.h` | **Profils de carte** : détection du modèle au démarrage par les eFuses (flash intégrée = SuperMini, module WROOM = DevKit) → broche LED + GPIO exposés. Un seul binaire pour toutes les cartes |
| `autoboot.h` | **Macro autoboot** : macro reçue en morceaux, gardée en NVS, enfilée une fois par démarrage après le montage USB + délai |
| `com_port.h` | **Port COM** : réutilise l'unique CDC (interface 0) en mode protocole (1 ligne JSON = 1 commande, 1 STATUS = 1 ligne). Pas de 2ᵉ CDC (budget d'endpoints S3) |
| `gpio_panel.h` | **Panneau GPIO** : table `GPIO_TABLE[]` construite au démarrage depuis le profil de carte (repères logiques `o1`…, `i1`…, + `BOOT`), scrutation anti-rebond, **effets autonomes** des sorties (tâche `gpiofx` : clignotement `loop`, PWM matériel `pwm`) |
| `ble_link.h` | **Appairage BLE ↔ BLE (chiffré LESC)** : scan, bind (bootstrap Just Works + provisioning sur PROV), unbind, tâche `link` (client GATT ; `secureConnection()` avant tout `writeValue`) |
| `web_assets.h` | App `WEB/Keyboard/` embarquée (gzip, **généré** — ne pas éditer à la main) |
| `tools/gen_web_assets.py` | Génère `web_assets.h` depuis `WEB/Keyboard/` |

## Choix d'implémentation

Le socle décrit une cible ESP-IDF (TinyUSB/NimBLE/cJSON) *à titre indicatif*. Ce
Ce firmware est réalisé sous **Arduino-ESP32**, plus rapide à valider et aligné sur le
code de référence fourni. Les équivalents utilisés :

- USB HID composite (clavier + Consumer Control + souris) → `USBHIDKeyboard` +
  `USBHIDConsumerControl` + `USBHIDMouse` (TinyUSB) ;
- serveur GATT → pile BLE intégrée (`BLEDevice.h`) ; le cœur esp32 3.x la construit
  sur **NimBLE** (sécurité LESC via `BLESecurity`) ;
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
| Flash Size | **4 MB** — `FlashSize=4M` pour **toutes** les cartes : SuperMini (4 Mo) et DevKit N8R2 (8 Mo, qui n'en utilise que 4). ⚠️ `8M` sur une SuperMini fait **boucler le boot** (`spi_flash: Detected size(4096k) smaller than … header(8192k)`) |
| Partition Scheme | **Huge APP (3 MB No OTA / 1 MB SPIFFS)** — requis pour loger BLE + Wi-Fi + USB + app (tient dans 4 Mo) |
| PSRAM | **QSPI PSRAM** (`PSRAM=enabled`, SuperMini et N8R2) — utile pour la RAM avec le Wi-Fi. Une N16R8 (PSRAM octale) demanderait `opi` |

> **SuperMini mono-USB** : le port sert de HID vers la cible ; le flash et les
> logs passent par le **même** port (CDC/JTAG) ou par le **mode BOOT** (maintenir
> BOOT, appuyer/relâcher RESET, relâcher BOOT, puis téléverser). Une carte
> double-USB garde un port libre pour la console — plus confortable.
>
> **DevKit N8R2 (2 USB-C)** : même binaire. Prise **« USB »** (USB natif) = HID vers la
> cible ; prise **« COM »** (pont USB-série sur UART0) = flash, reset automatique fiable.
> `hid_firmware_flash.bat` ne détecte que le VID Espressif `303A` : passer le port du
> pont en argument (`hid_firmware_flash.bat COM12`).

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

> **Le plus simple (Windows)** : `S3\hid_firmware_compile.bat` régénère `web_assets.h`
> (**bloquant** : sans Python 3 ou si la génération échoue, pas de compilation — le
> firmware n'embarque jamais un site captif périmé), compile et publie un sous-dossier par version, `S3\firmware\hid_firmware_vYYMM.dd\`,
> contenant `hid_firmware_vYYMM.dd.bin` + `.bootloader.bin` + `.partitions.bin` (une
> recompilation le même jour remplace la version du jour).
> `S3\hid_firmware_flash.bat [COMx ...] [dossier|fichier.bin]` téléverse, **sans
> recompiler**, la version la plus récente sur le port auto-détecté (USB VID Espressif
> `303A`, en firmware comme en ROM ; plusieurs modules → menu, `T` = tous). Il écrit
> bootloader + partitions + app : la **NVS est conservée** (réglages, appairages). Il
> flashe depuis une copie dans `%TEMP%` (le wrapper `flasher` du cœur y dépose ses
> `*_flashed.bin`), `firmware\` reste intact. Ils localisent l'arduino-cli de l'IDE et
> ciblent `FlashSize=4M`.

> **Après l'upload : appuyer sur RESET.** Sur ces cartes, le « hard reset via RTS »
> qui suit l'upload ne relance pas toujours l'application ; la carte reste alors en
> ROM (on voit *USB JTAG/serial debug unit*). Un appui sur **RESET** fait démarrer
> le firmware, qui bascule l'USB natif en TinyUSB : apparaissent alors un **clavier
> HID**, un **contrôle consommateur HID** et un **port série CDC** (nouveau COM).

Au démarrage, le moniteur série (nouveau COM, 115200) affiche l'annonce BLE
`S3-KBD` et l'état. Le PID énuméré est `1001` (dérivé par le cœur selon les
interfaces) : sans importance ici.

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

Test depuis un téléphone : rejoindre le réseau **`S3-KBD`** (clé `12345678` par
défaut, modifiable dans Réglages → Sécurité), accepter le portail captif ; la télécommande
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
| `{"t":"pair","a":"bind","mac":"aa:bb:…","name":"…"}` | **ajouter** ce module comme esclave (id auto 1..3 ; `name` = nom annoncé, mis en cache) | `{"id":0,"ev":"pair","ok":true}` + reboot, sinon `err:pair`/`err:full` |
| `{"t":"pair","a":"slave","mac":"<maître>","id":n,"pk":N}` | (reçu du maître **via PROV**) devenir **esclave** id `n`, enregistrer la passkey `pk` | `pair` ok + reboot |
| `{"t":"pair","a":"unbind","id":n}` | (maître) libérer l'esclave `n` (`id`=0 ou absent = **tous**) + effacer son bond | `pair` ok + reboot |
| `{"t":"pair","a":"reset"}` | **retour au mode standard** (passkey→`000000`, bonds effacés, **nom conservé**) — port COM d'un esclave orphelin | `pair` ok + reboot |
| **5 appuis sur BOOT** (< 3 s) | désappairage physique : maître → toute l'étoile, esclave → `reset` | `pair` ok + reboot |
| `{"t":"sec","a":"passkey","pk":"NNNNNN"}` | changer la passkey LESC (refusé si esclaves appairés) → efface les bonds | `{"id":0,"ev":"sec","ok":true}` + reboot, sinon `err:slaves`/`err:sec` |
| `{"t":"sec","a":"wifi","psk":"…"}` | changer la clé WPA2 du SoftAP (≥ 8 car.) | `sec` ok + reboot, sinon `err:sec` |
| `{"t":"sec","a":"get"}` | indicateurs sécurité (jamais les valeurs) | `{"id":0,"ev":"sec","pkset":0,"wifiset":0}` |
| `{"t":"name","name":"…"}` / `…,"id":n}` | renommer ce module (→ **reboot**, le nom est l'annonce BLE) / (maître) l'esclave `n` (routé, **pas de reboot** de l'esclave) | `{"id":n,"ev":"name","name":"…"[,"reboot":true]}` |
| `{"t":"gpio","p":"o1","a":"tgl","id":1}` / `{"t":"gpio","a":"read","id":0}` | sortie/lecture des GPIO de la carte `id` (`read` sans `p` = tout ; `clr` sans `p` = tout éteindre) | `{"id":1,"ev":"gpio","p":"o1","v":1}` (aussi spontané sur entrée) |
| `{"t":"gpio","p":"o1","a":"loop","t_set":200,"t_clr":800,"nb":5}` | clignotement **autonome** : `nb` cycles (0/absent = infini), phases 10 ms…24 h | départ `{…,"v":1,"fx":"loop"}`, fin `{…,"v":0,"fx":"loop","done":true}` |
| `{"t":"gpio","p":"o2","a":"pwm","duty":40,"t_pwm":5000,"hz":1000}` | PWM **matériel** (LEDC) : `duty` 0–100 %, `t_pwm` ms (0/absent = infini), `hz` 10–40000 (défaut 1000) | départ `{…,"fx":"pwm","duty":40}`, fin `{…,"done":true}` |
| `{"t":"sys","a":"get"}` / `…,"id":n}` | **modèle de carte** (profil + puce), GPIO exposés, CPU, puissances, température — **routé** par `id` | `{"id":0,"ev":"sys","b":"devkit","bn":"S3 DevKit","chip":"N8R2","cpu":160,"txb":9,"txba":1,"txw":11,"temp":41.5,"o":[…],"i":[…]}` |
| `{"t":"sys","a":"set","cpu":160,"txw":11,"txb":"auto"}` | CPU 80/160/240 MHz, Wi-Fi 2–20 dBm, BLE −24…+18 par 3 ou 20 dBm ou `"auto"` (0 standard / +9 appairé) → NVS + **reboot** | `{"id":n,"ev":"sys","saved":true}`, sinon `err:sys` |
| `{"t":"autoboot","a":"put","i":k,"n":N,"c":"…"[,"d":5,"name":"…"]}` | macro **autoboot** (JSON txt/seq/key/char sérialisé, en N morceaux ; `d`/`name` sur le dernier) — toujours **locale** | `{"id":0,"ev":"autoboot","put":k}` par morceau, puis `{"…","on":1,"d":5,"name":"…","len":L}` |
| `{"t":"autoboot","a":"get|clr|run"}` | état / effacer / essayer maintenant | `{"id":0,"ev":"autoboot","on":0|1,…}` ; au départ `{"…","run":"boot"|"run"}` |
| `{"t":"stop"}` | arrêt d'urgence **hors file** (séquence, file, touches) ; alias `{"t":"seq","n":"stop"}` | `{"id":0,"st":"ready"}` |
| — | événements du lien (maître, par esclave) | `{"id":1,"ev":"link","up":true}`, `{"…,"up":false}`, `{"…,"rssi":-62}` (2 s) |

Erreurs : `{"id":n,"err":"nolink"}` (esclave `n` injoignable), `err:id` (id inconnu),
`err:full` (table pleine), `err:slaves` (changement de passkey refusé : esclaves
appairés), `err:sec` (passkey/PSK invalide), `err:sys`, `err:autoboot`, `err:nohid`,
`err:gpio`, `err:pair`. Frames STATUS ≤ **512 o**. En **BLE**, une frame plus longue que
MTU − 3 (cfg d'un maître avec 3 esclaves nommés ≈ 470 o, MTU pas encore négocié…) est
**découpée** : morceaux préfixés par l'octet `0x1F`, dernier sans préfixe — le web recolle
les octets avant de décoder (`bleNotify`). Wi-Fi et COM ne sont pas concernés.

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
donc l'unique CDC** : il n'existe que si `serial` est actif et parle alors le protocole
(1 commande par ligne ou par trame STX…ETX, STATUS terminés CR LF) ; sinon aucun CDC n'est
exposé (console de debug supprimée). Vitesse nominale (USB CDC l'ignore : 9600 8N1
côté hôte convient). Le **numéro de série USB** dérive des flags (`S3KBD-KMS`, `x` = désactivé)
pour que Windows ré-énumère proprement chaque combinaison.

**Effets GPIO autonomes** : toute écriture sur une sortie (`set`/`clr`/`tgl`/`loop`/`pwm`)
interrompt l'effet en cours sur cette sortie ; `read` n'interrompt rien. Une seule tâche
`gpiofx` gère toutes les sorties : elle dort jusqu'à la prochaine échéance (2 réveils par
cycle de clignotement, aucun pendant un PWM, matériel). Les effets tournent sur la carte
visée (`id`) : seuls le départ et la fin produisent un STATUS.

**Port COM — tramage** : une commande se termine par CR, LF ou CRLF, **ou** s'encadre
`STX` (0x02) … `ETX` (0x03) (CR LF facultatifs après ETX ; STX jette tout reste partiel,
JSON multi-ligne accepté entre STX et ETX). Les réponses se terminent par CR LF et
reprennent le format de la dernière commande reçue. Détail : `WEB/Landing/protocole.html`.

**GPIO selon la carte** : `board.h` détecte le modèle au démarrage (eFuse `FLASH_CAP` :
flash intégrée = **SuperMini** ESP32-S3FH4R2, sinon module WROOM-1 = **DevKit**) et
`gpio_panel.h` construit `GPIO_TABLE[]` depuis son profil — `BOOT` (GPIO0, bouton intégré) +

| Repères | SuperMini | DevKit (N8R2) |
|---|---|---|
| sorties `o1`–`o4` | GPIO 4, 5, 6, 7 | GPIO 4, 5, 6, 7 |
| sorties `o5`–`o8` | — | GPIO 15, 16, 17, 18 |
| entrées `i1`–`i4` | GPIO 8, 9, 10, 11 | GPIO 8, 9, 10, 11 |
| entrées `i5`–`i8` | — | GPIO 12, 13, 14, 21 |

Entrées en pull-up, **1 = actif = niveau bas**. Repères **logiques** ; `o1`–`o4`/`i1`–`i4`
identiques partout. Nouvelle broche / nouvelle carte : la liste `out[]`/`in[]` du profil
(`board.h`) ; le web lit les broches de chaque carte via `sys` (rien à recopier, sauf le
repli `HW_DEFAULT` d'app.js pour un ancien firmware). On identifie la **puce**, pas le circuit
imprimé (une autre carte WROOM avec la LED ailleurs aurait le même profil). Le **maître**
scrute **ses propres** GPIO (id 0) ; ceux d'un esclave s'adressent par `id`.

**Autoboot** : la macro (≤ 2 Ko) est rangée en NVS (`abm`/`abd`/`abn`) ; au démarrage, une
tâche attend que la machine cible ait **monté** le clavier USB, puis le délai `d` (repris si
la cible se débranche), et enfile la macro **une fois** — même file que les commandes reçues
(STOP l'interrompt, le worker reste seul sur l'USB). Jamais sur un esclave ni sans clavier HID.
Effacée par le reset d'usine.

## Tester le lot seul (sans le site web)

Avec un client BLE générique (**nRF Connect**, LightBlue…) :

1. Se connecter à `S3-KBD`, ouvrir le service `9f1d0000-…-1001`.
2. Écrire sur **CMD** (`…-0001`) des commandes JSON (en octets/UTF-8) :
   - `{"t":"char","v":"a"}` → tape **a** sur une cible FR (si **q** apparaît, la
     cible n'est pas en AZERTY : c'est le piège n°1).
   - `{"t":"txt","v":"café à 5 €"}` → reproduit la chaîne accentuée.
   - `{"t":"key","c":"Enter","a":"tap"}` → valide une ligne.
   - `{"t":"key","c":"NumpadAdd"}` / `"NumpadSubtract"` → **+ / −** du pavé numérique,
     indépendants de la disposition (à utiliser dans un BIOS/UEFI, qui lit en QWERTY).
   - `{"t":"char","v":"c","m":1}` → **Ctrl+C**.
   - Séquence tempo (socle §5.3) : `{"t":"seq","s":[{"tap":"1"},{"wait":1000},{"tap":"2"},{"wait":1000},{"rep":3,"every":4000,"tap":"3"}]}`.
   - Arrêt : `{"t":"stop"}` (hors file ; l'ancienne forme `{"t":"seq","n":"stop"}` reste acceptée).
3. S'abonner à **STATUS** (`…-0002`) : les frames sont du **JSON**, ex.
   `{"id":0,"st":"ready"}`, `{"id":0,"st":"busy"}`, `{"id":0,"err":"unmapped"}`.
4. Nouveautés : `{"t":"ping","n":1}` → `{"id":0,"ev":"pong","n":1}` ; `{"t":"cfg","a":"get"}`
   → `{"id":0,"ev":"cfg",…}` ; `{"t":"gpio","p":"o1","a":"tgl"}` → `{"id":0,"ev":"gpio","p":"o1","v":1}`.
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

## Limites connues

- **Protocole boot / BIOS** : le clavier est construit **en premier** (`usbBegin()`),
  donc le cœur déclare l'interface HID en sous-classe *boot* / protocole clavier et
  retire le report ID quand l'hôte passe en mode boot (rapport 8 octets). Le
  fonctionnement *dès le BIOS/UEFI* (recette §6.1) reste à valider selon l'hôte.
  En mode boot, **tous** les rapports partent sans report ID : bouger la souris ou
  envoyer une touche média pendant le BIOS peut produire des frappes parasites.
  Un BIOS lit en QWERTY US : pour +/−, utiliser `NumpadAdd` / `NumpadSubtract`.
- **BLE + Wi-Fi + USB** cohabitent mais sont gourmands en RAM. Le cœur esp32 3.x
  construit déjà la pile BLE sur **NimBLE** (plus légère que Bluedroid, et support
  LESC via `BLESecurity`) : c'est ce qui est utilisé ici.
- **Total des connexions BLE simultanées ≤ 3** (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS`) :
  un maître à 3 esclaves n'a plus de connexion libre pour un téléphone → piloter le maître
  par **COM ou Wi-Fi** (qui ne consomment pas de connexion BLE). Un module déjà connecté à un
  téléphone n'annonce plus : il n'apparaît pas dans un scan d'appairage.
- Les frames STATUS font jusqu'à **512 octets** (le `cfg` embarque `slaves[]` + les noms) :
  un MTU BLE élevé est requis (Android/Chrome, Windows et nRF Connect négocient 517 ; log `[BLE] MTU=`).
- Majuscules accentuées et touches mortes exotiques hors couverture (spec S3 §6) :
  caractère absent de la table → ignoré + `err:unmapped`, jamais de frappe au hasard.

## Sécurité (phase LESC)

Les liaisons BLE sont chiffrées en **LE Secure Connections** avec une **passkey
statique** (défaut `000000`, redéfinissable). Points clés (pile **NimBLE**, cœur 3.x) :

- **iocap globale par rôle** : standard/maître = *DisplayOnly* (le téléphone **saisit**
  la passkey dans la boîte d'appairage de l'**OS**, pas la page web) ; esclave =
  *KeyboardOnly* → le lien maître↔esclave fait un **Passkey-Entry MITM automatique**
  (passkey partagée auto-injectée des deux côtés). `BLESecurity::*` **après**
  `BLEDevice::init()`.
- **CMD = `WRITE_AUTHEN`** (chiffré + MITM) : un pair non appairé ne peut **rien**
  écrire. STATUS = `READ_ENC`. `setAccessPermissions()` étant un no-op sous NimBLE, le
  niveau de sécurité passe par les **bits de propriété** de la caractéristique.
- **Bind = bootstrap Just Works** : le maître se connecte au module vierge (les deux
  *DisplayOnly* → Just Works chiffré, sans passkey), écrit l'ordre de provisioning
  **sur `PROV`** (mac + id + **passkey du maître**) sur ce lien déjà chiffré, efface le
  bond bootstrap, reboot. Au régime établi, `ble_link` fait `secureConnection()`
  (Passkey-Entry MITM) avant tout `writeValue`.
- **Changer la passkey** (`sec`/`passkey`) **efface les bonds** (`ble_store_clear`) —
  sinon l'ancienne LTK reste valable — et **exige d'abord de désappairer tous les
  esclaves**. Côté téléphone : **« oublier »** le module dans les réglages Bluetooth,
  puis se ré-appairer avec la nouvelle passkey.
- **Renommage** (`name`) : nom convivial persistant (NVS), annoncé en GAP (repère au
  scan / au ré-appairage) ; conservé au désappairage. Garder des noms **uniques**.
- **Mot de passe Wi-Fi** (`sec`/`wifi`) : la clé WPA2 du SoftAP est en NVS (défaut
  `12345678`). Le WebSocket reste en clair : **seule la WPA2** protège le canal Wi-Fi ; le
  **port COM** est un canal physique de confiance (non chiffré).

**À valider au matériel** : l'appairage passkey via **Web Bluetooth** (Chrome
Android/Windows) — l'OS affiche « Saisir le code », pas la page.

## ⚠️ À savoir

Côté **USB**, la cible ne distingue pas ce clavier/souris d'un périphérique réel : il
envoie de vraies frappes. Le canal BLE est appairé/chiffré (LESC) + authentifié, mais
l'authentification du **site web** et le chiffrement du WebSocket **Wi-Fi** restent hors
périmètre : brancher de préférence **sur des machines de confiance**.
