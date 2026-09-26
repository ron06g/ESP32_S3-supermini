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
| **AZERTY** | clavier AZERTY complet, modificateurs **collants** (clic = armé *one-shot*, 2ᵉ = **verrou** 🔒, 3ᵉ = off) |
| **Num** | pavé numérique (chiffres, opérateurs, ⌫, Entrée, Tab…) |
| **Fn/Média** | F1–F12, Ctrl/Maj/Alt/Win/AltGr, navigation, **télécommande média** |
| **Souris** | trackpad + boutons + molette — ✅ pris en charge par le firmware (USB HID souris) |
| **Texte** | saisie au **clavier physique/OS** du téléphone, **envoi en direct** (diff → `txt` / `Backspace`) |
| **Macro** | texte envoyé en une fois (`txt`), garde-fou MTU, macros mémorisées (session) |
| **Séq.** | éditeur frappe/attente/répétition + préréglage démo + **Stop** + séquences prédéfinies |
| **GPIO** | sorties 4–7 (boutons bascule) et entrées BOOT, 8–11 (voyants) — `{"t":"gpio",…}` / `gpio:<label>:<v>` ; sur un maître, ce sont les broches de l'esclave |
| **⚙️ Réglages** (icône de l'en-tête, à côté de « Connecter » ; contient aussi le bouton **Journal**) | config persistante du module (interrupteurs HID clavier / souris / COM / GPIO / appairage → `cfg set` + redémarrage), **appairage BLE ↔ BLE** (Rechercher → Appairer, état du lien, RSSI, Désappairer) et **Test liaison** (20 pings : RTT min/moy/max, pertes) |

À la connexion, l'app envoie `{"t":"cfg","a":"get"}` ; la réponse `cfg:{…}` **masque les
onglets** des fonctions désactivées (clavier → AZERTY/Num/Fn/Texte/Macro/Séq., souris,
GPIO) et la section Appairage si `pair` est à 0. Les STATUS préfixés (`cfg:`, `scan:`,
`pair:`, `link:`, `gpio:`, `pong:`) sont des événements dispatchés dans `handleStatus`.

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
