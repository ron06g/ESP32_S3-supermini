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
est `hwcdc`, qui n'expose **pas** de HID. `CDCOnBoot=default` (= Disabled) est
**tout aussi indispensable** depuis que les interfaces USB dépendent de la config NVS.

Depuis l'ajout du transport Wi-Fi, le FQBN inclut aussi `FlashSize=4M`,
`PartitionScheme=huge_app` (l'app par défaut ~1,3 Mo ne suffit plus) et
`PSRAM=enabled`. **Avant de compiler, régénérer l'app embarquée** :

```bash
python S3/hid_firmware/tools/gen_web_assets.py   # écrit S3/hid_firmware/web_assets.h
arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled S3/hid_firmware
arduino-cli upload  --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled -p COM7 S3/hid_firmware
```

- Prérequis : cœur **esp32 ≥ 2.0.14**, bibliothèque **ArduinoJson v7**,
  bibliothèque **WebSockets** (Markus Sattler / Links2004) pour le transport Wi-Fi.
- Via l'IDE : Board « ESP32S3 Dev Module », **USB Mode = USB-OTG (TinyUSB)**,
  **USB CDC On Boot = Disabled** (le sketch possède la console USB `Console` et
  appelle lui-même `USB.begin()` après avoir construit les interfaces choisies en
  NVS ; avec « Enabled » le cœur appelle `USB.begin()` avant `setup()` et le HID
  disparaît), Upload Mode = UART0 / Hardware CDC.
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
le statut revient en **frames JSON** (voir contrat STATUS ci-dessous). L'app choisit le
transport au chargement : Wi-Fi si le paramètre d'URL `D` (base64url de
`{"wifi":"192.168.4.1",…}`) est présent, sinon BLE. Firmware : `wifi_portal.h` +
`web_assets.h` (app embarquée). Web : façade `bleTransport`/`wifiTransport` dans
`app.js`, seul `send()` touche le transport. Le cœur (file, séquenceur, AZERTY,
USB) est partagé — les deux transports enfilent dans `g_cmdQueue`.

| Élément | UUID | Propriétés |
|---|---|---|
| Service HID-Bridge | `9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001` | — |
| CMD | `9f1d0001-…-1001` | Write / Write NR |
| STATUS | `9f1d0002-…-1001` | Notify (frames JSON `{"id":…}`) |

Commandes JSON UTF-8 sur CMD, champ `t` : `char` (un caractère, mode direct),
`txt` (chaîne / macro), `key` (touche nommée / média, action `tap`/`down`/`up`),
`seq` (séquence prédéfinie `n`, personnalisée `s[]`, ou `stop`), `mouse`
(déplacement `dx/dy`, molette `w`, bouton `b`+`a`), `ping` (`n` → `pong`),
`cfg` (`a`=`get`/`set` : flags persistants, réponse `ev:cfg`, `set` redémarre),
`pair` (`a`=`scan`/`bind`/`slave`/`unbind`/`reset` : appairage BLE↔BLE), `gpio`
(`p` label, `a`=`set`/`clr`/`tgl`/`read`). Masque
modificateurs `m` : bit0=Ctrl, 1=Shift, 2=Alt, 3=GUI, 4=AltGr.

**Champ `id` (routage étoile).** Chaque commande accepte un `id` optionnel
(**0 = maître/local par défaut**, 1..3 = esclave). Sur un maître, une commande
`id≠0` est **routée** vers l'esclave correspondant ; `cfg`/`pair` restent toujours
locaux (l'`id` d'un `pair` désigne l'esclave à désappairer, pas une cible de routage).

**Contrat STATUS (JSON).** Chaque frame est un objet portant `id` (0 = émetteur
local/maître, 1..3 = esclave qui stampe son `selfId` ; le maître **relaie
verbatim**). Discriminants : `{"id":n,"st":"ready|busy"}` (état ; seul l'id 0 est
l'« état courant »), `{"id":n,"err":"<code>"}`, ou événements
`{"id":n,"ev":"cfg|scan|pair|link|gpio|pong",…}` — ex. `{"id":1,"ev":"gpio","p":"4","v":1}`,
`{"id":2,"ev":"link","up":true}`. Frames jusqu'à **384 octets** (`STATUS_MAX`, le
`cfg` embarque la table `slaves[]`). Côté firmware, `notifyStatus()` est un
**adaptateur** qui convertit encore les chaînes legacy (`ready`/`err:*`/`gpio:*`…)
en JSON `{"id":g_myId,…}` et laisse passer une frame déjà JSON (`statusRaw`).

**Troisième transport (port COM) et rôles maître/esclave.** L'ESP32-S3 (USB-OTG
FS, 6 endpoints) ne peut pas héberger clavier + souris + **deux** CDC (composite
refusé, Windows code 10). Le port COM **réutilise donc l'unique CDC** (`com_port.h`,
interface 0) : si le flag `serial` est actif, ce CDC parle le protocole (1 ligne =
1 commande, STATUS en lignes, logs de debug tus) ; sinon il reste la console de
debug. Un maître peut s'appairer en **étoile** avec jusqu'à **`MAX_SLAVES` = 3**
esclaves (`ble_link.h`). **Le maître (id 0) injecte le HID/GPIO/COM LOCALEMENT** et
**route par `id`** vers l'esclave désigné (une tâche `linkN` + client GATT par
esclave, file de relais partagée) ; il garde BLE + Wi-Fi pour le téléphone. Un
esclave (id 1..3) **ne fait que des GPIO** : pas de Wi-Fi, un BLE réservé à son
maître (whitelist + vérif MAC), exécute localement GPIO + COM et se signale
toujours avec son `selfId`. Lien d'un esclave coupé → `{"id":n,"err":"nolink"}`.
**Plafond dur** : total des connexions BLE simultanées ≤ 3 (`CONFIG_BT_NIMBLE_MAX_
CONNECTIONS`) → l'hôte (RPi/PC) pilote le maître par **COM/Wi-Fi** (pas de
connexion BLE), les 3 connexions vont aux esclaves. Table de routage `id↔MAC` en
NVS ; à l'appairage le maître attribue le **plus petit id libre** (1..3).
Désappairage : `{"t":"pair","a":"unbind","id":n}` (un esclave) ou `id`=0 / **5
appuis BOOT** sur le maître (toute l'étoile) ; `{"t":"pair","a":"reset"}` sur le
COM d'un esclave le libère. Paramètres et rôle sont en NVS (`config.h`,
`Preferences`, namespace `s3kbd`).

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
- **Un seul thread par ressource** : WS TX → tâche `net` ; port COM RX/TX → tâche
  `com` (`USBCDC::write` peut bloquer 250 ms) ; **chaque client BLE du maître
  (connect / writeValue / getRssi) → SA tâche `linkN` uniquement** (`writeValue`
  attend un événement GATTC : interdit depuis un callback BLE) ; entrées GPIO →
  tâche `gpio`. Les callbacks BTC (`onWrite`, notify client) ne font que copier
  dans une file (les notify esclaves → file de relais partagée, drainée par `relay`).
- **Les interfaces USB sont enregistrées dans les constructeurs** (`USBHIDKeyboard()`,
  `USBCDC`) : les objets HID sont créés par `new` selon `g_cfg` **avant** `USB.begin()`,
  qui n'est appelé que par le sketch (FQBN `CDCOnBoot=default`, sinon le cœur l'appelle
  avant `setup()` et le HID disparaît). **Budget d'endpoints** : au plus clavier+souris
  (1 interface HID) + **un** CDC ; le port COM réutilise donc la console. Changer un flag
  USB = sauvegarde NVS + redémarrage ; le numéro de série USB dérive des flags (cache
  descripteur Windows).
- **`cfg` et `pair` sont toujours locaux** ; sur un maître, une commande `id≠0` est
  routée vers l'esclave (`id=0`/absent = exécution locale). Le maître **relaie
  verbatim** les STATUS des esclaves (déjà tagués de leur `selfId`).
- **STOP est hors-file** : détecté dans le callback d'écriture, il vide la file et
  interrompt immédiatement la séquence en cours (`g_stop`) au lieu d'attendre son
  tour ; sur un maître il est **diffusé à TOUS les esclaves** (hors-file des deux côtés).

### Firmware — fichiers

- `hid_firmware.ino` : orchestration (USB HID composite clavier+consumer+souris,
  serveur BLE Bluedroid, parseur ArduinoJson, dispatch, séquenceur, file/worker).
- `keymap_azerty.h` : table Unicode → frappe(s) HID (voir invariant ci-dessus).
- `config.h` : paramètres NVS (`cfg_t g_cfg` : flags, rôle, `selfId` de l'esclave,
  MAC du maître `peer[]`, table de routage `slaves[MAX_SLAVES]` + helpers de slot).
- `com_port.h` : port COM = CDC unique en mode protocole (tâche `com`), pas de 2ᵉ CDC.
- `gpio_panel.h` : table `GPIO_TABLE[]` (BOOT + sorties 4–7 + entrées 8–11,
  nommage sérigraphie SuperMini), scrutation anti-rebond, `gpioHandle()`.
- `ble_link.h` : étoile multi-esclaves. scan / bind (id auto) / unbind (par id ou
  tous) exécutés dans le worker + une tâche `linkN` par esclave (reconnexion,
  écriture, RSSI, événements `link`) + tâche `relay` (rediffuse les STATUS des
  esclaves via la file partagée `g_relayQueue`). Runtime `g_link[MAX_SLAVES]`.
- `status_led.h` : indicateur LED RGB WS2812 (GPIO48) dans une tâche dédiée à
  ~50 Hz. Modes esclave : `LST_SLAVE_WAIT` (ambre), `LST_SLAVE_LINKED` (vert),
  l'intensité verte suivant le RSSI (`ledSetRssi`, -90…-40 dBm ; via
  `ble_gap_conn_rssi` dans `loop()`). Sur un **maître** multi-esclaves, la LED
  reste `LST_CONNECTED` dès qu'un lien est actif (`linkAnyUp()`) sans teinte RSSI
  (plusieurs esclaves). Le reste du code déclare un **état** (`ledSetMode`/`ledSetError`) ou une
  **impulsion** brève (`ledPulse`) ; il ne pilote jamais la LED directement.

### Web — fichiers (`WEB/Keyboard/`)

Client Web Bluetooth pensé **smartphone Android en paysage**. `app.js` définit les
onglets (AZERTY, Num, Fn/Média, Souris, Texte, Macro, Séq., GPIO ; le panneau
Réglages s'ouvre par l'icône ⚙️ de l'en-tête et héberge le bouton Journal), les
**modificateurs collants** (clic = one-shot armé, 2ᵉ = verrou, 3ᵉ = off), le
garde-fou MTU (`MTU_GUARD` 500 o) et le **journal** (chaque JSON émis + chaque
STATUS reçu), outil de validation. À la connexion, `onConnected()` envoie
`cfg get` et `applyFlags()` masque les onglets désactivés (`TAB_FLAGS`).
Le panneau Réglages porte l'appairage **en étoile** : liste des esclaves
(`renderSlaves()` depuis `cfg.slaves[]` — id, MAC, lien, RSSI, unbind par id),
ajout par scan → bind, et le **test de liaison** par cible (`linkTest()` + sélecteur
`#linkTarget` : pings numérotés au maître ou à un esclave, RTT, pertes). L'onglet
GPIO affiche **une section par carte** (`buildGpioModules()` : maître id 0 + chaque
esclave, tout sur un écran) ; chaque élément porte `data-gid`+`data-gpio`, le clic
route via `send(…, id)` et `onGpio()` cible `[data-gid][data-gpio]`. « Relire tout »
lit toutes les cartes. Les libellés GPIO (`GPIO_OUT`/`GPIO_IN`) doivent rester
alignés sur `gpio_panel.h`. `WEB/index.html` redirige vers `Keyboard/`.

## Références

Les cahiers des charges sont dans `ressources/` : `cahier-des-charges_1.md`
(socle : vision, contrat GATT §5, pièges §7, hors-périmètre sécurité §8),
`_2.md` (lot S3), `_3.md` (lot WEB). La doc carte est dans `ressources/ESP32/`.
Le code C de référence (S3 SuperMini) est dans `ressources/ESP32/S3 Supermini/`.
