# Clavier HID piloté à distance

Chaîne de bout en bout : un **ESP32-S3** branché en USB se présente comme un
**clavier/souris AZERTY**, piloté par un **site web** (smartphone Android en
paysage) qui lui envoie ce qu'il faut taper. Trois canaux de commande possibles —
**BLE**, **Wi-Fi** ou **port COM** — partageant le **même protocole JSON**.

```
   ┌────────────┐   BLE (GATT)  ╲
   │  Site web  │ ─────────────── ╲   ┌──────────────┐   USB HID    ┌────────────┐
   │  (Chrome / │   Wi-Fi (WS)  ─── ► │  ESP32-S3    │ ───────────► │   Cible    │
   │   Android) │ ─────────────── ╱   │  (firmware)  │  frappes     │ PC / TV    │
   └────────────┘   port COM     ╱    └──────┬───────┘              └────────────┘
      WEB/Keyboard              maître (id 0) = clavier/souris │ BLE ↔ BLE (étoile, id 1..3)
                                                          ▼
                                              ┌──────────────┐   GPIO déportés
                                              │  ESP32-S3    │   (lecture / écriture
                                              │ esclave id 1 │    par `id`)
                                              └──────────────┘   … jusqu'à 3 esclaves
```

> **Sécurité** : liaisons BLE chiffrées **LESC** + authentification MITM (CMD), Wi-Fi
> **WPA2**. Hors périmètre actuel : auth du site web et chiffrement du WebSocket Wi-Fi.
> ⚠️ Le périphérique agit comme un **clavier/souris USB** : il envoie de vraies frappes
> à la machine cible. Comme tout clavier, ne le connecter qu'à des machines de confiance.

## État actuel

Ce qui fonctionne aujourd'hui (validation **manuelle**, pas de tests automatisés) :

- **USB HID** : clavier AZERTY + Consumer Control (média) + souris, activables
  indépendamment. Traduction AZERTY **entièrement dans le firmware**.
- **Transport BLE** (serveur GATT HID-Bridge) — Chrome/Edge, Android ou desktop.
- **Transport Wi-Fi** : SoftAP `S3-KBD` + portail captif + app web embarquée +
  **WebSocket** (`ws://192.168.4.1:81/`). Rejoue le même protocole JSON.
- **Transport port COM** : l'unique CDC USB bascule en mode protocole (flag `serial`).
- **Appairage en étoile** (BLE ↔ BLE, jusqu'à 3 esclaves) : le maître injecte le HID
  localement (id 0) et **route les GPIO par `id`** vers l'esclave désigné ; désappairage
  physique par **5 appuis sur BOOT**.
- **Panneau GPIO** pilotable (sorties `o1`–`o4`, entrées `i1`–`i4`, repères logiques
  numérotés à partir de 1 ; + bouton `BOOT`), avec **clignotement et PWM
  autonomes** exécutés par la carte (aucun trafic radio par transition) + **LED RGB d'état**.
- **Paramètres persistants** en NVS (`cfg`), pilotables depuis l'app (panneau Réglages).
- **Optimisation conso/chaleur** : CPU à 160 MHz, puissance TX radios réduite en
  usage standard (BLE gardé à fond en mode appairé). Voir plus bas.

## Arborescence

```
.
├── README.md                      ← ce fichier (vue d'ensemble)
├── serve.bat / serve.py           ← serveur HTTPS local (accès téléphone Android) + API de pré-inscription
├── inscriptions.py                ← pré-inscriptions « Je veux mon S3-KBD » (SQLite data/, non versionné)
├── ressources/                    ← cahiers des charges + doc carte (fournis)
├── S3/
│   ├── hid_firmware_compile.bat   ← régénère web_assets.h + compile → firmware/hid_firmware_vYYMM.dd/
│   ├── hid_firmware_flash.bat     ← téléverse la dernière version de firmware/ (port ESP32 auto-détecté)
│   ├── firmware/                  ← 1 sous-dossier par version (app + .bootloader + .partitions, non versionnés)
│   └── hid_firmware/              ← LOT S3 : firmware Arduino ESP32-S3
│       ├── hid_firmware.ino       ← orchestration (USB, BLE, dispatch, worker)
│       ├── keymap_azerty.h        ← table AZERTY → HID (pièce critique)
│       ├── config.h               ← paramètres NVS (flags, rôle, MAC du pair)
│       ├── wifi_portal.h          ← transport Wi-Fi (SoftAP + captif + WebSocket)
│       ├── com_port.h             ← transport port COM (CDC unique en mode protocole)
│       ├── ble_link.h             ← appairage BLE ↔ BLE (maître/esclave)
│       ├── gpio_panel.h           ← panneau GPIO (BOOT + sorties/entrées)
│       ├── status_led.h           ← indicateur LED RGB WS2812 (tâche dédiée)
│       ├── web_assets.h           ← app web embarquée (généré, ne pas éditer)
│       ├── tools/gen_web_assets.py← (re)génère web_assets.h depuis WEB/Keyboard/
│       └── README.md              ← build/flash + réglages carte (détaillé)
└── WEB/                           ← LOT WEB
    ├── index.html                 ← redirige vers Keyboard/
    ├── ssl/                        ← certificat auto-signé (généré par serve.bat)
    ├── Landing/                   ← site public, NON embarqué dans le firmware
    │   ├── landing.html           ← page commerciale + popup de pré-inscription
    │   ├── notice.html            ← notice technique (schémas, réglages d'usine, LED…)
    │   └── protocole.html         ← référence du protocole du port COM
    └── Keyboard/                  ← app statique Web Bluetooth / WebSocket (embarquée)
        ├── index.html · style.css · app.js
        ├── pwa.js · sw.js · manifest.webmanifest   ← PWA
        ├── accept.html · icons/
        └── README.md              ← lancement local / hébergement
```

## Contrat d'interface (la seule frontière entre les 2 lots — socle §5)

Le **GATT** est le contrat ; le Wi-Fi et le port COM rejouent le **même JSON**.

| Élément | UUID | Propriétés |
|---|---|---|
| Service HID-Bridge | `9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001` | — |
| CMD | `9f1d0001-5b8e-4a4a-9c2a-2b7f3e6a1001` | Write / Write NR |
| STATUS | `9f1d0002-5b8e-4a4a-9c2a-2b7f3e6a1001` | Notify (frames JSON `{"id":…}`) |

Commandes (JSON UTF-8, champ `t`) :

| `t` | Effet | Exemple |
|---|---|---|
| `char` | un caractère (mode direct) | `{"t":"char","v":"é"}` |
| `txt`  | une chaîne / macro | `{"t":"txt","v":"café à 5 €"}` |
| `key`  | touche nommée / média (`tap`/`down`/`up`) | `{"t":"key","c":"Enter","a":"tap"}` |
| `seq`  | séquence prédéfinie ou perso (`s[]`) | `{"t":"seq","n":"alt_tab"}` |
| `stop` | arrêt immédiat (hors file) de la séquence en cours | `{"t":"stop"}` |
| `mouse`| déplacement / molette / bouton | `{"t":"mouse","dx":10,"dy":-4}` |
| `ping` | test de liaison | `{"t":"ping","n":12}` → `{"id":0,"ev":"pong","n":12}` |
| `cfg`  | lire/écrire les flags persistants (`set` redémarre) | `{"t":"cfg","a":"get"}` |
| `pair` | appairage en étoile (`scan`/`bind`/`slave`/`unbind`/`reset`) | `{"t":"pair","a":"scan"}` |
| `gpio` | sortie / lecture d'une broche (routable par `id`), clignotement `loop` et PWM `pwm` autonomes | `{"t":"gpio","p":"o1","a":"loop","t_set":200,"t_clr":800,"nb":5}` |

Masque modificateurs `m` : bit0=Ctrl, 1=Shift, 2=Alt, 3=GUI, 4=AltGr. Champ **`id`**
optionnel (0 = maître/local par défaut, 1..3 = esclave) : le maître route la commande.
Le **STATUS est en JSON** (`{"id":n,"st|err|ev":…}`). Détail complet et réponses :
[`S3/hid_firmware/README.md`](S3/hid_firmware/README.md).

## Transports & rôles

- **BLE** — transport par défaut. L'app se connecte au service HID-Bridge.
- **Wi-Fi** (résolution A, compatible iPhone) — le S3 est un point d'accès `S3-KBD`
  qui sert l'app et reçoit les commandes par WebSocket. L'app choisit le Wi-Fi si
  le paramètre d'URL `D` (base64url) est présent, sinon le BLE.
- **Port COM** — le CDC USB unique parle le protocole si le flag `serial` est actif
  (sinon il reste la console de debug). Pas de 2ᵉ CDC (budget d'endpoints du S3).
- **Étoile maître / esclaves** — un maître peut s'appairer avec jusqu'à **3** esclaves.
  Le **maître (id 0)** injecte le HID localement, garde BLE + Wi-Fi pour le téléphone et
  **route les commandes par `id`** vers ses esclaves ; un **esclave** ne fait que des
  **GPIO** (pas de Wi-Fi, BLE réservé à son maître). Total connexions BLE ≤ 3 → piloter le
  maître par COM/Wi-Fi. **5 appuis sur BOOT** (< 3 s) sur le maître = désappairage de toute
  l'étoile.

## Démarrage rapide

1. **Firmware** — voir [`S3/hid_firmware/README.md`](S3/hid_firmware/README.md).
   Régénérer l'app embarquée **avant** de compiler, puis flasher :
   ```bash
   python S3/hid_firmware/tools/gen_web_assets.py
   arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled S3/hid_firmware
   arduino-cli upload  --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=4M,PartitionScheme=huge_app,PSRAM=enabled -p COM7 S3/hid_firmware
   ```
   > Sous Windows : `S3\hid_firmware_compile.bat` (publie `S3\firmware\hid_firmware_vYYMM.dd\`)
   > puis `S3\hid_firmware_flash.bat [COMx] [dossier]` (dernière version, port auto-détecté).
   > **Réglages IDE** : USB Mode = **USB-OTG (TinyUSB)**, USB CDC On Boot = **Disabled**.
   > **Après l'upload : appuyer sur RESET** (sinon la carte reste en ROM).
2. **Site** — deux options :
   - **Sur ce PC** : `cd WEB/Keyboard && python -m http.server 8000` puis
     **http://localhost:8000** dans Chrome/Edge.
   - **Depuis un téléphone Android** : `serve.bat` (HTTPS auto-signé, port 8443) —
     voir ci-dessous. Ou, sans PC, rejoindre le **Wi-Fi `S3-KBD`** (le S3 sert l'app).
3. Cliquer **Connecter**, choisir `S3-KBD`, taper. Le **journal** (panneau Réglages ⚙️)
   montre chaque JSON émis et chaque STATUS reçu.

## Accès depuis un téléphone Android (HTTPS)

Web Bluetooth exige un **contexte sécurisé** : `localhost` (ce PC) **ou HTTPS**.
Depuis un téléphone, `localhost` ne s'applique pas → il faut du HTTPS (transport BLE),
ou passer par le **Wi-Fi** du S3 (transport WebSocket, pas de certificat requis).

1. Sur le PC : double-clic sur **`serve.bat`** (génère le certificat au 1ᵉʳ lancement
   dans `WEB/ssl/`, puis sert `WEB/` en HTTPS sur le port **8443**). La console
   affiche l'URL à utiliser, ex. `https://192.168.10.121:8443/`.
2. Téléphone et PC sur le **même réseau Wi-Fi/LAN**. Autoriser l'invite du
   **pare-feu Windows** si elle apparaît (accès réseau privé).
3. Sur **Chrome/Edge Android** : ouvrir `https://<IP-du-PC>:8443/`.
   Certificat **auto-signé** → avertissement « connexion non privée » →
   **Paramètres avancés → Continuer**. → **Connecter** → `S3-KBD`.

> **iOS/iPadOS : impossible** en Web Bluetooth (moteur WebKit imposé) — utiliser
> le **transport Wi-Fi**. **Brave** : Web Bluetooth désactivé par défaut →
> préférer **Edge/Chrome**.

## Choix techniques

- **Arduino-ESP32** plutôt qu'ESP-IDF (plus rapide à valider, aligné sur le code
  de référence fourni). TinyUSB (HID composite), pile BLE (NimBLE), ArduinoJson,
  tâches FreeRTOS (worker, réseau, com, link, gpio, led).
- **Disposition AZERTY entièrement dans le firmware** : le web n'envoie que
  l'intention (piège n°1 du socle). Table dans `keymap_azerty.h`, à tester en premier.
- Tempo des séquences **côté firmware** (horloge locale), pour que la latence des
  transports ne déforme pas les délais.
- **Un seul thread par ressource** : seul le `worker` touche l'USB HID ; les
  transports ne font qu'enfiler dans une file commune.
- **Conso / chaleur** : le CPU tourne à **160 MHz** (au lieu de 240) et la
  puissance TX des radios est réduite en usage standard (Wi-Fi ~11 dBm, BLE 0 dBm) ;
  le **BLE reste à pleine puissance en mode appairé** (portée du lien maître/esclave).
  Aucune sonde de courant/tension sur la carte SuperMini → validation conso par
  wattmètre USB externe ; la puce dispose d'un capteur de température interne (non
  encore exploité par le firmware).

## Ce qui reste hors périmètre (phase sécurité)

Appairage BLE chiffré + liste blanche authentifiée, échange de clés / challenge
applicatif, authentification du site, disposition paramétrable, variante BLE-HID
directe (Apple TV). Voir `ressources/cahier-des-charges_1.md` §8.
