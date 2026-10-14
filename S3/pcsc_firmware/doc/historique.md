# Historique et décisions

## Demande initiale (2026-10-14)

Nouveau firmware **totalement dédié** à l'ESP32-S3 N8R2, sans rapport avec le
firmware HID, qui peut toutefois en reprendre des idées. Emplacements demandés :

| Quoi | Où |
|---|---|
| Sources | `S3\pcsc_firmware\` |
| Scripts | `S3\pcsc_firmware_compile.bat` et `S3\pcsc_firmware_flash.bat` |
| Versions compilées | `S3\firmware\` |

Fonction :
- le port OTG reçoit un lecteur de carte à puce **PC/SC** (« Card Reader » USB) ;
- le port de debug (COM) porte un protocole simple ;
- but : **lire les données publiques d'une Carte Vitale**. Si le résultat est bon,
  on ajoutera un transfert **Base64** des données de la carte. « Pour le moment je
  veux juste savoir si ça peut fonctionner » ;
- BLE et Wi-Fi désactivés, CPU à fréquence réduite (surchauffe, sécurité).

Protocole demandé, trames `<STX>…<CR><LF><EOT>`.

Événements :

```
CARD READER v1.0
READER_OK / READER_KO
CARD_IN
CARD_READ:.....DATA PUBLIQUE DE LA CARTE VITALE.....
CARD_READ_ERROR
CARD_READ_NOT_CV:[CODE]
CARD_OUT
```

Commandes et réponses attendues :

| Commande | Réponse attendue |
|---|---|
| `RESTART` | `CARD READER v1.0` |
| `IS_READER` | `READER_OK` / `READER_KO` |
| `IS_CARD` | `CARD_IN` / `CARD_OUT` |
| `READ_CARD` / `CARD_STATE` | `NO_CARD` / `CARD_READ_ERROR` / `CARD_READ_NOT_CV:[CODE]` / `CARD_READ:…` |

`READ_CARD` permet de **forcer** une relecture.

## Décisions validées

| Sujet | Décision |
|---|---|
| Vitale 1 | reconnue par son ATR → `CARD_READ_NOT_CV:V1`, **non lue** (format non documenté) |
| `RESTART` | « redémarrage du lecteur de carte USB » : ré-énumération du lecteur, **pas** de reboot de l'ESP32 (sauf pile USB bloquée) |
| Diagnostic | commande `INFO` ajoutée, hors spécification |
| `READ_CARD` / `CARD_STATE` | `READ_CARD` relit, `CARD_STATE` renvoie le cache |
| Commande inconnue | `UNKNOWN_CMD` |
| Format de `CARD_READ` | **provisoire** `NOM=…;PRENOM=…;NAISS=…;NIR=…`, un groupe par bénéficiaire séparé par `\|` ; le Base64 viendra ensuite |

## Chronologie

1. **v1.0, 2026-10-14 (nuit)**. Commits `e194eb0` (firmware) et `c457b96`
   (CLAUDE.md).
   - Contenu :
     - pilote CCID minimal sur la pile USB Host de l'IDF 5.5 ;
     - couche APDU T=0 ;
     - séquence Vitale 2 d'après Cardpeek ;
     - protocole COM ;
     - scripts `.bat` ;
     - `tools/pcsc_term.py`.
   - Une relecture du code a conduit à 7 corrections de robustesse :
     - files USB et COM séparées ;
     - garde si la pile USB ne s'installe pas ;
     - résultat de `READ_CARD` émis une seule fois ;
     - carte coupée après chaque lecture (supprime une détection d'échange de carte
       qui pouvait boucler) ;
     - nouvelles tentatives de release / close ;
     - pile USB bloquée signalée ;
     - message CCID trop long vidé.
2. **LED RGB**, même nuit, à la demande de l'utilisateur :
   - bleu qui respire au démarrage ;
   - vert qui respire quand le lecteur est prêt ;
   - rouge 50 % sans lecteur ;
   - vert 100 % sur `CARD_IN` ;
   - vert 50 % qui clignote vite pendant la lecture ;
   - rouge 50 % sur erreur ;
   - violet sur non-Vitale.

   Ajouts proposés :
   - orange pour un appareil qui n'est pas un lecteur ;
   - rouge rapide pour une pile USB en panne ;
   - éclair blanc à chaque commande ;
   - auto-test au démarrage.
3. **2026-10-14 (soir)** : « aucun lecteur détecté ». Le protocole série fonctionnait.
   Trois demandes :
   - **commande `DEBUG`** : journal `DBG:` détaillé (état électrique du port,
     énumération, descripteurs, CCID / APDU en hexa, erreurs IDF) ;
   - **analyse des deux lecteurs** : ce sont des CCID standards (base libccid). En
     mode DEBUG, le port était électriquement vide. Une fois le Gemalto branché, il
     est reconnu en moins de 0,3 s ;
   - **recherche toutes les 30 s** sans lecteur, avec la LED en bleu rapide.

   La recette a révélé un **défaut réel** : une carte en mode spécifique (TA2)
   donnait des erreurs de parité. `SetParameters` reprend maintenant le Fi/Di de TA1.
   Détail dans [recette.md](recette.md).
4. **Documentation** regroupée dans `S3\pcsc_firmware\` (`README.md` + `doc/`), à la
   demande de l'utilisateur.

## Points ouverts

- **Valider la lecture d'une Carte Vitale 2** : la carte de l'utilisateur est une
  Vitale 1.
- Transfert **Base64** des données brutes de la carte (étape suivante prévue).
- Vitale 1 : lecture impossible sans la documentation du GIE SESAM-Vitale.
- Tester l'Identiv CLOUD 2700 R.
- Identifier ce qui empêchait la détection au premier essai (alimentation ?
  câble ? adaptateur ?).
- Accélération possible : PPS en mode négociable (Fi/Di de TA1), si la durée de
  lecture d'une Vitale 2 le justifie.
