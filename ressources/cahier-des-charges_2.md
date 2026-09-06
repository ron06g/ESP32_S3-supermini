# Lot S3 — Firmware « HID-Bridge » — Cahier des charges

> Spécification du firmware ESP32-S3. **Mock, sans sécurité.** Le contrat GATT et
> l'architecture d'ensemble sont dans `../../cahier-des-charges.md` (socle) — ce
> document ne redéfinit pas le protocole, il en spécifie la **réalisation** côté
> périphérique.
>
> Révision : 2026-08-30

---

## 1. Objet

Le firmware fait de l'ESP32-S3, simultanément :

- un **clavier USB HID** (+ Consumer Control) vu par la machine cible ;
- un **serveur BLE GATT** qui reçoit les commandes du site web ;
- un **traducteur** commande → frappe, incluant la **disposition AZERTY** et
  l'**exécution autonome des séquences**.

---

## 2. Matériel cible

| Élément | Valeur |
|---|---|
| Module | ESP32-S3-WROOM-1 **N8R2** (ou S3 SuperMini équivalent) |
| USB natif | GPIO19 (D−) / GPIO20 (D+) — **ne rien y câbler d'autre** |
| Console/flash | 2ᵉ port USB-UART si carte double-USB ; sinon USB natif (CDC/JTAG) + bouton BOOT sur SuperMini mono-port |
| LED d'état | WS2812 sur GPIO48 (indicateur de connexion, facultatif) |

> Sur **SuperMini mono-USB**, le port sert de HID vers la cible : le flash et les
> logs passent par le même port (CDC/JTAG) ou le mode BOOT. Confort de dev moindre,
> non bloquant. La carte **double-USB** garde un port libre pour la console.

---

## 3. Rôle USB

### 3.1 Interfaces exposées

| Interface HID | Usage | Remarque |
|---|---|---|
| **Keyboard** | Frappes clavier | **Boot protocol** obligatoire (BIOS/UEFI) |
| **Consumer Control** | Touches média (volume, lecture/pause…) | Requise pour l'usage télécommande |

- Descripteur clavier : rapport standard 8 octets `[modificateurs][réservé][6 codes]`.
- Pas de NKRO (inutile ici).
- VID/PID : valeurs de développement pour le mock ; **prévoir un VID/PID ou un
  numéro de série stable** pour éviter le recache de pilote Windows entre flashs.
- Nom produit USB proposé : `S3-KBD (mock)`.

### 3.2 Composition

Une seule configuration USB, deux interfaces HID (Keyboard + Consumer). Pas de CDC
sur l'USB natif si évitable (préserver le budget d'endpoints et la simplicité).

---

## 4. Rôle BLE

- Serveur GATT exposant le **service HID-Bridge** du socle (§5.1).
- Caractéristique **CMD** en écriture : point d'entrée unique des commandes.
- Caractéristique **STATUS** en notification : état minimal.
- **Négocier un MTU** suffisant à la connexion (cible ≥ 200 o) pour absorber les
  `txt` courts sans fragmentation (cf. socle §5.4).
- Une seule connexion cliente à la fois pour le mock.
- **Aucune authentification / aucun chiffrement applicatif** à ce stade (report
  explicite en phase sécurité). L'appairage « just works » du contrôleur est toléré.

### États notifiés sur STATUS (proposition minimale)

| État | Sens |
|---|---|
| `ready` | Prêt à recevoir |
| `busy` | Séquence en cours (ignorer/commander en file les commandes entrantes) |
| `err:<code>` | Commande rejetée (JSON invalide, caractère non mappé, séquence inconnue) |

---

## 5. Fonctions de traduction

### 5.1 Frappe directe — `char` et `key`

- `char` : le firmware cherche le caractère dans sa **table AZERTY** (§6), en tire
  le **code de touche HID + modificateurs** requis, applique le masque `m`
  additionnel éventuel, émet un rapport « appui » puis « relâché ».
- `key` : touche fonctionnelle nommée → code HID direct. `a` vaut :
  - `tap` : appui + relâché ;
  - `down` : appui maintenu (pour composer un raccourci touche-à-touche) ;
  - `up` : relâché.
- **Gestion de l'état des modificateurs** : maintenir un registre des modificateurs
  actuellement « down » et l'appliquer aux rapports suivants, jusqu'au `up`
  correspondant. Prévoir un **relâchement de sûreté** (tout relâcher) sur
  déconnexion BLE, pour ne pas laisser une touche « collée » sur la cible.

### 5.2 Texte-macro — `txt`

- Itérer la chaîne caractère par caractère via la table AZERTY.
- **Cadence** : insérer un court délai inter-frappe (proposition 5–10 ms) pour
  fiabiliser la réception côté cible ; l'exposer en constante ajustable.
- Gérer les caractères multi-octets UTF-8 (accentués, `€`).

### 5.3 Séquences — `seq`

- **Exécution autonome** dans une **tâche dédiée**, horloge locale : ni la latence
  ni le rythme des écritures BLE ne doivent influer sur le tempo.
- Prédéfinies : table de correspondance `n` → suite d'actions (socle §5.3).
- Personnalisées : interpréter la liste `s` (`tap` / `wait` / `rep`).
- Pendant une séquence : STATUS = `busy`. Politique proposée pour une commande
  reçue en cours de séquence : **mise en file** (simple) ou **rejet `err:busy`**.
  À trancher ; file recommandée.
- **Interruptibilité** : prévoir une commande d'arrêt (p. ex. `{"t":"seq","n":"stop"}`)
  qui vide la file et relâche tout — utile si une séquence part de travers.

---

## 6. Table de disposition AZERTY — pièce critique

> C'est le cœur de risque du lot (socle §7). À écrire et **tester en premier**.

- Table `caractère Unicode → (code de touche HID, modificateurs)` pour une cible
  **configurée en français AZERTY**.
- Couverture minimale du mock :
  - lettres `a`–`z` et `A`–`Z` ;
  - chiffres de la rangée haute (avec Shift) et leurs symboles directs
    (`& é " ' ( - è _ ç à`) ;
  - ponctuation courante : `. , ; : ! ? / § * µ % ° + = ) ]° …` selon clavier FR ;
  - **AltGr** : `@ # { } [ ] | \ € ~` ;
  - accentués usuels : `é è à ç ù ê â î ô û`, et les touches mortes si nécessaire
    (`^` `¨` → à composer, ou table directe des combinaisons courantes).
- Comportement si caractère absent de la table : ignorer + `err:unmapped` sur STATUS
  (ne jamais taper « au hasard »).
- **Point de vérification** : produire un fichier de test contenant tout le jeu de
  caractères couvert, l'envoyer en `txt`, comparer octet à octet avec ce qui arrive
  dans un éditeur sur la cible FR. C'est ce test qui valide la table, pas la
  relecture visuelle.

> Évolution possible (hors mock) : rendre la disposition **paramétrable** (US, autre)
> via le futur fichier de config. Pour le mock, AZERTY en dur.

---

## 7. Structure de projet (indicative, ESP-IDF)

> Description d'organisation, **pas** de code.

```
S3/hid_firmware/
├── cahier-des-charges.md
├── (projet ESP-IDF à venir)
│   ├── main/                 # point d'entrée, orchestration
│   ├── components/
│   │   ├── usb_hid/          # descripteurs Keyboard + Consumer, émission des rapports
│   │   ├── ble_cmd/          # serveur GATT, service HID-Bridge, parsing JSON des commandes
│   │   ├── keymap_azerty/    # table caractère → HID (la pièce critique §6)
│   │   └── sequencer/        # tâche d'exécution des séquences
│   ├── sdkconfig.defaults    # TinyUSB, BLE, MTU, tailles de pile
│   └── partitions.csv        # pas de LittleFS nécessaire au mock
```

- **Dépendances clés** : pile TinyUSB (HID composite), pile BLE (NimBLE recommandé
  pour l'empreinte), un parseur JSON léger (cJSON).
- **Cœurs** : garder BLE et USB HID sur des cœurs distincts si des micro-latences
  apparaissent (BLE cœur 0, séquenceur/HID cœur 1), comme pour l'audio isochrone
  évoqué en amont.

---

## 8. Critères de validation du lot (isolé)

Validable **sans le site web**, avec un client BLE générique (nRF Connect, etc.) :

1. La cible voit un clavier USB **au BIOS**.
2. Écriture `{"t":"char","v":"a"}` → `q`? Non : doit produire `a` sur cible FR.
   (Si `q` apparaît, la table ou la disposition cible est en cause — piège AZERTY.)
3. `{"t":"txt","v":"café à 5 €"}` reproduit fidèlement la chaîne.
4. `{"t":"key","c":"Enter","a":"tap"}` valide une ligne.
5. `{"t":"char","v":"c","m":1}` = Ctrl+C effectif.
6. Séquence personnalisée de tempo (socle §5.3) : chronométrer, tolérance ±10 %.
7. Déconnexion BLE en pleine frappe → aucune touche restée enfoncée sur la cible.
8. Touches média sur Android TV.

---

## 9. Hors périmètre (rappel)

Sécurité (appairage chiffré, clés, liste blanche), fichier de configuration en
flash, disposition paramétrable, Wi-Fi, variante BLE-HID direct pour Apple TV.
