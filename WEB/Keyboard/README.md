# Lot WEB — Site « Keyboard »

Page **statique** (HTML/CSS/JS, sans build ni dépendance) qui se connecte au
périphérique S3 en **Web Bluetooth** et lui envoie des commandes clavier.

## Fichiers

| Fichier | Rôle |
|---|---|
| `index.html` | Structure : barre de connexion, onglets, clavier, journal |
| `style.css`  | Mise en page tactile (mobile Android d'abord), thème sombre |
| `app.js`     | Client Web Bluetooth, encodage des commandes, 3 modes de saisie |

## Interface (onglets)

Pensée **smartphone en paysage** : détection téléphone + orientation, clavier en
**plein écran** (touches agrandies au maximum), bouton **plein écran** (⛶, avec
verrouillage paysage best-effort) et **journal en tiroir** (☰) pour libérer l'espace.

| Onglet | Contenu |
|---|---|
| **AZERTY** | clavier AZERTY complet, modificateurs **collants** (clic = armé *one-shot*, 2ᵉ = **verrou** 🔒, 3ᵉ = off) ; **maintien = répétition** |
| **Num** | pavé numérique (chiffres, opérateurs, ⌫, Entrée, Tab…) ; maintien = répétition |
| **Fn/Média** | F1–F12, Ctrl/Maj/Alt/Win/AltGr, navigation, **télécommande média** ; Vol± et flèches répétés au maintien (page qui défile : frappe au relâchement, glissement = annulation) |
| **Bios** | démarrage d'un PC (BIOS/UEFI, menu de boot, GRUB, Windows) : **grosses touches en pavés** — F1–F12, Suppr, Ctrl+Alt+Suppr ; croix de navigation (Entrée au centre), Pg, Début/Fin, **+/− du pavé numérique** (`NumpadAdd`/`NumpadSubtract` : un BIOS lit en QWERTY), Échap, Espace, ⌫ ; Tab, Maj+F10, Y/N, e/c/Ctrl+X, Pause. Ignore les modificateurs collants |
| **Souris** | trackpad + boutons + molette — ✅ pris en charge par le firmware (USB HID souris). Appui bref = clic, **appui long (450 ms) = clic maintenu** (glisser avec le bouton, relâché au lever du doigt) ; ~60 envois/s, file GATT unique |
| **Texte** (⌨️ de l'onglet Souris) | saisie au **clavier physique/OS** du téléphone, **envoi en direct** (diff → `txt` / `Backspace`) ; 💾 mémorise le texte comme **macro** |
| **Séquence** (bas de Fn/Média) | éditeur frappe/attente/répétition + préréglage démo + **Stop** ; **💾 Mémoriser** = macro |
| **💾 Macros** | bibliothèque des macros (texte / séquence) gardée par **ce navigateur** (`localStorage` `s3kbd.macros`) : ▶ lancer, 📝 rouvrir une séquence dans l'éditeur, renommer, supprimer. **Enregistrer / charger** un fichier JSON (`s3kbd-macros-AAAA-MM-JJ.json`, remplacer ou ajouter). **Autoboot** : copie une macro **dans le module** (`autoboot put` en morceaux ≤ 300 o, acquittés) avec un délai 1–30 s ; le module la tape seul à chaque démarrage, après la reconnaissance USB ; Tester / Désactiver |
| **GPIO** | une section par carte (maître + esclaves) **selon son modèle** (lu par `sys` : SuperMini `o1`–`o4`/`i1`–`i4`, DevKit `o1`–`o8`/`i1`–`i8`, GPIO physique affiché sous chaque repère) : sorties (bascule ; appui long = clignotement / PWM autonomes) et entrées BOOT, `i…` (voyants) — `{"t":"gpio",…}` |
| **⚙️ Réglages** (icône de l'en-tête, à côté de « Connecter » ; contient aussi le bouton **Journal**) | config persistante du module (interrupteurs HID clavier / souris / COM / GPIO / appairage → `cfg set` + redémarrage), **appairage BLE ↔ BLE** (Rechercher → Appairer, état du lien, RSSI, Désappairer) et **Test liaison** (20 pings : RTT min/moy/max, pertes). Sous-onglet **🔒 Sécurité** : passkey, clé Wi-Fi, **curseurs de puissance BLE** (−24…+20 dBm, ou *Auto* selon le rôle) et **Wi-Fi** (2–20 dBm), section **Processeur** (80 / 160 / 240 MHz, température de la puce) → `sys set` + redémarrage. Sous-onglet **📱 Appareil** : **retour au toucher** (vibration / clic / les deux / aucun), préférence du téléphone en `localStorage`, utilisable hors connexion ; version (`APP_VERSION`) et bouton **Recharger le site** (`reloadApp()` : vérifie que le site répond via une URL unique hors cache SW, puis vide le cache, désinscrit le SW et recharge) |

**Répétition au maintien** : 1 frappe, puis une toutes les 120 ms après 400 ms (taps
successifs, jamais `down`/`up` → aucune touche collée). Tous les caractères + ⌫, Suppr,
Entrée, Espace, Tab, flèches, Pg↑/↓, Vol± ; **jamais** les modificateurs ni les bascules
(Verr. Maj, Muet, Lecture). La commande est figée à la 1re frappe (Maj armé + maintien de
« a » = « AAAA »).

À la connexion, l'app **synchronise** (`syncModule`) : `{"t":"cfg","a":"get"}` avec
**ré-essais** (6 essais, délai croissant, réabonnement aux notifications BLE au 3ᵉ) — un seul
`cfg get` perdu laissait les Réglages « Hors connexion » alors que le lien était actif. Le
bandeau des Réglages distingue *hors connexion*, *lecture en cours (essai n/6)* et *échec*
(bouton **Réessayer**) ; ouvrir les Réglages connecté sans config relance la lecture. Puis
`sys get` (modèle, puissances) et `autoboot get` ; le modèle de chaque esclave est lu dès que
son lien monte. Un ancien firmware répond `err:type` : repli immédiat (brochage SuperMini,
réglages de puissance masqués). La réponse `cfg` **masque les onglets** des fonctions
désactivées (clavier → AZERTY/Num/Fn/Bios/Macros/Texte, souris, GPIO) et la section
Appairage si `pair` est à 0. Les STATUS sont des frames JSON `{"id":n,…}` dispatchées dans
`handleStatus` ; en BLE, une frame découpée par le firmware (morceaux préfixés `0x1F`) est
recollée **en octets** dans `onStatus` avant décodage.

- **Connexion BLE** filtrée sur le service `9f1d0000-…-1001` (socle §5.1), notifications
  **STATUS**, indicateur d'état + reconnexion, **diagnostic** au chargement (contexte
  sécurisé, présence de l'API, adaptateur actif).
- **Journal** (tiroir) : chaque JSON émis (→) et chaque STATUS reçu (←) — outil de validation.

### Protocole souris (pris en charge par le firmware)

| Commande | Sens |
|---|---|
| `{"t":"mouse","dx":<int>,"dy":<int>}` | déplacement relatif du curseur |
| `{"t":"mouse","b":"left\|right\|middle","a":"click\|down\|up\|dbl"}` | clic / maintien / double |
| `{"t":"mouse","w":<int>}` | molette (vertical) |

Le firmware expose une interface **USB HID souris** (en plus du clavier et du
consumer control). Un bouton inconnu → `err:mouse`.

> **Responsabilité (piège AZERTY)** : le web n'envoie **jamais** de code de scan,
> seulement l'intention — `{"t":"char","v":"é"}` ou `{"t":"key","c":"Enter"}`.
> Shift/AltGr choisissent le glyphe côté web ; la traduction AZERTY→HID est dans le firmware.

## Contraintes (importantes)

| Élément | Règle |
|---|---|
| Navigateur | **Chrome / Edge** sur **Android** ou **desktop** (testé OK sur Edge desktop) |
| **Exclus** | **iOS / iPadOS** (tous navigateurs), Firefox, Safari — Web Bluetooth absent |
| **Brave** | Chromium, mais **Web Bluetooth désactivé par défaut** (`brave://flags` → *Web Bluetooth API*) — préférer Edge/Chrome |
| Pré-requis | **Bluetooth de Windows activé** (sinon `getAvailability` = NON, `NotFoundError` immédiat) |
| Contexte | **HTTPS obligatoire** ; `localhost` accepté ; `file://` **refusé** |

Si `navigator.bluetooth` est absent, un bandeau « navigateur non compatible »
s'affiche et le bouton *Connecter* est désactivé.

## Lancer en local

`file://` ne marche pas (Web Bluetooth exige un contexte sécurisé). Servez le
dossier en HTTP **localhost** (accepté comme contexte sécurisé) :

```bash
cd WEB/Keyboard
python -m http.server 8000
```

Puis ouvrez **http://localhost:8000** dans Chrome/Edge, et cliquez *Connecter*.

> Depuis un **téléphone Android**, `localhost` ne s'applique pas : il faut du
> **HTTPS**. Le plus simple est d'héberger le dossier (Netlify / GitHub Pages),
> ou d'utiliser un tunnel HTTPS vers le serveur local.

## Tester le lot seul (sans firmware)

Contre un **périphérique bouchon** exposant le service HID-Bridge (nRF Connect en
mode serveur GATT, ou un 2ᵉ ESP) : le **journal** de la page affiche exactement le
JSON émis pour chaque action — c'est le critère de validation §9 (mode direct,
Ctrl+C = `{"t":"char","v":"c","m":1}`, macro, séquences).

## Critères couverts (spec WEB §9)

1. Message d'incompatibilité si Web Bluetooth absent ✔
2. Connexion/déconnexion + indicateur d'état ✔
3. Mode direct → JSON attendu, modificateurs collants inclus ✔
4. Ctrl+C via Ctrl armé + `c` → `{"t":"char","v":"c","m":1}` ✔
5. Macro → un `txt` conforme (garde-fou MTU) ✔
6. Séquences prédéfinies + personnalisée conformes au socle ✔
7. Ergonomie tactile (grandes cibles, pensé smartphone) ✔

## PWA (application installable)

La page est une **PWA** : installable sur l'écran d'accueil (Android) ou en
raccourci d'application (Chrome/Edge desktop), et utilisable hors-ligne.

Fichiers ajoutés (dans ce dossier) :

- `manifest.webmanifest` — nom, icônes, `display: standalone`, couleurs.
- `sw.js` — service worker, stratégie **réseau d'abord / cache en repli**
  (toujours à jour en ligne, disponible hors-ligne).
- `pwa.js` — enregistre le SW et gère le bouton **« Installer »** (barre du haut,
  visible quand le navigateur propose l'installation).
- `icons/` — jeu d'icônes (192, 512, maskable, apple-touch, favicon).
- `.htaccess` — Apache : type MIME du manifest + anti-cache du SW (facultatif).

### Aucune URL finale requise

Tous les chemins sont **relatifs**. La portée du service worker se déduit de son
emplacement, donc la PWA fonctionne **quel que soit le dossier d'hébergement**
(racine du domaine *ou* `/un/sous/dossier/`), sans rien reconfigurer. Seule
exigence : **HTTPS** (déjà en place). Inutile de figer le chemin dès maintenant.

> Si le chemin change **après** une première installation, l'appli déjà installée
> continue de tourner, mais une réinstallation au nouveau chemin est vue comme
> une nouvelle appli (l'identité dérive de `start_url`).

### Hors Apache (Nginx)

Le `.htaccess` est ignoré par Nginx. Équivalent :

```nginx
location = /manifest.webmanifest { types { application/manifest+json webmanifest; } }
location = /sw.js { add_header Cache-Control "no-cache, no-store, must-revalidate"; }
```

Le serveur intégré `serve.py` (racine `WEB/`) convient pour tester en HTTPS.
