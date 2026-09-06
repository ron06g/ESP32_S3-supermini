# Clavier HID piloté en BLE — Cahier des charges (socle)

> **Statut : prototype / mock.** Aucune sécurité à ce stade (ni authentification du
> site, ni échange de clés BLE). Ces éléments sont explicitement reportés — voir
> §8. Objectif du prototype : **prouver la chaîne de bout en bout**.
>
> **Nature du document** : spécification (réflexion avant projet). Il ne contient
> pas de code source ; il décrit ce que chaque lot doit faire et, surtout, le
> **contrat d'interface** qui les relie.
>
> Révision : 2026-08-30

---

## 1. Vision

Un petit périphérique (ESP32-S3) se branche en **USB sur une machine cible** et s'y
fait passer pour un **clavier**. Un **site web**, ouvert sur un téléphone ou un
portable, se connecte au périphérique **en Bluetooth LE** et lui envoie ce qu'il
faut taper. Le périphérique traduit et frappe sur la cible.

```
  ┌─────────────┐   BLE (GATT)   ┌──────────────┐   USB HID    ┌────────────┐
  │  Site web   │ ─────────────► │  ESP32-S3    │ ───────────► │   Cible    │
  │ (navigateur)│   commandes    │  (firmware)  │  frappes     │ PC/TV/borne│
  └─────────────┘                └──────────────┘              └────────────┘
     LOT « WEB »                    LOT « S3 »
```

### Cas d'usage visés

| Cas | Cible USB | Particularité |
|---|---|---|
| Contrôle à distance de mon PC | PC (Windows/Linux) | Cas de référence, le plus simple |
| Borne professionnelle | PC de la borne | Saisie rapide, séquences prédéfinies |
| Remplacement de télécommande | Android TV / Apple TV | Navigation + touches média (cf. pièges §7) |

---

## 2. Découpage en lots

**Deux lots indépendants, un seul contrat entre eux : le protocole GATT (§5).**
Chaque lot est validable seul contre ce contrat (le firmware avec un client BLE
générique type nRF Connect ; le web avec un périphérique BLE bouchon).

| Lot | Dossier | Responsabilité |
|---|---|---|
| **S3** | `./S3/hid_firmware/` | Serveur BLE + clavier USB HID + exécution des séquences + **table de disposition AZERTY** |
| **WEB** | `./WEB/Keyboard/` | Client BLE (Web Bluetooth) + IHM clavier AZERTY + modes saisie directe / macro |

**Décision structurante — où vit la disposition clavier :** la traduction
« caractère → code de touche HID » est **entièrement dans le firmware** (lot S3).
Le web envoie une **intention** (le caractère `é`, ou la touche nommée `Enter`),
jamais un code de scan brut. Justification en §7 (piège AZERTY).

---

## 3. Périmètre du mock

### Dans le périmètre

- Énumération USB en clavier HID (protocole **boot** pour compatibilité BIOS/UEFI).
- Interface **Consumer Control** USB pour les touches média (volume, lecture/pause)
  — nécessaire pour l'usage télécommande.
- Serveur GATT BLE avec le service défini en §5.
- Trois familles de commandes : **frappe directe**, **texte-macro**, **séquences**.
- Site web statique connecté en Web Bluetooth, clavier AZERTY, deux modes de saisie.

### Hors périmètre (reporté)

| Élément | Renvoi |
|---|---|
| Authentification du site web | Phase sécurité |
| Appairage chiffré / échange de clés BLE | Phase sécurité |
| Fichier de configuration en flash (LittleFS) | Non nécessaire au mock |
| Wi-Fi, interface Ethernet USB | Autre projet |
| Support iOS du site web | Impossible (cf. §7) |

---

## 4. Cibles et compatibilité

| Cible | Clavier USB HID | Touches média (Consumer) | Réserve |
|---|---|---|---|
| PC Windows/Linux | ✅ | ✅ | Disposition cible = **FR AZERTY** requise (cf. §7) |
| Android TV | ✅ si port USB présent | ✅ en général | Navigation par flèches + Entrée + Retour |
| Apple TV | ⚠️ pas de port USB-A standard | — | **À valider** ; variante BLE-HID direct envisageable hors mock |
| Borne | ✅ | selon besoin | Cas PC classique |

> Le **contrôleur** (la machine qui ouvre le site web) doit faire tourner un
> navigateur compatible Web Bluetooth : **Chrome/Edge sur Android ou desktop**.
> **iOS est exclu** (cf. §7).

---

## 5. Contrat d'interface — Service GATT BLE

C'est **la** frontière entre les deux lots. Toute évolution se négocie ici.

### 5.1 Service et caractéristiques

| Élément | UUID (à régénérer avant gel) | Propriétés |
|---|---|---|
| Service « HID-Bridge » | `9f1d0000-5b8e-4a4a-9c2a-2b7f3e6a1001` | — |
| Caractéristique **CMD** | `9f1d0001-5b8e-4a4a-9c2a-2b7f3e6a1001` | Write, Write Without Response |
| Caractéristique **STATUS** | `9f1d0002-5b8e-4a4a-9c2a-2b7f3e6a1001` | Notify |

- Le web **écrit** ses commandes sur **CMD**.
- Le firmware **notifie** un état minimal sur **STATUS** (accusé, occupé, erreur).
  Facultatif pour un premier mock, mais réserver la caractéristique dès maintenant.
- Nom d'annonce BLE proposé : `S3-KBD`. Filtrage `requestDevice` sur le **service UUID**.

### 5.2 Format des commandes (payload de CMD)

**JSON UTF-8.** Compact, lisible au débogage. Un objet par écriture (hors chunking
§5.4). Champ `t` = type.

| `t` | Sens | Champs | Exemple |
|---|---|---|---|
| `char` | Taper **un caractère** (mode direct) | `v` (le caractère), `m` (masque modif., optionnel) | `{"t":"char","v":"a"}` |
| `txt` | Taper **une chaîne** (mode macro) | `v` (la chaîne) | `{"t":"txt","v":"Bonjour"}` |
| `key` | Touche **fonctionnelle nommée** | `c` (nom), `m` (modif.), `a` (`tap`/`down`/`up`) | `{"t":"key","c":"Enter","a":"tap"}` |
| `seq` | **Séquence** prédéfinie ou personnalisée | `n` (nom) **ou** `s` (liste d'étapes) | voir §5.3 |

**Masque de modificateurs `m`** (entier, OU binaire) :

| Bit | Valeur | Modificateur |
|---|---|---|
| 0 | 1 | Ctrl (gauche) |
| 1 | 2 | Shift (gauche) |
| 2 | 4 | Alt (gauche) |
| 3 | 8 | GUI / Win / Cmd (gauche) |
| 4 | 16 | AltGr (Ctrl droit) |

Exemple Ctrl+C (le `c` est un caractère AZERTY) : `{"t":"char","v":"c","m":1}`.

**Touches nommées `c`** (liste minimale du mock, alignée sur `KeyboardEvent.code`) :
`Enter`, `Escape`, `Backspace`, `Tab`, `Space`, `Delete`,
`ArrowUp`, `ArrowDown`, `ArrowLeft`, `ArrowRight`,
`Home`, `End`, `PageUp`, `PageDown`,
`F1`…`F12`,
modificateurs seuls pour `down`/`up` : `ControlLeft`, `ShiftLeft`, `AltLeft`,
`MetaLeft`, `AltRight`.

**Touches média (Consumer Control)**, préfixe `Media` :
`MediaPlayPause`, `MediaVolumeUp`, `MediaVolumeDown`, `MediaMute`,
`MediaNext`, `MediaPrevious`, `MediaHome`, `MediaBack`.

### 5.3 Séquences

**Prédéfinies** (le firmware les connaît par leur nom `n`) :

| `n` | Effet |
|---|---|
| `ctrl_alt_del` | Ctrl+Alt+Suppr |
| `ctrl_esc` | Ctrl+Échap |
| `alt_tab` | Alt+Tab |
| `alt_f4` | Alt+F4 |
| `win_d` | Win+D (afficher le bureau) |

**Personnalisées** (liste d'étapes `s`, exécutées **par le firmware** avec sa propre
horloge — la latence BLE ne doit pas fausser le tempo) :

| Étape | Sens |
|---|---|
| `{"tap":"<c ou char>","m":<modif>}` | Frappe une touche/caractère |
| `{"wait":<ms>}` | Attente en millisecondes |
| `{"rep":<n>,"every":<ms>,"tap":"<…>"}` | Répète `n` fois, avec `every` ms d'intervalle |

Exemple de l'énoncé — « 1, +1 s, 2, +1 s, puis 3 tapé 3× toutes les 4 s » :

```json
{"t":"seq","s":[
  {"tap":"1"},
  {"wait":1000},
  {"tap":"2"},
  {"wait":1000},
  {"rep":3,"every":4000,"tap":"3"}
]}
```

### 5.4 Chunking (textes longs)

Le MTU BLE par défaut (~20 o utiles) est vite dépassé par un `txt`. Deux options,
à trancher dans les specs de lot :

- **Négocier un MTU élevé** (jusqu'à 512 o) à la connexion — suffisant pour le mock.
- **Fragmenter** : préfixer chaque écriture d'un entête `{"seq":i,"last":bool}`.
  Réservé si des textes > MTU sont attendus.

Choix mock : **négociation MTU** + garde-fou « une commande ne doit pas dépasser
le MTU négocié ». Le fragmentation est notée comme évolution.

---

## 6. Critères de recette (bout en bout)

Le prototype est validé quand, depuis le site web sur un téléphone Android :

1. La cible reconnaît un clavier USB **dès le BIOS/UEFI** (test hors OS).
2. Mode direct : taper `bonjour` produit `bonjour` sur la cible FR.
3. `Ctrl+C` / `Ctrl+V` fonctionnent (modificateur + caractère).
4. Une **input box** envoyée en macro reproduit fidèlement un texte accentué
   (`café à 5 € — l'été`).
5. Chaque séquence prédéfinie agit (Alt+Tab bascule, Ctrl+Alt+Suppr déclenche).
6. La séquence personnalisée de §5.3 respecte les temporisations.
7. Touches média : volume et lecture/pause agissent sur une Android TV.
8. Latence perçue en frappe directe « acceptable » (objectif < 150 ms bout en bout).

---

## 7. Pièges & points d'attention

| Piège | Règle |
|---|---|
| **Disposition AZERTY** | Un clavier USB envoie des **codes de position**, pas des caractères. Le caractère obtenu dépend de la disposition **configurée sur la cible**. Le firmware embarque une table AZERTY et **suppose la cible en FR**. C'est le **risque de validation n°1** : à tester tôt. |
| **Web Bluetooth sur iOS** | **Aucun** navigateur iOS ne supporte Web Bluetooth (moteur WebKit imposé). Le contrôleur doit être Android ou desktop sous Chrome/Edge. Ne pas promettre l'iPhone. |
| **Contexte sécurisé** | Web Bluetooth exige HTTPS. `localhost` est accepté ; `file://` **non**. Prévoir un hébergement (Netlify/GitHub Pages) ou un serveur local. |
| **Protocole boot** | Pour agir dans le BIOS/UEFI, le descripteur HID doit fonctionner en **boot protocol** (rapport clavier standard 8 octets). Le NKRO n'est pas requis. |
| **Touches média ≠ clavier** | Volume et lecture/pause ne sont **pas** des touches clavier : elles relèvent de la page **Consumer Control**. Sans interface Consumer, pas de télécommande TV complète. |
| **Apple TV** | Pas de port USB-A standard : l'usage USB HID est incertain. À valider ; sinon, variante **BLE-HID direct** (le S3 s'appaire à la TV comme clavier Bluetooth) — hors mock car elle change l'architecture. |
| **Tempo des séquences** | L'horloge des séquences doit être **côté firmware**, pas côté web : sinon la latence BLE déforme les délais. |
| **BadUSB** | Ce périphérique **est** un injecteur de frappes. Tant que la sécurité n'est pas en place, ne le laisser branché que sur des machines de confiance. |

---

## 8. Feuille de route

- [ ] **Lot S3** : firmware validé seul avec un client BLE générique.
- [ ] **Lot WEB** : IHM validée seule contre un périphérique BLE bouchon.
- [ ] Intégration : recette §6.
- [ ] **Phase sécurité** (après succès du mock) :
  - Appairage BLE chiffré (LE Secure Connections) + liste blanche d'appareils.
  - Échange de clés / challenge applicatif avant acceptation des commandes.
  - Authentification du site web.

---

## 9. Arborescence des livrables

```
.
├── cahier-des-charges.md            # ce document (socle + contrat GATT)
├── S3/
│   └── hid_firmware/
│       └── cahier-des-charges.md    # spec du firmware
└── WEB/
    └── Keyboard/
        └── cahier-des-charges.md    # spec du site web
```
