# Évolution — Transport Wi-Fi SoftAP + portail captif — Cahier des charges

> **Document d'évolution.** Le projet clavier BLE (firmware S3 + app web) est
> **déjà fonctionnel**. Des ajustements récents ont été intégrés (gestion
> clavier/souris, site en PWA…). Ce cahier des charges **annonce les modifications**
> à apporter à l'existant pour ajouter un **second transport, Wi-Fi**, compatible
> iPhone — sans redétailler le code déjà en place.
>
> **Nature** : spécification (réflexion avant projet), pas de code source.
>
> Révision : 2026-08-30

---

## 1. Pourquoi cette évolution

Le transport **BLE** (Web Bluetooth) fonctionne sur Android et desktop, mais **reste
inaccessible depuis iOS** — aucun navigateur iPhone n'expose Web Bluetooth. On ajoute
donc un transport **Wi-Fi point-à-point** qui, lui, passe partout, **en réutilisant
tel quel le protocole de commandes JSON existant** (`char`, `key`, `txt`, `seq`,
+ souris). Aucune régression du mode BLE : les deux transports coexistent.

---

## 2. Principe du flux visé

```
1. L'utilisateur rejoint le Wi-Fi du S3          SSID « S3-KBD », WPA2
2. Le portail captif s'ouvre                     page minimaliste servie par le S3
3. Il appuie sur « Accepter »                    redirection avec paramètres
4. L'app web s'affiche, connexion activée        même app, transport = Wi-Fi
5. Les touches transitent par l'API locale       même protocole JSON qu'en BLE
```

Redirection cible (telle que spécifiée) :

```
https://php.ron06.fr/Keyboard/?D=<base64url(JSON)>
```

avec, avant encodage :

```json
{ "wifi": "192.168.4.1", "date": "2026-08-30T14:30:00" }
```

- `D` **encodé en base64url** (voir §5) : vise à **limiter la visibilité** du contenu,
  pas à le sécuriser.
- Le **JSON** est retenu pour pouvoir **transporter plus tard des informations de
  sécurité** (jeton, horodatage signé…) sans changer la forme de l'URL.

---

## 3. Modifications annoncées — vue haut niveau

> Détails d'implémentation de l'existant volontairement omis.

### Lot S3 — `./S3/hid_firmware/`

Ajouts, cohabitant avec l'USB HID et le serveur BLE déjà en place :

- **SoftAP Wi-Fi** (mode point d'accès, **sans uplink internet**), SSID `S3-KBD`,
  **WPA2** — cf. réserve §6 sur la clé.
- **Serveur HTTP** local + **portail captif** (réponses aux sondes de détection
  iOS/Android/Windows pour déclencher l'ouverture automatique).
- **Page minimaliste** « Accepter » servie par le S3, qui construit l'URL de
  redirection et son paramètre `D`.
- **API locale** (HTTP/WebSocket) qui **rejoue le protocole JSON existant** :
  le séquenceur, la table AZERTY et l'émission HID ne changent pas ; on ajoute
  seulement une **nouvelle entrée de transport** en amont du même cœur de traitement.

### Lot WEB — `./WEB/Keyboard/`

- Lecture du paramètre **`D`** au chargement (base64url → JSON), activation
  automatique du **transport Wi-Fi** vers l'IP fournie.
- Réutilisation de la **façade de transport** déjà présente : on ajoute un
  `transport-wifi` (API/WebSocket) à côté du `transport-ble` existant ; **l'IHM et
  l'encodage des commandes ne bougent pas**.
- Comportement **PWA/hors-ligne** à confirmer dans le contexte Wi-Fi (cf. §6).

---

## 4. Ce qui ne change pas

- Le **protocole de commandes JSON** (clavier, souris, séquences).
- L'**IHM** du clavier/souris.
- Le **mode BLE** et son parcours Android/desktop.
- Le cœur firmware : table AZERTY, séquenceur, émission USB HID.

C'est le principe de **façade de transport** déjà en place (à la manière de
`transport-hid` / `transport-mock` sur d'autres projets) : un transport de plus,
même contrat au-dessus.

---

## 5. Format du paramètre `D`

| Point | Règle |
|---|---|
| Encodage | **base64url** (`-` et `_` au lieu de `+` `/`, padding `=` géré/omis) — le base64 standard contient des caractères problématiques en query string |
| Contenu | Objet JSON UTF-8 |
| Champ `wifi` | IP du S3 sur le SoftAP (`192.168.4.1`) — cible de l'API locale |
| Champ `date` | Horodatage — **voir réserve §6** (le S3 n'a pas l'heure réelle hors ligne) |
| Extensibilité | Champs de sécurité ajoutés **ultérieurement** (jeton, signature) sans changer la forme |
| **Nature sécurité** | base64 = **obfuscation, pas chiffrement**. Trivialement décodable. La vraie sécurité viendra en phase dédiée. |

---

## 6. Décisions à trancher / risques à lever **avant gel**

> Ces points ne sont pas des détails : deux d'entre eux déterminent si le mode Wi-Fi
> **fonctionne ou non**. À valider empiriquement tôt, comme d'habitude.

### 6.1 ⚠️ Contenu mixte : page HTTPS → appareil local en HTTP (bloquant)

Une page servie en **HTTPS** (`https://php.ron06.fr/...`) qui tente d'ouvrir une
connexion **`http://192.168.4.1`** ou **`ws://192.168.4.1`** est **bloquée par le
navigateur** (mixed content actif). C'est valable sur Chrome, Safari, Firefox. Le
carve-out « localhost » ne s'applique **pas** à `192.168.4.1`.

**Conséquence** : le flux « app HTTPS externe qui pilote l'API locale » **ne peut pas
fonctionner tel quel**. Trois résolutions possibles :

| # | Résolution | Effet | Coût |
|---|---|---|---|
| **A (recommandée mock)** | **Servir l'app depuis le S3 en HTTP** (`http://192.168.4.1/Keyboard/?D=…`). Même origine HTTP → `ws://192.168.4.1` autorisé. | Marche hors-ligne, marche **sur iOS**, pas de mixed content | Embarquer un build de l'app en flash (N8 = 8 Mo, large) |
| B | Garder l'origine **HTTPS externe** mais servir l'API du S3 en **`wss://` avec un vrai certificat** pour un nom (`kbd.ron06.fr`) que le **DNS du portail captif** résout vers `192.168.4.1` | Conserve HTTPS et la PWA unique | Certificat LE embarqué + renouvellement + TLS sur l'ESP : lourd |
| C | Uplink internet sur le SoftAP | Rend `php.ron06.fr` joignable en ligne | Contredit « sans partage internet » |

**Recommandation** : pour le mock, **résolution A**. `php.ron06.fr` reste la source
canonique / l'outil de mise à jour de l'app ; le firmware en embarque une copie
servie localement. Le paramètre `D` est alors passé en local
(`http://192.168.4.1/Keyboard/?D=…`). La résolution B reste la cible si l'on tient
absolument à l'origine HTTPS unique — c'est jouable avec ton infra (NPM, certs
`ron06.fr`, DNS du captif), mais hors périmètre mock.

### 6.2 ⚠️ Chargement hors-ligne dans le portail captif (surtout iOS)

Sur SoftAP **sans internet**, une redirection vers `https://php.ron06.fr` ne charge
que si l'app est **déjà en cache PWA**. Or le **mini-navigateur captif d'iOS (CNA)**
n'exécute pas les service workers et n'a pas d'accès réseau : la cible externe **ne
se chargera pas** dans la fenêtre captive. Il faut « **sortir vers Safari** », ce qui
est peu fiable.

**Conséquence** : encore un argument pour la **résolution A** (page locale servie par
le S3), qui s'affiche dans le captif **sans dépendre d'internet ni du cache**.

### 6.3 ⚠️ Longueur de la clé WPA2

**WPA2-PSK impose 8 à 63 caractères.** La valeur `"apikey"` (6 caractères) **sera
refusée** par la pile Wi-Fi. Choisir une clé **≥ 8 caractères** (p. ex. `apikey00`
ou une vraie valeur). À décider avant gel.

### 6.4 Heure du S3 hors ligne

Sans uplink, **pas de NTP** : le S3 ne connaît pas la date réelle. Le champ `date`
serait donc relatif au démarrage ou fictif. Options mock : l'omettre, y mettre
l'uptime, ou laisser l'app web renseigner l'heure du **téléphone** après connexion.
À trancher (mineur).

### 6.5 Autres points

| Point | Règle |
|---|---|
| Perte d'internet côté téléphone | En rejoignant le SoftAP, le téléphone **perd internet** (bascule 4G pour le reste). Normal, à assumer. |
| Détection captive | Répondre aux sondes iOS (`captive.apple.com`), Android (`generate_204`), Windows (`msftconnecttest`) pour l'ouverture auto. |
| IP fixe vs mDNS | Viser l'**IP fixe** `192.168.4.1`, plus fiable que `.local` en captif. |
| Ouverture de l'AP | WPA2 « partagée » = barrière minimale du mock : **toute personne à portée avec la clé peut taper sur la cible**. La vraie autorisation vient en phase sécurité. |

---

## 7. Périmètre

### Dans le périmètre (mock)

Nouveau transport Wi-Fi de bout en bout : SoftAP WPA2, portail captif + page
« Accepter », service local de l'app (résolution A recommandée), paramètre `D`
base64url/JSON, API HTTP/WebSocket rejouant le protocole JSON, réutilisation de la
façade de transport et de l'IHM existantes.

### Hors périmètre (reporté en phase sécurité)

Authentification réelle du site, **contenu chiffré de `D`** (jeton/signature),
échange de clés, appairage BLE sécurisé, résolution B (HTTPS local par certificat).

---

## 8. Critères de recette

Depuis un **iPhone** (cible de l'évolution) et un Android :

1. Le réseau `S3-KBD` apparaît, la connexion WPA2 réussit (clé ≥ 8 car.).
2. Le portail captif s'ouvre et affiche la page « Accepter ».
3. « Accepter » amène à l'app **avec la connexion active** (résolution A : page
   locale ; pas d'écran blanc dû au mixed content ou au hors-ligne).
4. Le paramètre `D` est décodé (base64url → JSON) et l'app cible bien
   `192.168.4.1`.
5. Frappe directe, macro, séquences et souris fonctionnent **à l'identique** du mode
   BLE (même protocole JSON) sur la cible USB.
6. Le mode **BLE reste fonctionnel** sans régression (Android/desktop).
7. Bascule d'un transport à l'autre sans redémarrer le firmware.
