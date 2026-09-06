# Clavier HID piloté en BLE — MOCK / POC

Prototype de bout en bout : un **ESP32-S3** branché en USB se fait passer pour un
**clavier AZERTY**, et un **site web** (Web Bluetooth) lui envoie ce qu'il faut taper.

```
  ┌─────────────┐   BLE (GATT)   ┌──────────────┐   USB HID    ┌────────────┐
  │  Site web   │ ─────────────► │  ESP32-S3    │ ───────────► │   Cible    │
  │ (Chrome)    │   JSON/CMD     │  (firmware)  │  frappes     │ PC / TV    │
  └─────────────┘                └──────────────┘              └────────────┘
     WEB/Keyboard                  S3/hid_firmware
```

> **Statut : mock, sans aucune sécurité** (ni auth du site, ni appairage chiffré).
> Reporté en phase sécurité (voir les cahiers des charges dans `ressources/`).
> ⚠️ Ce périphérique **est** un injecteur de frappes (BadUSB) : ne le brancher que
> sur des machines de confiance tant que la sécurité n'est pas en place.

## Arborescence

```
.
├── README.md                      ← ce fichier
├── serve.bat                      ← lance le serveur HTTPS local (pour le téléphone)
├── serve.py                       ← moteur du serveur HTTPS + génération du certificat
├── ressources/                    ← cahiers des charges + doc carte (fournis)
├── S3/hid_firmware/               ← LOT S3 : firmware Arduino ESP32-S3
│   ├── hid_firmware.ino
│   ├── keymap_azerty.h            ← table AZERTY (pièce critique)
│   ├── status_led.h               ← indicateur d'état LED RGB (tâche dédiée)
│   └── README.md                  ← build/flash + réglages carte
└── WEB/                           ← LOT WEB (peut héberger plusieurs projets)
    ├── index.html                 ← redirige vers le projet par défaut (Keyboard)
    ├── ssl/                        ← certificat auto-signé (généré par serve.bat)
    └── Keyboard/                  ← site statique Web Bluetooth
        ├── index.html
        ├── style.css
        ├── app.js
        └── README.md              ← lancement local / hébergement
```

## Contrat d'interface (la seule frontière entre les 2 lots — socle §5)

| Élément | UUID | Propriétés |
|---|---|---|
| Service HID-Bridge | `9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001` | — |
| CMD | `9f1d0001-5b8e-4a4a-9c2a-2b7f3e6a1001` | Write / Write NR |
| STATUS | `9f1d0002-5b8e-4a4a-9c2a-2b7f3e6a1001` | Notify |

Commandes (JSON UTF-8 sur **CMD**) :

| `t` | Effet | Exemple |
|---|---|---|
| `char` | un caractère (mode direct) | `{"t":"char","v":"é"}` |
| `txt`  | une chaîne (macro) | `{"t":"txt","v":"café à 5 €"}` |
| `key`  | touche nommée / média | `{"t":"key","c":"Enter","a":"tap"}` |
| `seq`  | séquence prédéfinie ou perso | `{"t":"seq","n":"alt_tab"}` |
| `mouse`| déplacement / clic / molette | `{"t":"mouse","dx":10,"dy":-4}` |

## Démarrage rapide

1. **Firmware** — voir [`S3/hid_firmware/README.md`](S3/hid_firmware/README.md) :
   Arduino IDE, cœur *esp32*, lib *ArduinoJson*, **USB Mode = USB-OTG (TinyUSB)**,
   téléverser, brancher l'ESP32-S3 sur la cible.
2. **Site** — deux options :
   - **Sur ce PC** : `python -m http.server 8000` puis **http://localhost:8000** dans Chrome/Edge.
   - **Depuis un téléphone Android** (HTTPS requis) : lancer **`serve.bat`** (serveur
     HTTPS auto-signé, port 8443) puis ouvrir **https://\<IP-du-PC\>:8443** sur le
     téléphone et accepter l'avertissement de certificat. Voir plus bas.
3. Cliquer **Connecter**, choisir `S3-KBD`, taper. Le **journal** de la page
   montre le JSON émis et l'état renvoyé.

## Accès depuis un téléphone Android (HTTPS)

Web Bluetooth exige un **contexte sécurisé** : `localhost` (ce PC) **ou HTTPS**.
Depuis un téléphone, `localhost` ne s'applique pas → il faut du HTTPS.

1. Sur le PC : double-clic sur **`serve.bat`** (génère le certificat au 1ᵉʳ lancement
   dans `WEB/ssl/`, puis sert `WEB/` en HTTPS sur le port **8443**). La console
   affiche l'URL à utiliser, ex. `https://192.168.10.121:8443/`.
2. Téléphone et PC sur le **même réseau Wi-Fi/LAN**. Autoriser l'invite du
   **pare-feu Windows** si elle apparaît (accès réseau privé).
3. Sur **Chrome/Edge Android** : ouvrir `https://<IP-du-PC>:8443/`.
   Le certificat est **auto-signé** → avertissement « connexion non privée » →
   **Paramètres avancés → Continuer**. La page est alors en contexte sécurisé et
   Web Bluetooth fonctionne. → **Connecter** → `S3-KBD`.

> **iOS/iPadOS : impossible** — aucun navigateur iOS n'implémente Web Bluetooth
> (moteur WebKit imposé). Le HTTPS n'y change rien (socle §7). **Brave** : Chromium
> mais Web Bluetooth désactivé par défaut → préférer **Edge/Chrome**.

## Choix techniques (mock)

- **Arduino-ESP32** plutôt qu'ESP-IDF (plus rapide à valider, aligné sur le code
  de référence fourni). Les briques restent équivalentes : TinyUSB (HID composite),
  pile BLE intégrée, ArduinoJson, tâche FreeRTOS pour le séquenceur.
- **Disposition AZERTY entièrement dans le firmware** : le web n'envoie que
  l'intention (piège n°1 du socle). Table dans `keymap_azerty.h`, à tester en premier.
- Tempo des séquences **côté firmware** (horloge locale), pour que la latence BLE
  ne déforme pas les délais.

## Ce qui reste hors périmètre (phase sécurité)

Appairage BLE chiffré + liste blanche, échange de clés / challenge applicatif,
authentification du site, stockage persistant, disposition paramétrable, variante
BLE-HID directe (Apple TV). Voir `ressources/cahier-des-charges_1.md` §8.
