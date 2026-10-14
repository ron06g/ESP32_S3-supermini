#!/usr/bin/env python3
"""Terminal de test du firmware pcsc_firmware (prise « COM » de l'ESP32-S3 N8R2).

Affiche chaque trame recue <STX>TEXTE<CR><LF><EOT> sur une ligne horodatee
(« << TEXTE »). Les octets hors trame (log ROM de l'ESP32 au reset materiel)
sont affiches a part (« ~~ »). Une commande saisie puis Entree est envoyee
tramee (« >> COMMANDE »).

Le port est ouvert DTR/RTS inactifs : sur le pont USB-serie de la carte, ces
lignes pilotent EN/BOOT (auto-reset) ; les basculer redemarrerait l'ESP32.

Usage :
    python pcsc_term.py COM12                    # interactif (Ctrl+C pour quitter)
    python pcsc_term.py COM12 IS_READER INFO     # envoie, attend les reponses, quitte
    python pcsc_term.py COM12 --wait 8 READ_CARD
    python pcsc_term.py COM12 --listen 120 --log debug.txt DEBUG   # journal DEBUG 2 min

Dependance : pyserial (pip install pyserial).
"""
import argparse
import sys
import threading
import time

import serial

STX, ETX, EOT = 0x02, 0x03, 0x04
LOG = None                 # fichier journal (--log), sinon None


def out(line: str) -> None:
    print(line, flush=True)
    if LOG:
        LOG.write(line + "\n")
        LOG.flush()


def stamp() -> str:
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"


def open_port(port: str, baud: int) -> serial.Serial:
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.05
    s.dtr = False          # AVANT open() : pas d'impulsion sur EN / BOOT
    s.rts = False
    s.open()
    return s


def reader(s: serial.Serial, stop: threading.Event) -> None:
    frame = None           # bytearray pendant une trame, None hors trame
    noise = bytearray()

    def flush_noise() -> None:
        txt = noise.decode("ascii", "replace").strip()
        if txt:
            out(f"{stamp()} ~~ {txt}")
        noise.clear()

    while not stop.is_set():
        data = s.read(256)
        if not data:
            if noise:
                flush_noise()
            continue
        for b in data:
            if b == STX:
                flush_noise()
                frame = bytearray()
            elif frame is not None:
                if b in (EOT, ETX):
                    out(f"{stamp()} << {frame.decode('ascii', 'replace').rstrip()}")
                    frame = None
                else:
                    frame.append(b)
            else:
                if b in (0x0D, 0x0A):
                    flush_noise()
                else:
                    noise.append(b)


def send(s: serial.Serial, cmd: str) -> None:
    out(f"{stamp()} >> {cmd}")
    s.write(bytes([STX]) + cmd.encode("ascii", "replace") + b"\r\n" + bytes([EOT]))


def main() -> int:
    ap = argparse.ArgumentParser(description="Terminal de test du firmware pcsc_firmware")
    ap.add_argument("port", help="port COM de la prise COM de la carte (ex. COM12)")
    ap.add_argument("cmds", nargs="*", help="commandes a envoyer puis quitter (sinon interactif)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wait", type=float, default=3.0, help="attente apres chaque commande (s)")
    ap.add_argument("--listen", type=float, default=0.0,
                    help="apres les commandes, rester a l'ecoute N secondes (journal, branchements...)")
    ap.add_argument("--log", help="ajoute aussi chaque ligne a ce fichier")
    args = ap.parse_args()

    global LOG
    if args.log:
        LOG = open(args.log, "a", encoding="utf-8")

    try:
        s = open_port(args.port, args.baud)
    except serial.SerialException as e:
        print(f"Ouverture de {args.port} impossible : {e}", file=sys.stderr)
        return 1

    stop = threading.Event()
    th = threading.Thread(target=reader, args=(s, stop), daemon=True)
    th.start()
    try:
        if args.cmds:
            time.sleep(0.3)
            for c in args.cmds:
                send(s, c)
                time.sleep(args.wait)
            time.sleep(args.listen)
        else:
            print("Commandes : RESTART  IS_READER  IS_CARD  READ_CARD  CARD_STATE  INFO  DEBUG   (Ctrl+C : quitter)")
            while True:
                line = input().strip()
                if line:
                    send(s, line)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        th.join(timeout=1)
        s.close()
        if LOG:
            LOG.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
