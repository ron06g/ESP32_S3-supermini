# Lot WEB — Site « Keyboard » — Cahier des charges

> Spécification du site web de contrôle. **Mock, sans authentification.** Le contrat
> GATT et l'architecture sont dans `../../cahier-des-charges.md` (socle) — ce
> document spécifie l'**IHM** et le **client BLE**, sans redéfinir le protocole.
>
> Révision : 2026-08-30

---

## 1. Objet

Une page web qui :

- se **connecte au périphérique** S3 en **Web Bluetooth** ;
- affiche un **clavier AZERTY** cliquable ;
- offre **deux modes de saisie** :
  - **direct** : chaque touche cliquée est transmise instantanément (comportement
    d'un vrai clavier) ;
  - **macro** : une zone de texte (input box) dont le contenu est transmis **en une
    fois** au clic d'envoi.

---

## 2. Technologie & contraintes

| Élément | Choix mock |
|---|---|
| Type | Page statique (HTML/CSS/JS), aucune dépendance serveur |
| Accès BLE | **Web Bluetooth API** (`navigator.bluetooth`) |
| Contexte | **HTTPS obligatoire** ; `localhost` accepté ; `file://` **refusé** |
| Hébergement | Netlify / GitHub Pages, ou serveur local HTTPS/localhost pour tests |
| Navigateurs | **Chrome/Edge sur Android ou desktop** |
| **Exclus** | **iOS/iPadOS (tous navigateurs)**, Firefox, Safari — Web Bluetooth absent |

> Bien afficher, dans l'IHM, un **message clair si `navigator.bluetooth` est absent**
> (« navigateur non compatible ») plutôt qu'un échec silencieux.

---

## 3. Connexion BLE

- Bouton **« Connecter »** → `requestDevice` filtré sur le **service UUID** du socle
  (§5.1), puis connexion GATT, récupération du service et des caractéristiques
  **CMD** (écriture) et **STATUS** (notifications).
- S'abonner aux notifications **STATUS** ; refléter l'état dans l'IHM
  (`ready` / `busy` / `err`).
- **Indicateur de connexion** visible en permanence (pastille + nom de l'appareil).
- **Reconnexion** : gérer `gattserverdisconnected` ; proposer une reconnexion
  (Web Bluetooth n'a pas d'accès en arrière-plan : si l'onglet se ferme, la
  connexion tombe — l'assumer).
- **Geste utilisateur** requis pour la première sélection d'appareil (contrainte API).

---

## 4. IHM — clavier AZERTY

- Disposition **AZERTY visuelle** complète : rangée chiffres, `A–Z`, ponctuation,
  `Espace`, `Entrée`, `Retour arrière`, `Tab`, `Échap`.
- **Modificateurs** (Ctrl, Shift, Alt, Win, AltGr) en **touches bascules
  « collantes »** : cliquer arme le modificateur pour la frappe suivante (ou le
  maintient jusqu'au prochain clic). Retour visuel de l'état armé.
- **Touches de navigation** : flèches, Début/Fin, Page préc./suiv., Suppr.
- **Touches média** (volume, lecture/pause, muet, précédent/suivant) dans une zone
  dédiée « télécommande ».
- **Rangée de séquences** : boutons pour `ctrl_alt_del`, `ctrl_esc`, `alt_tab`,
  `alt_f4`, `win_d`.
- Adaptée **tactile** (cibles de clic assez grandes, pensée pour smartphone Android).

### Rappel de responsabilité (piège AZERTY)

Le web **n'encode pas** de code de scan : il envoie le **caractère** voulu (`char`)
ou la **touche nommée** (`key`). La traduction AZERTY → HID est **dans le firmware**
(socle §2, spec S3 §6). L'IHM AZERTY sert donc surtout à l'ergonomie ; ce qui part
sur le fil, c'est l'intention (`{"t":"char","v":"é"}`), pas une position.

---

## 5. Mode direct

- Au clic (ou appui tactile) sur une touche caractère : envoyer immédiatement
  `{"t":"char","v":"<caractère>","m":<masque des modificateurs armés>}`.
- Touche fonctionnelle : `{"t":"key","c":"<code>","a":"tap","m":<…>}`.
- Après émission, **désarmer** les modificateurs collants « one-shot » (sauf mode
  « maintien » explicite).
- Objectif de latence perçue < 150 ms (socle §6). Utiliser **Write Without Response**
  pour la frappe directe si la fiabilité observée le permet ; sinon Write.

---

## 6. Mode macro

- **Input box** multi-lignes + bouton **« Envoyer »**.
- À l'envoi : transmettre le contenu en `{"t":"txt","v":"<contenu>"}`.
- **Respecter la limite MTU** négociée : si le texte dépasse, soit refuser avec un
  message, soit fragmenter selon la politique retenue au socle §5.4 (pour le mock,
  privilégier MTU élevé + garde-fou de longueur).
- Prévu pour les **saisies répétitives** (identifiants de borne, commandes types).
- Option pratique : une petite **liste de macros mémorisées** côté page (en mémoire
  de session uniquement — pas de stockage pour le mock).

---

## 7. Éditeur de séquence (optionnel mock, utile)

- Construire une séquence personnalisée (socle §5.3) : ajouter des étapes
  `frappe` / `attente(ms)` / `répétition(n, intervalle)`, puis **« Lancer »** →
  `{"t":"seq","s":[…]}`.
- Bouton **« Stop »** → commande d'arrêt (spec S3 §5.3).
- Reproduire l'exemple de l'énoncé (1 · +1 s · 2 · +1 s · 3×/4 s) comme préréglage
  de démonstration.

---

## 8. Structure de projet (indicative)

> Description d'organisation, **pas** de code.

```
WEB/Keyboard/
├── cahier-des-charges.md
└── (site à venir)
    ├── index.html        # structure de la page + clavier AZERTY
    ├── style.css         # mise en page tactile, thème
    └── app.js            # connexion Web Bluetooth, encodage des commandes, modes
```

Aucune bibliothèque imposée. Rester **sans build** (fichiers statiques) pour la
simplicité du mock ; un framework n'apporte rien à ce stade.

---

## 9. Critères de validation du lot (isolé)

Validable **sans le firmware**, contre un **périphérique BLE bouchon** exposant le
service HID-Bridge (p. ex. un second ESP ou un simulateur GATT) qui **journalise**
les écritures reçues :

1. Détection d'incompatibilité affichée sur un navigateur sans Web Bluetooth.
2. Connexion/déconnexion propres, indicateur d'état correct.
3. Mode direct : chaque touche produit **exactement** la commande JSON attendue
   (vérifiée dans le journal du bouchon), modificateurs collants inclus.
4. Ctrl+C via modificateur armé + `c` → `{"t":"char","v":"c","m":1}`.
5. Mode macro : le texte de l'input box part en un `txt` conforme (ou fragments
   conformes si > MTU).
6. Séquences prédéfinies et personnalisée : JSON conforme au socle.
7. Ergonomie tactile validée sur un smartphone Android réel.

---

## 10. Hors périmètre (rappel)

Authentification / login du site, secret partagé, échange de clés BLE, stockage
persistant des macros, support iOS. Reportés en phase sécurité.
