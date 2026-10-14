# Matériel

## Carte : ESP32-S3-WROOM-1 N8R2 (DevKit, 2 prises USB-C)

| Élément | Détail |
|---|---|
| Module | ESP32-S3-WROOM-1 **N8R2** : 8 Mo de flash externe, 2 Mo de PSRAM QSPI (inutilisée ici) |
| Prise **« USB »** | USB natif de l'ESP32-S3 (GPIO19 = D-, GPIO20 = D+), contrôleur USB-OTG en **mode hôte** → lecteur de carte |
| Prise **« COM »** | pont USB-série **WCH CH343** (VID 1A86, PID 55D3) sur UART0 (GPIO43 TX / GPIO44 RX) → protocole + flash |
| LED | WS2812 RGB sur **GPIO48** |
| Boutons | BOOT (GPIO0), RESET (EN) |

Ce firmware est **dédié à la N8R2** : il est compilé pour 8 Mo de flash. Flashé sur
une SuperMini 4 Mo, il la ferait boucler au démarrage.

## Alimentation du lecteur (VBUS) : point bloquant n°1

En mode hôte, c'est l'ESP32 qui doit fournir le 5 V au lecteur. Sur ces cartes
DevKit, la prise USB native **n'alimente généralement pas** le périphérique
branché. Une diode sur VBUS laisse le 5 V entrer (alimentation de la carte par
cette prise) mais pas sortir.

Solutions :
- ponter le cavalier à souder **« USB-OTG »** (parfois « IN-OUT ») au dos de la
  carte, s'il existe ;
- utiliser un câble OTG en Y avec une alimentation externe ;
- utiliser un hub USB alimenté (pris en charge par la pile IDF, non testé).

Contrôles :
- **au multimètre** : mesurer 5 V entre VBUS et GND sur la prise A femelle de
  l'adaptateur OTG ;
- **par le firmware** : avec un lecteur branché, le mode `DEBUG` doit afficher
  `PORT … connecte=1 … D+=1`. Si l'on voit `connecte=0 D+=0 D-=0` et la ligne
  `DIAG : aucun appareil ELECTRIQUEMENT connecte`, le lecteur n'est pas alimenté
  (ou pas branché). Voir le constat du 2026-10-14 dans [recette.md](recette.md).

Avec le cavalier ponté, ne jamais brancher la prise « USB » sur un PC : il y aurait
deux sources de 5 V.

Adaptateur : USB-C **mâle** (côté carte, prise « USB ») vers USB-A **femelle**
(côté lecteur).

## Port COM

- Flash et dialogue se font **uniquement par la prise « COM »** : la prise « USB »
  est devenue hôte.
- Les lignes DTR/RTS du CH343 pilotent EN/BOOT (auto-reset au flash). Une
  application hôte qui les bascule redémarre l'ESP32 : il faut ouvrir le port DTR/RTS
  inactifs.
- Sur le PC de développement, **COM3 n'est pas un ESP32** : c'est un écran USB
  3,5" (VID 1A86, PID 5722, puce WCH). Le script de flash filtre donc par PID,
  voir [build_flash.md](build_flash.md).

## Sobriété (chaleur, consommation, sécurité)

- BLE et Wi-Fi ne sont **jamais initialisés** : aucune bibliothèque radio n'est
  incluse, et le cœur libère la mémoire du contrôleur BT.
- CPU à **80 MHz** (FQBN `CPUFreq=80`). On ne descend pas à 40 MHz : la PLL est
  nécessaire à l'USB.
- PSRAM désactivée.
- La carte à puce est **coupée après chaque lecture**.
- LED volontairement douce (`LED_MAX` = 64/255).

## LED d'état (WS2812, GPIO48)

« 100 % » = `LED_MAX` (64/255), réglable dans `status_led.h`. Le code ne pilote
jamais la LED directement : il déclare un état (`ledSet`) ou demande un éclair
(`ledFlash`), et une tâche dédiée l'anime à ~50 Hz.

| État | LED |
|---|---|
| Démarrage, `RESTART` (délai de grâce de 2,5 s) | **bleu**, respiration lente |
| Lecteur détecté, pas de carte (et après `CARD_OUT`) | **vert**, respiration lente |
| Aucun lecteur | **rouge** fixe 50 % |
| Périphérique USB branché mais pas un lecteur CCID (ou lecteur de niveau caractère) | **orange** fixe 50 % |
| Nouvelle recherche du lecteur (toutes les 30 s sans lecteur, ~3 s) | **bleu**, respiration rapide |
| `CARD_IN` / carte lue avec succès | **vert** fixe 100 % |
| Lecture en cours | **vert** 50 %, clignotement rapide (5 Hz) |
| `CARD_READ_ERROR` | **rouge** fixe 50 % |
| `CARD_READ_NOT_CV` | **violet** fixe 50 % (rouge + bleu) |
| Pile USB en panne (`STEP=USBHOST` / `STUCK`) | **rouge** 100 %, clignotement rapide (verrouillé jusqu'au redémarrage) |

En plus :
- un **éclair blanc** bref à chaque commande reçue sur le port COM ;
- au démarrage, un **auto-test** rouge, vert, bleu (prouve les trois canaux).

« Aucun lecteur » et `CARD_READ_ERROR` ont la même couleur, comme demandé. Seul
le contexte les distingue : carte insérée ou non.
