#!/usr/bin/env python3
"""Terminal du firmware pcsc_firmware (lecteur de Carte Vitale sur ESP32-S3 N8R2).

Deux modes dans le meme fichier :

  - GRAPHIQUE (par defaut : sans argument, ou executable compile) : fenetre
    Windows avec choix du port COM, tuiles de commande, terminal colore des
    trames (filtres DBG / bruit / horodatage, enregistrement) et panneau lateral
    qui dessine le lecteur et la carte (port ferme, recherche, lecteur absent,
    pret, carte inseree, lecture, Carte Vitale lue, erreur, pas une Vitale),
    avec les donnees decodees et la LED deduite du module ;
  - LIGNE DE COMMANDE (un port en argument) : terminal texte, pour les scripts
    et les captures DEBUG.

Usage :
    python pcsc_term.py                                        # interface graphique
    python pcsc_term.py --demo                                 # interface + module simule (sans materiel)
    python pcsc_term.py COM12                                  # terminal texte (Ctrl+C : quitter)
    python pcsc_term.py COM12 IS_READER INFO                   # envoie, attend, quitte
    python pcsc_term.py COM12 --listen 300 --log debug.txt "DEBUG ON"
Executable autonome : build_exe.bat (PyInstaller) -> dist/pcsc_term.exe

Le port est ouvert DTR/RTS inactifs : sur le pont USB-serie de la carte, ces
lignes pilotent EN/BOOT (auto-reset) ; les basculer redemarrerait l'ESP32.

Dependance : pyserial (pip install pyserial). Interface : tkinter (fourni avec Python).
"""
import argparse
import json
import os
import queue
import struct
import sys
import threading
import time

import serial

STX, ETX, EOT = 0x02, 0x03, 0x04
BAUD = 115200
APP_TITLE = "Lecteur Carte Vitale"
APP_VERSION = "2.0"

# Ponts USB-serie reconnus (meme filtre que pcsc_firmware_flash.bat) : la N8R2
# expose un CH343 (1A86:55D3). COM3 (1A86:5722) est un ecran USB, pas un ESP32.
BRIDGE_IDS = (
    {(0x1A86, p) for p in (0x7522, 0x7523, 0x5523, 0x55D2, 0x55D3, 0x55D4, 0x55D5,
                           0x55D6, 0x55D7, 0x55D8, 0x55D9)}
    | {(0x10C4, 0xEA60), (0x10C4, 0xEA70)}
    | {(0x0403, p) for p in (0x6001, 0x6010, 0x6011, 0x6014, 0x6015)}
)


# =============================================================================
#  Noyau commun : port serie et tramage
# =============================================================================
def open_port(port: str, baud: int = BAUD) -> serial.Serial:
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.05
    s.dtr = False          # AVANT open() : pas d'impulsion sur EN / BOOT
    s.rts = False
    s.open()
    return s


def encode_cmd(cmd: str) -> bytes:
    return bytes([STX]) + cmd.encode("ascii", "replace") + b"\r\n" + bytes([EOT])


class FrameParser:
    """Decoupe le flux en trames STX...EOT ; le reste est du bruit (log ROM)."""

    def __init__(self) -> None:
        self.frame = None          # bytearray pendant une trame, None hors trame
        self.noise = bytearray()

    def flush_noise(self) -> list:
        txt = self.noise.decode("ascii", "replace").strip()
        self.noise.clear()
        return [("noise", txt)] if txt else []

    def feed(self, data: bytes) -> list:
        out = []
        for b in data:
            if b == STX:
                out += self.flush_noise()
                self.frame = bytearray()
            elif self.frame is not None:
                if b in (EOT, ETX):
                    out.append(("frame", self.frame.decode("ascii", "replace").rstrip()))
                    self.frame = None
                else:
                    self.frame.append(b)
            elif b in (0x0D, 0x0A):
                out += self.flush_noise()
            else:
                self.noise.append(b)
        return out


def list_ports() -> list:
    """[(device, description, prefere)] tries : ponts USB-serie d'abord."""
    from serial.tools import list_ports as lp
    res = []
    for p in lp.comports():
        pref = (p.vid, p.pid) in BRIDGE_IDS
        res.append((p.device, p.description or "", pref))
    res.sort(key=lambda r: (not r[2], int("".join(c for c in r[0] if c.isdigit()) or 0)))
    return res


def stamp(t: float = None) -> str:
    t = time.time() if t is None else t
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"


class DemoPort:
    """Module SIMULE (option --demo) : repond comme le firmware, sans materiel.

    Memes methodes que serial.Serial pour ce qu'utilise l'interface (read,
    write, cancel_read, close). Commandes de simulation en plus : DEMO VITALE,
    DEMO BANK, DEMO V1, DEMO ERROR, DEMO OUT, DEMO UNPLUG, DEMO PLUG. Les donnees
    de la « Carte Vitale » simulee sont fictives.
    """
    RESULTS = {
        "VITALE": ("CARD_READ:NOM=MARTIN;PRENOM=CAMILLE;NAISS=19850412;NIR=285047512345612"
                   "|NOM=MARTIN;PRENOM=LEO;NAISS=20150908;NIR=115097512345634",
                   "3B751300004909EA9000"),
        "BANK": ("CARD_READ_NOT_CV:CB;RESEAU=CB+VISA;LIBELLE=VISA DEBIT;NOM=MARTIN;PRENOM=CAMILLE;"
                 "NUM=497010******1234", "3BFF13000010003101F1564011001900000000000000"),
        "CPS": ("CARD_READ_NOT_CV:CPS;NOM=DURAND;PRENOM=ALICE;TITULAIRE=ALICE DURAND;"
                "ETAB=CABINET MEDICAL DEMO;IDENT=810000000000;PROFESSION=MEDECIN",
                "3BAC00402A001225006480000310009000"),
        "OPENPGP": ("CARD_READ_NOT_CV:OPENPGP;NOM=MARTIN;PRENOM=CAMILLE;NUM=0005-0000ABCD",
                    "3BDA18FF81B1FE751F030031C573C001400090000C"),
        "PIV": ("CARD_READ_NOT_CV:PIV;TITULAIRE=CAMILLE MARTIN;ETAB=DEMO CORP;"
                "NUM=0123456789ABCDEF0123456789ABCDEF", "3B8880014A434F503231B3"),
        "ID": ("CARD_READ_NOT_CV:ID", "3B8880014E41545249434B5F"),
        "V1": ("CARD_READ_NOT_CV:V1", "3F652500A4096A9000"),
        "ERROR": ("CARD_READ_ERROR", "3B751300004909EA9000"),
    }

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.out = []                 # (instant d'emission, octets)
        self.t0 = time.time()
        self.reader, self.card, self.debug = True, None, False
        self.result, self.atr = "", ""
        self._raw(b"ESP-ROM:esp32s3-20210327\r\nrst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n", 0.05)
        self._emit("CARD READER v1.0", 0.3)
        self._emit("READER_OK", 0.9)

    # -- file d'emission ---------------------------------------------------------
    def _raw(self, data: bytes, delay: float) -> None:
        with self.lock:
            self.out.append((time.time() + delay, data))
            self.out.sort(key=lambda x: x[0])

    def _emit(self, text: str, delay: float = 0.03) -> None:
        self._raw(encode_cmd(text), delay)

    def _dbg(self, text: str, delay: float) -> None:
        if self.debug:
            t = time.time() + delay - self.t0
            self._emit(f"DBG:{t:.3f} {text}", delay)

    # -- interface serial ----------------------------------------------------------
    def read(self, n: int = 256) -> bytes:
        end = time.time() + 0.05
        while True:
            now = time.time()
            with self.lock:
                due = [d for t, d in self.out if t <= now]
                self.out = [(t, d) for t, d in self.out if t > now]
            if due or now >= end:
                return b"".join(due)
            time.sleep(0.01)

    def write(self, data: bytes) -> int:
        for part in data.split(bytes([STX]))[1:]:
            cmd = part.split(b"\r")[0].split(bytes([EOT]))[0].decode("ascii", "replace").strip().upper()
            if cmd:
                self._handle(cmd)
        return len(data)

    def cancel_read(self) -> None:
        pass

    def close(self) -> None:
        pass

    # -- comportement du firmware ----------------------------------------------------
    def _insert(self, kind: str, delay: float) -> None:
        if self.card:
            self._emit("CARD_OUT", delay)
            delay += 0.4
        self.card = kind
        self.result, self.atr = self.RESULTS[kind]
        self._dbg("SLOT : bmICCStatus 1 (carte presente inactive)", delay)
        self._emit("CARD_IN", delay + 0.01)
        self._dbg("LECTURE : debut", delay + 0.02)
        self._dbg(f"CARTE ATR [{len(self.atr) // 2}] {self.atr}", delay + 0.3)
        self._dbg("APDU> [16] 00A404000BD250000002564954414C45", delay + 0.35)
        if kind == "BANK":
            self._dbg("CARTE : carte bancaire (annuaire EMV 1PAY.SYS.DDF01)", delay + 0.6)
        self._emit(self.result, delay + 0.9)

    def _handle(self, cmd: str) -> None:
        if cmd == "RESTART":
            self._emit("CARD READER v1.0", 0.6)
            if self.reader:
                self._emit("READER_OK", 1.2)
                if self.card:
                    self._emit("CARD_IN", 1.4)
                    self._emit(self.result, 2.0)
            else:
                self._emit("READER_KO", 3.1)
        elif cmd == "IS_READER":
            self._emit("READER_OK" if self.reader else "READER_KO")
        elif cmd == "IS_CARD":
            self._emit("CARD_IN" if self.card else "CARD_OUT")
        elif cmd == "READ_CARD":
            if not (self.reader and self.card):
                self._emit("NO_CARD")
            else:
                self._dbg("LECTURE : debut", 0.02)
                self._emit(self.result, 0.8)
        elif cmd == "CARD_STATE":
            self._emit(self.result if self.card else "NO_CARD")
        elif cmd == "INFO":
            if self.reader:
                self._emit(f"INFO:READER=08E6:3437;NAME=USB SmartCard Reader;LEVEL=TPDU;FEAT=00010230;"
                           f"ATR={self.atr or 'NONE'};STEP=NONE;SW={'9000' if self.card else '0000'};MS=640")
            else:
                self._emit("INFO:READER=NONE;LASTDEV=NONE;STEP=NONE")
        elif cmd in ("DEBUG", "DEBUG ON", "DEBUG OFF"):
            self.debug = (not self.debug) if cmd == "DEBUG" else (cmd == "DEBUG ON")
            self._emit("DEBUG_ON" if self.debug else "DEBUG_OFF")
            self._dbg("FIRMWARE CARD READER v1.0 (simulation --demo)", 0.05)
            self._dbg("PORT hprt=0x00021405 alim=1 connecte=1 actif=1 vitesse=FS D+=1 D-=0 surintensite=0", 0.06)
        elif cmd.startswith("DEMO "):
            what = cmd[5:]
            if what in self.RESULTS and self.reader:
                self._insert(what, 0.05)
            elif what == "OUT" and self.card:
                self.card, self.result, self.atr = None, "", ""
                self._emit("CARD_OUT", 0.05)
            elif what == "UNPLUG" and self.reader:
                if self.card:
                    self._emit("CARD_OUT", 0.05)
                self.reader, self.card, self.result, self.atr = False, None, "", ""
                self._dbg("USB : DEV_GONE (le lecteur)", 0.06)
                self._emit("READER_KO", 0.07)
            elif what == "PLUG" and not self.reader:
                self.reader = True
                self._dbg("ENUM : appareil qui repond 08E6:3437", 0.3)
                self._emit("READER_OK", 0.4)
        else:
            self._emit("UNKNOWN_CMD")


# =============================================================================
#  Mode ligne de commande (inchange : captures DEBUG, scripts)
# =============================================================================
LOG = None                 # fichier journal (--log), sinon None


def out(line: str) -> None:
    print(line, flush=True)
    if LOG:
        LOG.write(line + "\n")
        LOG.flush()


def cli_reader(s: serial.Serial, stop: threading.Event) -> None:
    parser = FrameParser()
    while not stop.is_set():
        data = s.read(256)
        items = parser.feed(data) if data else parser.flush_noise()
        for kind, txt in items:
            out(f"{stamp()} {'<<' if kind == 'frame' else '~~'} {txt}")


def cli_send(s: serial.Serial, cmd: str) -> None:
    out(f"{stamp()} >> {cmd}")
    s.write(encode_cmd(cmd))


def run_cli(argv: list) -> int:
    ap = argparse.ArgumentParser(description="Terminal texte du firmware pcsc_firmware")
    ap.add_argument("port", help="port COM de la prise COM de la carte (ex. COM12)")
    ap.add_argument("cmds", nargs="*", help="commandes a envoyer puis quitter (sinon interactif)")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--wait", type=float, default=3.0, help="attente apres chaque commande (s)")
    ap.add_argument("--listen", type=float, default=0.0,
                    help="apres les commandes, rester a l'ecoute N secondes (journal, branchements...)")
    ap.add_argument("--log", help="ajoute aussi chaque ligne a ce fichier")
    args = ap.parse_args(argv)

    global LOG
    if args.log:
        LOG = open(args.log, "a", encoding="utf-8")

    try:
        s = open_port(args.port, args.baud)
    except serial.SerialException as e:
        print(f"Ouverture de {args.port} impossible : {e}", file=sys.stderr)
        return 1

    stop = threading.Event()
    th = threading.Thread(target=cli_reader, args=(s, stop), daemon=True)
    th.start()
    try:
        if args.cmds:
            time.sleep(0.3)
            for c in args.cmds:
                cli_send(s, c)
                time.sleep(args.wait)
            time.sleep(args.listen)
        else:
            print("Commandes : RESTART  IS_READER  IS_CARD  READ_CARD  CARD_STATE  INFO  DEBUG   (Ctrl+C : quitter)")
            while True:
                line = input().strip()
                if line:
                    cli_send(s, line)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        th.join(timeout=1)
        s.close()
        if LOG:
            LOG.close()
    return 0


# =============================================================================
#  Icone (generee par le code : fenetre + executable)
# =============================================================================
def _in_rrect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    dx = max(x0 + r - x, 0.0, x - (x1 - r))
    dy = max(y0 + r - y, 0.0, y - (y1 - r))
    return dx * dx + dy * dy <= r * r


def _icon_sample(x, y):
    """Couleur RGBA au point (x, y) de [0,1]^2 : carte verte, puce doree."""
    if not _in_rrect(x, y, 0.04, 0.17, 0.96, 0.83, 0.10):
        return (0, 0, 0, 0)
    if _in_rrect(x, y, 0.13, 0.33, 0.39, 0.59, 0.05):             # puce
        if abs(x - 0.26) < 0.012 or abs(y - 0.46) < 0.012:
            return (161, 98, 7, 255)
        return (250, 204, 21, 255)
    if 0.48 <= x <= 0.86 and 0.37 <= y <= 0.44:                    # lignes de texte
        return (236, 253, 245, 255)
    if 0.48 <= x <= 0.74 and 0.51 <= y <= 0.58:
        return (187, 247, 208, 255)
    if 0.13 <= x <= 0.86 and 0.68 <= y <= 0.72:
        return (21, 128, 61, 255)
    return (22, 163, 74, 255)


def icon_pixels(size: int, ss: int = 4) -> list:
    """Lignes de pixels RGBA (anticrenelage par sur-echantillonnage)."""
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    c = _icon_sample((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    a = c[3]
                    acc[0] += c[0] * a
                    acc[1] += c[1] * a
                    acc[2] += c[2] * a
                    acc[3] += a
            n = ss * ss
            a = acc[3] / n
            if a:
                row.append((round(acc[0] / acc[3]), round(acc[1] / acc[3]), round(acc[2] / acc[3]), round(a)))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def write_ico(path: str, sizes=(16, 24, 32, 48, 64, 128, 256)) -> None:
    """Ecrit un .ico (images BMP 32 bits avec alpha) pour l'executable."""
    images = []
    for sz in sizes:
        rows = icon_pixels(sz, 4 if sz <= 64 else 2)
        pix = bytearray()
        for row in reversed(rows):                                  # BMP : de bas en haut
            for r, g, b, a in row:
                pix += bytes((b, g, r, a))
        mask_row = ((sz + 31) // 32) * 4
        mask = bytes(mask_row * sz)                                 # alpha suffit
        hdr = struct.pack("<IiiHHIIiiII", 40, sz, sz * 2, 1, 32, 0, len(pix) + len(mask), 0, 0, 0, 0)
        images.append((sz, hdr + bytes(pix) + mask))
    data = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for sz, img in images:
        d = sz if sz < 256 else 0
        data += struct.pack("<BBBBHHII", d, d, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    for _, img in images:
        data += img
    with open(path, "wb") as f:
        f.write(data)


# =============================================================================
#  Interface graphique
# =============================================================================
def run_gui(demo: bool = False) -> int:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    try:                                    # nettete sur ecran haute resolution
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    # ---- palette (theme sombre) ---------------------------------------------
    BG, PANEL, PANEL2, BORDER = "#16191f", "#1f232b", "#2a2f39", "#363c48"
    HOVER = "#343a46"
    TEXT, MUTED, DIM = "#e7eaf0", "#9aa3b2", "#5c6473"
    TERM_BG = "#0f1115"
    ACCENT, GREEN, RED = "#3b82f6", "#22c55e", "#ef4444"
    ORANGE, VIOLET, CYAN, YELLOW = "#f59e0b", "#a855f7", "#22d3ee", "#facc15"
    UI = "Segoe UI"
    MONO = "Consolas"

    # aspect des cartes reconnues (hors Vitale) : couleur, libelle
    CARD_LOOK = {
        "CB": ("#1e3a8a", "carte bancaire"),
        "CPS": ("#0f766e", "carte CPS"),
        "OPENPGP": ("#6d28d9", "OpenPGP"),
        "PIV": ("#334155", "carte PIV"),
        "ID": ("#1f2937", "identité"),
    }
    CARD_NAMES = {"CB": "Carte bancaire", "CPS": "Carte CPS", "OPENPGP": "Carte OpenPGP",
                  "PIV": "Carte PIV", "ID": "Document d'identité", "V1": "Carte Vitale 1"}
    FIELD_LABELS = (("RESEAU", "Réseau"), ("LIBELLE", "Libellé"), ("NOM", "Nom"), ("PRENOM", "Prénom"),
                    ("TITULAIRE", "Titulaire"), ("ETAB", "Établissement"), ("IDENT", "Identifiant"),
                    ("PROFESSION", "Profession"), ("NUM", "Numéro"))

    def mix(c1: str, c2: str, t: float) -> str:
        a = [int(c1[i:i + 2], 16) for i in (1, 3, 5)]
        b = [int(c2[i:i + 2], 16) for i in (1, 3, 5)]
        return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(a, b))

    def rrect(c, x1, y1, x2, y2, r, **kw):
        pts = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2,
               x2 - r, y2, x1 + r, y2, x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
        return c.create_polygon(pts, smooth=True, splinesteps=16, **kw)

    SETTINGS_PATH = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")),
                                 "pcsc_term", "settings.json")

    def load_settings() -> dict:
        try:
            with open(SETTINGS_PATH, encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return {}

    def save_settings(d: dict) -> None:
        try:
            os.makedirs(os.path.dirname(SETTINGS_PATH), exist_ok=True)
            with open(SETTINGS_PATH, "w", encoding="utf-8") as f:
                json.dump(d, f, indent=2)
        except Exception:
            pass

    # ---- etat du lecteur, deduit des trames ---------------------------------
    class ReaderState:
        def __init__(self) -> None:
            self.reset(port_open=False)

        def reset(self, port_open: bool) -> None:
            self.port_open = port_open
            self.booting = False          # banniere recue, lecteur pas encore annonce
            self.reader = None            # None inconnu, True present, False absent
            self.card = None              # None inconnu, True inseree, False absente
            self.reading = False
            self.reading_since = 0.0
            self.result = None            # None | ("ok", benefs) | ("error",) | ("notcv", code)
            self.info = {}
            self.debug = False
            self.expect_iscard = 0.0      # reponse attendue a IS_CARD jusqu'a cet instant

        def on_frame(self, f: str) -> None:
            now = time.time()
            if f.startswith("CARD READER"):
                dbg = self.debug
                self.reset(port_open=True)
                self.booting, self.debug = True, dbg
            elif f == "READER_OK":
                self.reader, self.booting = True, False
                if self.card is None:
                    self.card = False
            elif f == "READER_KO":
                self.reader, self.booting = False, False
                self.card, self.reading, self.result = False, False, None
            elif f == "CARD_IN":
                if now < self.expect_iscard:          # reponse a IS_CARD : etat, pas insertion
                    self.expect_iscard = 0
                    self.card = True
                else:
                    self.card, self.reading, self.result = True, True, None
                    self.reading_since = now
                self.reader = True
            elif f in ("CARD_OUT", "NO_CARD"):
                if f == "CARD_OUT" and now < self.expect_iscard:
                    self.expect_iscard = 0
                self.card, self.reading, self.result = False, False, None
            elif f.startswith("CARD_READ:"):
                self.card, self.reading, self.reader = True, False, True
                self.result = ("ok", parse_benefs(f[len("CARD_READ:"):]))
            elif f == "CARD_READ_ERROR":
                self.card, self.reading, self.reader = True, False, True
                self.result = ("error",)
            elif f.startswith("CARD_READ_NOT_CV"):
                self.card, self.reading, self.reader = True, False, True
                parts = (f.split(":", 1)[1] if ":" in f else "?").split(";")
                fields = {}
                for kv in parts[1:]:
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        fields[k] = v
                self.result = ("notcv", parts[0], fields)
            elif f.startswith("INFO:"):
                self.info = {}
                for kv in f[5:].split(";"):
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        self.info[k] = v
                if self.info.get("READER") == "NONE":
                    self.reader = False
                elif "READER" in self.info:
                    self.reader = True
            elif f == "DEBUG_ON":
                self.debug = True
            elif f == "DEBUG_OFF":
                self.debug = False

        def tick(self) -> None:
            if self.reading and time.time() - self.reading_since > 20:
                self.reading = False                  # pas de resultat : carte presente, non lue

    def parse_benefs(s: str) -> list:
        benefs = []
        for grp in s.split("|"):
            d = {}
            for kv in grp.split(";"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    d[k] = v
            if d:
                benefs.append(d)
        return benefs

    def fmt_date(s: str) -> str:
        return f"{s[6:8]}/{s[4:6]}/{s[0:4]}" if len(s) == 8 and s.isdigit() else s

    def fmt_nir(s: str, masked: bool) -> str:
        if masked and len(s) > 5:
            s = s[:5] + "•" * (len(s) - 5)
        if len(s) in (13, 15):
            parts, i = [], 0
            for n in (1, 2, 2, 2, 3, 3, 2):
                if i < len(s):
                    parts.append(s[i:i + n])
                    i += n
            return " ".join(parts)
        return s

    # ---- fenetre ----------------------------------------------------------------
    class App:
        def __init__(self) -> None:
            self.demo = demo
            self.root = tk.Tk()
            self.root.title(f"{APP_TITLE} — terminal PCSC" + ("  [DÉMO]" if demo else ""))
            self.root.configure(bg=BG)
            self.root.minsize(1060, 640)
            self.settings = load_settings()
            self.root.geometry(self.settings.get("geometry", "1440x920"))
            self._set_icon()

            self.ser = None
            self.port = ""
            self.rx_q = queue.Queue()
            self.stop_evt = threading.Event()
            self.thread = None
            self.state = ReaderState()
            self.lines = []                           # (t, sens, texte) pour l'enregistrement
            self.rx = self.tx = 0
            self.last_evt = None
            self.history, self.hist_pos = [], 0
            self.phase = 0.0
            self.benef_idx = 0
            self.ports = []

            self._style()
            self._build()
            self.refresh_ports()
            self.root.protocol("WM_DELETE_WINDOW", self.on_close)
            self.root.bind("<F5>", lambda e: self.send("READ_CARD"))
            self.root.bind("<Control-l>", lambda e: self.clear_term())
            self.root.after(30, self.poll)
            self.root.after(50, self.animate)
            self.root.after(3000, self.auto_refresh_ports)
            if os.environ.get("PCSC_TERM_SELFTEST"):
                self.root.after(300, self.selftest)
            if demo:
                self.sys_line("Mode DÉMO : port « DEMO » = module simulé (données fictives). "
                              "Les boutons « Simulation » jouent les gestes physiques.")
            self.sys_line(f"{APP_TITLE} {APP_VERSION} — choisissez le port COM de la carte puis « Connecter ».")

        # -- icone et style --------------------------------------------------------
        def _set_icon(self) -> None:
            try:
                sz = 32
                img = tk.PhotoImage(width=sz, height=sz)
                rows = icon_pixels(sz, 3)
                for y, row in enumerate(rows):
                    img.put("{" + " ".join(f"#{r:02x}{g:02x}{b:02x}" for r, g, b, a in row) + "}", to=(0, y))
                for y, row in enumerate(rows):
                    for x, (r, g, b, a) in enumerate(row):
                        if a < 128:
                            img.transparency_set(x, y, True)
                self.icon = img
                self.root.iconphoto(True, img)
            except Exception:
                pass

        def _style(self) -> None:
            st = ttk.Style(self.root)
            st.theme_use("clam")
            st.configure(".", background=PANEL, foreground=TEXT, fieldbackground=PANEL2,
                         bordercolor=BORDER, lightcolor=PANEL2, darkcolor=PANEL2,
                         troughcolor=PANEL, font=(UI, 10))
            st.configure("TCombobox", fieldbackground=PANEL2, background=PANEL2, foreground=TEXT,
                         arrowcolor=TEXT, bordercolor=BORDER, padding=6)
            st.map("TCombobox", fieldbackground=[("readonly", PANEL2), ("disabled", PANEL)],
                   foreground=[("readonly", TEXT), ("disabled", DIM)],
                   selectbackground=[("readonly", PANEL2)], selectforeground=[("readonly", TEXT)],
                   bordercolor=[("focus", ACCENT)])
            self.root.option_add("*TCombobox*Listbox.background", PANEL2)
            self.root.option_add("*TCombobox*Listbox.foreground", TEXT)
            self.root.option_add("*TCombobox*Listbox.selectBackground", ACCENT)
            self.root.option_add("*TCombobox*Listbox.selectForeground", "#ffffff")
            self.root.option_add("*TCombobox*Listbox.font", (UI, 10))
            st.configure("TCheckbutton", background=PANEL, foreground=MUTED, font=(UI, 9),
                         indicatorbackground=PANEL2, indicatorforeground=ACCENT,
                         upperbordercolor=BORDER, lowerbordercolor=BORDER)
            st.map("TCheckbutton", background=[("active", PANEL)], foreground=[("active", TEXT)],
                   indicatorbackground=[("selected", PANEL2)])
            for o in ("Vertical", "Horizontal"):
                st.configure(f"{o}.TScrollbar", background=PANEL2, troughcolor=TERM_BG,
                             bordercolor=TERM_BG, arrowcolor=MUTED, lightcolor=PANEL2,
                             darkcolor=PANEL2, gripcount=0)
                st.map(f"{o}.TScrollbar", background=[("active", HOVER)])

        # -- widgets utilitaires ----------------------------------------------------
        def card(self, parent, title=None, pad=12):
            outer = tk.Frame(parent, bg=PANEL, highlightthickness=1, highlightbackground=BORDER)
            if title:
                tk.Label(outer, text=title.upper(), bg=PANEL, fg=MUTED,
                         font=(UI, 8, "bold")).pack(anchor="w", padx=pad, pady=(pad - 2, 0))
            inner = tk.Frame(outer, bg=PANEL)
            inner.pack(fill="both", expand=True, padx=pad, pady=pad)
            return outer, inner

        def button(self, parent, text, cmd, kind="normal", **kw):
            colors = {"normal": (PANEL2, HOVER, TEXT), "accent": (ACCENT, "#2563eb", "#ffffff"),
                      "danger": ("#7f1d1d", "#991b1b", "#fecaca")}
            bg, hov, fg = colors[kind]
            b = tk.Label(parent, text=text, bg=bg, fg=fg, font=(UI, 10, "bold" if kind != "normal" else "normal"),
                         padx=kw.get("padx", 14), pady=kw.get("pady", 6), cursor="hand2")
            b._colors = (bg, hov, fg)
            b._enabled = True
            b.bind("<Enter>", lambda e: b._enabled and b.configure(bg=b._colors[1]))
            b.bind("<Leave>", lambda e: b.configure(bg=b._colors[0] if b._enabled else PANEL2))
            b.bind("<Button-1>", lambda e: b._enabled and cmd())
            return b

        def set_button_kind(self, b, kind, text=None):
            colors = {"normal": (PANEL2, HOVER, TEXT), "accent": (ACCENT, "#2563eb", "#ffffff"),
                      "danger": ("#7f1d1d", "#991b1b", "#fecaca")}
            b._colors = colors[kind]
            b.configure(bg=b._colors[0], fg=b._colors[2])
            if text:
                b.configure(text=text)

        # -- construction ----------------------------------------------------------
        def _build(self) -> None:
            r = self.root
            r.columnconfigure(0, weight=1)
            r.rowconfigure(2, weight=1)

            # En-tete
            head = tk.Frame(r, bg=BG)
            head.grid(row=0, column=0, sticky="ew", padx=18, pady=(14, 6))
            logo = tk.Canvas(head, width=40, height=40, bg=BG, highlightthickness=0)
            logo.pack(side="left")
            rrect(logo, 3, 9, 37, 33, 5, fill=GREEN, outline="")
            rrect(logo, 8, 15, 18, 25, 2, fill=YELLOW, outline="")
            logo.create_line(22, 17, 33, 17, fill="#ecfdf5", width=2)
            logo.create_line(22, 23, 29, 23, fill="#bbf7d0", width=2)
            tt = tk.Frame(head, bg=BG)
            tt.pack(side="left", padx=10)
            tk.Label(tt, text=APP_TITLE, bg=BG, fg=TEXT, font=(UI, 16, "bold")).pack(anchor="w")
            tk.Label(tt, text="Terminal PCSC — ESP32-S3 N8R2  ·  protocole STX…EOT, 115200 8N1",
                     bg=BG, fg=MUTED, font=(UI, 9)).pack(anchor="w")
            self.pill = tk.Label(head, text="●  Déconnecté", bg=PANEL2, fg=RED,
                                 font=(UI, 10, "bold"), padx=14, pady=6)
            self.pill.pack(side="right")

            # Barre de connexion
            bar_o, bar = self.card(r, pad=10)
            bar_o.grid(row=1, column=0, sticky="ew", padx=18, pady=6)
            tk.Label(bar, text="Port", bg=PANEL, fg=MUTED, font=(UI, 10)).pack(side="left")
            self.port_var = tk.StringVar()
            self.port_cb = ttk.Combobox(bar, textvariable=self.port_var, state="readonly", width=46)
            self.port_cb.pack(side="left", padx=(8, 6))
            self.btn_refresh = self.button(bar, "↻", self.refresh_ports, padx=10)
            self.btn_refresh.pack(side="left")
            self.btn_connect = self.button(bar, "Connecter", self.toggle_connect, "accent")
            self.btn_connect.pack(side="left", padx=(10, 0))
            self.sync_var = tk.BooleanVar(value=self.settings.get("sync", True))
            ttk.Checkbutton(bar, text="Synchroniser à l'ouverture",
                            variable=self.sync_var).pack(side="left", padx=16)
            tk.Label(bar, text="115200 8N1 · DTR/RTS inactifs", bg=PANEL, fg=DIM,
                     font=(UI, 9)).pack(side="right")

            # Corps
            body = tk.Frame(r, bg=BG)
            body.grid(row=2, column=0, sticky="nsew", padx=18, pady=6)
            body.columnconfigure(0, weight=1)
            body.rowconfigure(0, weight=1)
            left = tk.Frame(body, bg=BG)
            left.grid(row=0, column=0, sticky="nsew")
            left.columnconfigure(0, weight=1)
            left.rowconfigure(1, weight=1)
            right = tk.Frame(body, bg=BG, width=372)
            right.grid(row=0, column=1, sticky="ns", padx=(12, 0))
            right.pack_propagate(False)                     # largeur fixe : pas de saut de mise en page

            self._build_commands(left)
            self._build_terminal(left)
            self._build_side(right)

            # Barre d'etat
            self.status = tk.Label(r, text="", bg=BG, fg=DIM, font=(UI, 9), anchor="w")
            self.status.grid(row=3, column=0, sticky="ew", padx=20, pady=(2, 10))
            self.update_status()
            self.update_controls()

        def _build_commands(self, parent) -> None:
            o, c = self.card(parent, "Commandes")
            o.grid(row=0, column=0, sticky="ew", pady=(0, 10))
            tiles = [
                ("READ_CARD", "Relire (F5)", "▶"),
                ("CARD_STATE", "Résultat", "≡"),
                ("IS_CARD", "Carte ?", "▭"),
                ("IS_READER", "Lecteur ?", "⏻"),
                ("INFO", "Diagnostic", "ⓘ"),
                ("DEBUG", "Journal", "⚙"),
                ("RESTART", "Réinit.", "↻"),
            ]
            self.tiles = {}
            grid = tk.Frame(c, bg=PANEL)
            grid.pack(fill="x")
            for i, (cmd, desc, ico) in enumerate(tiles):
                t = tk.Frame(grid, bg=PANEL2, cursor="hand2", highlightthickness=1, highlightbackground=PANEL2)
                t.grid(row=0, column=i, sticky="nsew", padx=(0 if i == 0 else 6, 0))
                grid.columnconfigure(i, weight=1, uniform="tiles")
                top = tk.Frame(t, bg=PANEL2)
                top.pack(fill="x", padx=10, pady=(8, 0))
                lc = tk.Label(top, text=cmd, bg=PANEL2, fg=TEXT, font=(MONO, 10, "bold"))
                lc.pack(side="left")
                badge = tk.Label(top, text="", bg=PANEL2, fg=YELLOW, font=(UI, 8, "bold"))
                badge.pack(side="right")
                bot = tk.Frame(t, bg=PANEL2)
                bot.pack(fill="x", padx=10, pady=(2, 8))
                li = tk.Label(bot, text=ico, bg=PANEL2, fg=ACCENT, font=("Segoe UI Symbol", 9))
                li.pack(side="left")
                ld = tk.Label(bot, text=desc, bg=PANEL2, fg=MUTED, font=(UI, 8), anchor="w")
                ld.pack(side="left", padx=(5, 0))
                parts = (t, top, bot, li, badge, lc, ld)
                t._parts, t._enabled = parts, True
                for w in parts:
                    w.bind("<Enter>", lambda e, t=t: t._enabled and self._tile_bg(t, HOVER))
                    w.bind("<Leave>", lambda e, t=t: self._tile_bg(t, PANEL2))
                    w.bind("<Button-1>", lambda e, cmd=cmd: self.tile_click(cmd))
                t._labels = (li, lc, ld, badge)
                self.tiles[cmd] = t

            self.sim_buttons = []
            if self.demo:
                sim = tk.Frame(c, bg=PANEL)
                sim.pack(fill="x", pady=(10, 0))
                tk.Label(sim, text="Simulation", bg=PANEL, fg=YELLOW, font=(UI, 9, "bold")).pack(side="left", padx=(0, 8))
                rows = ((("Vitale 2", "DEMO VITALE"), ("Vitale 1", "DEMO V1"), ("Bancaire", "DEMO BANK"),
                         ("CPS", "DEMO CPS"), ("OpenPGP", "DEMO OPENPGP"), ("PIV", "DEMO PIV"),
                         ("Identité", "DEMO ID")),
                        (("Carte en erreur", "DEMO ERROR"), ("Retirer", "DEMO OUT"),
                         ("Débrancher", "DEMO UNPLUG"), ("Rebrancher", "DEMO PLUG")))
                col = tk.Frame(sim, bg=PANEL)
                col.pack(side="left", fill="x")
                for r_i, row_def in enumerate(rows):
                    line = tk.Frame(col, bg=PANEL)
                    line.pack(fill="x", pady=(0 if r_i == 0 else 4, 0))
                    tk.Label(line, text="insérer :" if r_i == 0 else "lecteur :", bg=PANEL, fg=MUTED,
                             font=(UI, 8), width=8, anchor="w").pack(side="left")
                    for txt, cmd in row_def:
                        b = self.button(line, txt, lambda c=cmd: self.send(c), padx=8, pady=2)
                        b.configure(font=(UI, 9))
                        b.pack(side="left", padx=(0, 5))
                        self.sim_buttons.append(b)

        def _tile_bg(self, t, color) -> None:
            for w in t._parts:
                w.configure(bg=color)
            t.configure(highlightbackground=ACCENT if (t is self.tiles.get("DEBUG") and self.state.debug) else color)

        def _build_terminal(self, parent) -> None:
            o, c = self.card(parent, "Terminal")
            o.grid(row=1, column=0, sticky="nsew")
            tools = tk.Frame(c, bg=PANEL)
            tools.pack(fill="x", pady=(0, 8))
            self.v_dbg = tk.BooleanVar(value=self.settings.get("show_dbg", True))
            self.v_noise = tk.BooleanVar(value=self.settings.get("show_noise", True))
            self.v_ts = tk.BooleanVar(value=self.settings.get("show_ts", True))
            self.v_auto = tk.BooleanVar(value=True)
            for txt, var in (("Lignes DBG", self.v_dbg), ("Log ROM", self.v_noise),
                             ("Horodatage", self.v_ts), ("Défilement auto", self.v_auto)):
                ttk.Checkbutton(tools, text=txt, variable=var, command=self.apply_filters).pack(side="left", padx=(0, 14))
            self.button(tools, "Enregistrer…", self.save_log, padx=10, pady=3).pack(side="right")
            self.button(tools, "Effacer", self.clear_term, padx=10, pady=3).pack(side="right", padx=6)

            frame = tk.Frame(c, bg=TERM_BG, highlightthickness=1, highlightbackground=BORDER)
            frame.pack(fill="both", expand=True)
            self.term = tk.Text(frame, bg=TERM_BG, fg=TEXT, font=(MONO, 10), relief="flat", wrap="word",
                                padx=10, pady=8, insertbackground=TEXT, selectbackground="#264f78",
                                state="disabled", spacing1=1, spacing3=1)
            sb = ttk.Scrollbar(frame, orient="vertical", command=self.term.yview)
            self.term.configure(yscrollcommand=sb.set)
            sb.pack(side="right", fill="y")
            self.term.pack(side="left", fill="both", expand=True)
            tags = {
                "ts": DIM, "tx": ACCENT, "rx": TEXT, "ok": GREEN, "ko": RED, "card": CYAN,
                "muted": MUTED, "read_ok": GREEN, "notcv": VIOLET, "info": "#93c5fd",
                "banner": YELLOW, "debug": YELLOW, "warn": ORANGE, "dbg": "#7c8596",
                "noise": "#4b5260", "sys": ORANGE,
            }
            for name, col in tags.items():
                self.term.tag_configure(name, foreground=col)
            self.term.tag_configure("read_ok", font=(MONO, 10, "bold"))
            self.term.tag_configure("banner", font=(MONO, 10, "bold"))
            self.term.tag_configure("dbg", font=(MONO, 9))
            self.term.tag_configure("noise", font=(MONO, 9, "italic"))
            self.apply_filters()

            # Saisie libre, facon console (Entree = envoyer, fleches = historique)
            row = tk.Frame(c, bg=PANEL)
            row.pack(fill="x", pady=(8, 0))
            tk.Label(row, text="›", bg=PANEL, fg=ACCENT, font=(MONO, 14, "bold")).pack(side="left")
            self.entry = tk.Entry(row, bg=TERM_BG, fg=TEXT, insertbackground=TEXT, relief="flat",
                                  font=(MONO, 10), highlightthickness=1, highlightbackground=BORDER,
                                  highlightcolor=ACCENT)
            self.entry.pack(side="left", fill="x", expand=True, padx=8, ipady=5)
            self.entry.bind("<Return>", lambda e: self.send_entry())
            self.entry.bind("<Up>", lambda e: self.hist_move(-1))
            self.entry.bind("<Down>", lambda e: self.hist_move(1))
            self.btn_send = self.button(row, "Envoyer", self.send_entry)
            self.btn_send.pack(side="left")

        def _build_side(self, parent) -> None:
            o, c = self.card(parent, "Lecteur")
            o.pack(fill="x")
            self.cv = tk.Canvas(c, width=320, height=204, bg=PANEL, highlightthickness=0)
            self.cv.pack()
            self.lbl_state = tk.Label(c, text="", bg=PANEL, fg=TEXT, font=(UI, 13, "bold"))
            self.lbl_state.pack()
            self.lbl_sub = tk.Label(c, text="", bg=PANEL, fg=MUTED, font=(UI, 9), wraplength=320,
                                    justify="center", height=2)
            self.lbl_sub.pack()

            # Details : onglets « Carte » / « Lecteur USB »
            o2 = tk.Frame(parent, bg=PANEL, highlightthickness=1, highlightbackground=BORDER)
            o2.pack(fill="x", pady=(10, 0))
            tabs = tk.Frame(o2, bg=PANEL)
            tabs.pack(fill="x", padx=12, pady=(8, 0))
            self.tab_btns = {}
            for key, txt in (("card", "Données de la carte"), ("usb", "Lecteur USB")):
                lb = tk.Label(tabs, text=txt.upper(), bg=PANEL, fg=MUTED, font=(UI, 8, "bold"),
                              cursor="hand2", padx=2, pady=4)
                lb.pack(side="left", padx=(0, 16))
                lb.bind("<Button-1>", lambda e, k=key: self.show_tab(k))
                self.tab_btns[key] = lb
            tk.Frame(o2, bg=BORDER, height=1).pack(fill="x", padx=12)
            body = tk.Frame(o2, bg=PANEL)
            body.pack(fill="both", expand=True, padx=12, pady=(8, 12))

            # -- onglet Carte
            v1 = tk.Frame(body, bg=PANEL)
            hdr = tk.Frame(v1, bg=PANEL)
            hdr.pack(fill="x")
            self.lbl_benef = tk.Label(hdr, text="", bg=PANEL, fg=MUTED, font=(UI, 9))
            self.lbl_benef.pack(side="left")
            self.btn_prev = self.button(hdr, "◀", lambda: self.benef_step(-1), padx=6, pady=0)
            self.btn_next = self.button(hdr, "▶", lambda: self.benef_step(1), padx=6, pady=0)
            self.btn_copy = self.button(hdr, "Copier", self.copy_data, padx=8, pady=1)
            self.btn_copy.configure(font=(UI, 9))
            self.btn_copy.pack(side="right")
            self.data_grid = tk.Frame(v1, bg=PANEL)
            self.data_grid.pack(fill="x", pady=(6, 0))
            self._rows = None
            self.mask_var = tk.BooleanVar(value=self.settings.get("mask_nir", False))
            self.mask_cb = ttk.Checkbutton(v1, text="Masquer le NIR", variable=self.mask_var,
                                           command=self.update_side)
            self.mask_cb.pack(anchor="w", pady=(4, 0))

            # -- onglet Lecteur USB
            v2 = tk.Frame(body, bg=PANEL)
            self.lbl_info = tk.Label(v2, text="", bg=PANEL, fg=MUTED, font=(MONO, 9), justify="left",
                                     anchor="nw", wraplength=320)
            self.lbl_info.pack(fill="x")
            self.btn_info = self.button(v2, "Actualiser (INFO)", lambda: self.send("INFO"), padx=8, pady=1)
            self.btn_info.configure(font=(UI, 9))
            self.btn_info.pack(anchor="w", pady=(8, 0))

            self.tab_views = {"card": v1, "usb": v2}
            self.show_tab("card")
            self.update_side()

        def show_tab(self, key: str) -> None:
            for k, v in self.tab_views.items():
                v.pack_forget()
                self.tab_btns[k].configure(fg=TEXT if k == key else DIM)
            self.tab_views[key].pack(fill="both", expand=True)
            self.tab = key

        # -- ports -----------------------------------------------------------------
        def refresh_ports(self) -> None:
            if self.ser:
                return
            try:
                ports = list_ports()
            except Exception:
                ports = []
            if self.demo:
                ports = [("DEMO", "module simulé, sans matériel", True)] + ports
            star = "   ★"
            labels = [f"{d}  —  {desc}" + (star if pref else "") for d, desc, pref in ports]
            same = [p[0] for p in ports] == [p[0] for p in self.ports]
            self.ports = ports
            self.port_cb.configure(values=labels)
            cur = self.port_var.get().split(" ")[0] if self.port_var.get() else ""
            if same and cur:
                return
            want = cur or ("DEMO" if self.demo else self.settings.get("port", ""))
            idx = next((i for i, p in enumerate(ports) if p[0] == want), None)
            if idx is None:
                idx = next((i for i, p in enumerate(ports) if p[2]), 0 if ports else None)
            if idx is not None:
                self.port_cb.current(idx)
            else:
                self.port_var.set("")

        def auto_refresh_ports(self) -> None:
            if not self.ser:
                self.refresh_ports()
            self.root.after(3000, self.auto_refresh_ports)

        # -- connexion -------------------------------------------------------------
        def toggle_connect(self) -> None:
            if self.ser:
                self.disconnect("déconnecté")
            else:
                self.connect()

        def connect(self) -> None:
            sel = self.port_var.get()
            if not sel:
                messagebox.showwarning(APP_TITLE, "Aucun port COM sélectionné.")
                return
            port = sel.split(" ")[0]
            try:
                self.ser = DemoPort() if port == "DEMO" else open_port(port)
            except serial.SerialException as e:
                self.ser = None
                self.sys_line(f"Ouverture de {port} impossible : {e}")
                messagebox.showerror(APP_TITLE, f"Ouverture de {port} impossible :\n{e}")
                return
            self.port = port
            if port != "DEMO":
                self.settings["port"] = port
            self.stop_evt.clear()
            self.thread = threading.Thread(target=self.reader_thread, daemon=True)
            self.thread.start()
            self.state.reset(port_open=True)
            self.sys_line(f"Port {port} ouvert (115200 8N1, DTR/RTS inactifs).")
            self.update_controls()
            self.update_side()
            if self.sync_var.get():
                for i, cmd in enumerate(("IS_READER", "IS_CARD", "CARD_STATE", "INFO")):
                    self.root.after(150 + 200 * i, lambda c=cmd: self.send(c))

        def disconnect(self, why: str) -> None:
            if not self.ser:
                return
            self.stop_evt.set()
            try:
                self.ser.cancel_read()
            except Exception:
                pass
            if self.thread:
                self.thread.join(timeout=1)
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            self.sys_line(f"Port {self.port} fermé ({why}).")
            self.state.reset(port_open=False)
            self.update_controls()
            self.update_side()
            self.refresh_ports()

        def reader_thread(self) -> None:
            parser = FrameParser()
            ser = self.ser
            while not self.stop_evt.is_set():
                try:
                    data = ser.read(256)
                except Exception as e:                # port arrache, carte debranchee
                    if not self.stop_evt.is_set():
                        self.rx_q.put(("lost", str(e)))
                    return
                items = parser.feed(data) if data else parser.flush_noise()
                t = time.time()
                for kind, txt in items:
                    self.rx_q.put((kind, txt, t))

        # -- emission --------------------------------------------------------------
        def send(self, cmd: str) -> None:
            if not self.ser:
                return
            try:
                self.ser.write(encode_cmd(cmd))
            except Exception as e:
                self.disconnect(f"erreur d'écriture : {e}")
                return
            self.tx += 1
            u = cmd.upper()
            if u == "IS_CARD":
                self.state.expect_iscard = time.time() + 2.0
            elif u == "READ_CARD" and self.state.card:
                self.state.reading, self.state.result = True, None
                self.state.reading_since = time.time()
            self.add_line("tx", cmd, time.time())
            self.update_status()
            self.update_side()

        def tile_click(self, cmd: str) -> None:
            if not self.ser:
                return
            if cmd == "DEBUG":
                cmd = "DEBUG OFF" if self.state.debug else "DEBUG ON"
            self.send(cmd)

        def send_entry(self) -> None:
            cmd = self.entry.get().strip()
            if not cmd or not self.ser:
                return
            self.history.append(cmd)
            self.hist_pos = len(self.history)
            self.entry.delete(0, "end")
            self.send(cmd)

        def hist_move(self, d: int) -> str:
            if not self.history:
                return "break"
            self.hist_pos = max(0, min(len(self.history), self.hist_pos + d))
            self.entry.delete(0, "end")
            if self.hist_pos < len(self.history):
                self.entry.insert(0, self.history[self.hist_pos])
            return "break"

        # -- reception ---------------------------------------------------------------
        def poll(self) -> None:
            n = 0
            changed = False
            try:
                while n < 400:
                    item = self.rx_q.get_nowait()
                    n += 1
                    if item[0] == "lost":
                        self.disconnect(f"port perdu : {item[1]}")
                        break
                    kind, txt, t = item
                    if kind == "frame":
                        self.rx += 1
                        self.add_line("rx", txt, t)
                        if not txt.startswith("DBG:"):
                            self.state.on_frame(txt)
                            self.last_evt = (txt, t)
                            changed = True
                        elif not self.state.debug:        # des lignes DBG arrivent : le module est en DEBUG
                            self.state.debug = True
                            changed = True
                    else:
                        self.add_line("noise", txt, t)
                        if self.state.debug:              # log ROM = l'ESP32 a redemarre : DEBUG perdu
                            self.state.debug = False
                            changed = True
            except queue.Empty:
                pass
            if changed:
                self.benef_idx = 0
                res = self.state.result
                if res and res[0] == "ok" and getattr(self, "tab", "card") != "card":
                    self.show_tab("card")
                self.update_side()
                self.update_controls()
            if n:
                self.update_status()
            self.root.after(30, self.poll)

        @staticmethod
        def classify(f: str) -> str:
            if f.startswith("DBG:"):
                return "dbg"
            if f == "READER_OK":
                return "ok"
            if f in ("READER_KO", "CARD_READ_ERROR"):
                return "ko"
            if f == "CARD_IN":
                return "card"
            if f in ("CARD_OUT", "NO_CARD"):
                return "muted"
            if f.startswith("CARD_READ:"):
                return "read_ok"
            if f.startswith("CARD_READ_NOT_CV"):
                return "notcv"
            if f.startswith("INFO:"):
                return "info"
            if f.startswith("CARD READER"):
                return "banner"
            if f.startswith("DEBUG_"):
                return "debug"
            if f == "UNKNOWN_CMD":
                return "warn"
            return "rx"

        def add_line(self, sens: str, txt: str, t: float) -> None:
            self.lines.append((t, sens, txt))
            if len(self.lines) > 50000:
                del self.lines[:10000]
            if sens == "tx":
                cat, arrow, filt = "tx", "→", "f_tx"
            elif sens == "noise":
                cat, arrow, filt = "noise", "~", "f_noise"
            elif sens == "sys":
                cat, arrow, filt = "sys", "!", "f_sys"
            else:
                cat = self.classify(txt)
                arrow = "·" if cat == "dbg" else "←"
                filt = "f_dbg" if cat == "dbg" else "f_rx"
            tm = self.term
            tm.configure(state="normal")
            tm.insert("end", stamp(t) + "  ", ("ts", "f_ts", filt))
            tm.insert("end", arrow + " ", (cat, filt))
            tm.insert("end", txt + "\n", (cat, filt))
            if int(tm.index("end-1c").split(".")[0]) > 12000:
                tm.delete("1.0", "2001.0")
            tm.configure(state="disabled")
            if self.v_auto.get():
                tm.see("end")

        def sys_line(self, txt: str) -> None:
            self.add_line("sys", txt, time.time())

        def apply_filters(self) -> None:
            self.term.tag_configure("f_dbg", elide=not self.v_dbg.get())
            self.term.tag_configure("f_noise", elide=not self.v_noise.get())
            self.term.tag_configure("f_ts", elide=not self.v_ts.get())
            if self.v_auto.get():
                self.term.see("end")

        def clear_term(self) -> None:
            self.term.configure(state="normal")
            self.term.delete("1.0", "end")
            self.term.configure(state="disabled")

        def save_log(self) -> None:
            name = time.strftime("pcsc_journal_%Y%m%d_%H%M%S.txt")
            path = filedialog.asksaveasfilename(title="Enregistrer le journal", initialfile=name,
                                                defaultextension=".txt",
                                                filetypes=[("Texte", "*.txt"), ("Tous", "*.*")])
            if not path:
                return
            marks = {"tx": ">>", "rx": "<<", "noise": "~~", "sys": "!!"}
            try:
                with open(path, "w", encoding="utf-8") as f:
                    for t, sens, txt in self.lines:
                        f.write(f"{stamp(t)} {marks.get(sens, '??')} {txt}\n")
                self.sys_line(f"Journal enregistré : {path}")
            except OSError as e:
                messagebox.showerror(APP_TITLE, f"Enregistrement impossible :\n{e}")

        # -- etat de l'interface -------------------------------------------------------
        def update_controls(self) -> None:
            on = self.ser is not None
            self.set_button_kind(self.btn_connect, "danger" if on else "accent",
                                 "Déconnecter" if on else "Connecter")
            self.port_cb.configure(state="disabled" if on else "readonly")
            self.btn_refresh._enabled = not on
            self.btn_refresh.configure(fg=DIM if on else TEXT)
            self.pill.configure(text=f"●  {self.port} connecté" if on else "●  Déconnecté",
                                fg=GREEN if on else RED)
            for cmd, t in self.tiles.items():
                t._enabled = on
                li, lc, ld, badge = t._labels
                lc.configure(fg=TEXT if on else DIM)
                li.configure(fg=ACCENT if on else DIM)
                ld.configure(fg=MUTED if on else DIM)
                t.configure(cursor="hand2" if on else "arrow")
            dt = self.tiles["DEBUG"]
            dt._labels[3].configure(text="ON" if (on and self.state.debug) else "")
            self._tile_bg(dt, PANEL2)
            self.btn_send._enabled = on
            for b in self.sim_buttons:
                b._enabled = on
                b.configure(fg=TEXT if on else DIM)

        def update_status(self) -> None:
            port = self.port if self.ser else "aucun port"
            last = ""
            if self.last_evt:
                last = f"   ·   dernier événement : {self.last_evt[0][:40]}  ({stamp(self.last_evt[1])[:8]})"
            self.status.configure(text=f"{port}   ·   trames reçues : {self.rx}   ·   "
                                       f"commandes envoyées : {self.tx}{last}")

        def benef_step(self, d: int) -> None:
            res = self.state.result
            if res and res[0] == "ok" and res[1]:
                self.benef_idx = (self.benef_idx + d) % len(res[1])
                self.update_side()

        def data_rows(self) -> list:
            """Lignes (libelle, valeur) de l'onglet Donnees, et libelle d'en-tete."""
            res = self.state.result
            if res and res[0] == "ok" and res[1]:
                benefs = res[1]
                self.benef_idx %= len(benefs)
                b = benefs[self.benef_idx]
                self.lbl_benef.configure(text=f"Bénéficiaire {self.benef_idx + 1} / {len(benefs)}")
                return [("Nom", b.get("NOM", "—")), ("Prénom", b.get("PRENOM", "—")),
                        ("Naissance", fmt_date(b.get("NAISS", "—"))),
                        ("NIR", fmt_nir(b.get("NIR", "—"), self.mask_var.get())),
                        ("Nationalité", b.get("NAT", "—"))]
            if res and res[0] == "notcv":
                f = res[2] if len(res) > 2 else {}
                self.lbl_benef.configure(text=CARD_NAMES.get(res[1], f"Carte inconnue (SW {res[1]})"))
                skip = {"TITULAIRE"} if (f.get("NOM") and f.get("PRENOM")) else set()
                rows = [(lab, f[k]) for k, lab in FIELD_LABELS if f.get(k) and k not in skip]
                return rows or [("Données", "aucune donnée publique")]
            self.lbl_benef.configure(text="Aucune donnée lue")
            return [("Nom", "—"), ("Prénom", "—"), ("Naissance", "—"), ("NIR", "—"), ("Nationalité", "—")]

        def copy_data(self) -> None:
            rows = self.data_rows()
            if not any(v != "—" for _, v in rows):
                return
            lines = [self.lbl_benef.cget("text")] + [f"{lab} : {val}" for lab, val in rows]
            self.root.clipboard_clear()
            self.root.clipboard_append("\n".join(lines))
            self.sys_line("Données de la carte copiées dans le presse-papiers.")

        def view(self):
            """(titre, sous-titre, couleur) du panneau lateral."""
            s = self.state
            if not s.port_open:
                return "Port fermé", "Choisissez le port COM de la carte puis « Connecter ».", MUTED
            if s.booting:
                return "Recherche du lecteur…", "Démarrage ou RESTART du module (2,5 s).", ACCENT
            if s.reader is False:
                return "Lecteur absent", "Branchez un lecteur (5 V sur la prise « USB »). Recherche toutes les 30 s.", RED
            if s.reader is None:
                return "État inconnu", "Interrogez le module (IS_READER).", MUTED
            if not s.card:
                return "Prêt — insérez une carte", "Lecteur détecté, aucune carte.", GREEN
            if s.reading:
                return "Lecture de la carte…", "", ACCENT
            r = s.result
            if r is None:
                return "Carte insérée", "Résultat inconnu : CARD_STATE ou READ_CARD.", CYAN
            if r[0] == "ok":
                n = len(r[1])
                return "Carte Vitale lue", f"{n} bénéficiaire{'s' if n > 1 else ''}.", GREEN
            if r[0] == "error":
                return "Erreur de lecture", "CARD_READ_ERROR — voir INFO (STEP, SW) ou DEBUG.", RED
            code, f = r[1], (r[2] if len(r) > 2 else {})
            if code == "V1":
                return "Carte Vitale 1", "Ancien modèle (sans photo) : reconnue, non lue.", VIOLET
            if code == "CB":
                sub = " · ".join(x for x in (f.get("RESEAU", ""), f.get("LIBELLE", "")) if x)
                return "Carte bancaire", sub or "Carte de paiement (EMV).", VIOLET
            if code == "CPS":
                return "Carte CPS", f.get("ETAB") or "Carte de Professionnel de Santé.", VIOLET
            if code == "OPENPGP":
                return "Carte OpenPGP", "Carte de chiffrement / signature OpenPGP.", VIOLET
            if code == "PIV":
                return "Carte PIV", f.get("ETAB") or "Carte d'authentification PIV.", VIOLET
            if code == "ID":
                return "Document d'identité", "Données protégées (BAC/PACE) : nom non lisible.", VIOLET
            if code == "T1":
                return "Pas une Carte Vitale", "Carte T=1 sur un lecteur TPDU.", VIOLET
            return "Pas une Carte Vitale", f"Application Vitale absente (SW {code}).", VIOLET

        def module_led(self):
            """Couleur deduite de la LED du module (cf. status_led.h)."""
            s, p = self.state, self.phase
            breath = (1 - abs((p % 1.6) / 0.8 - 1)) ** 2
            blink = 1.0 if (p % 0.2) < 0.1 else 0.0
            if not s.port_open:
                return DIM
            if s.booting:
                return mix(PANEL, "#3b82f6", 0.15 + 0.85 * breath)
            if s.reader is False:
                return mix(PANEL, RED, 0.6)
            if s.reader is None:
                return DIM
            if not s.card:
                return mix(PANEL, GREEN, 0.15 + 0.85 * breath)
            if s.reading:
                return mix(PANEL, GREEN, 0.6 * blink)
            r = s.result
            if r is None or r[0] == "ok":
                return GREEN
            if r[0] == "error":
                return mix(PANEL, RED, 0.6)
            return mix(PANEL, VIOLET, 0.75)

        def update_side(self) -> None:
            title, sub, col = self.view()
            self.lbl_state.configure(text=title, fg=col)
            self.lbl_sub.configure(text=sub)
            self.draw()
            # donnees
            rows = self.data_rows()
            benefs = self.state.result[1] if (self.state.result and self.state.result[0] == "ok") else []
            if rows != self._rows:                    # reconstruit seulement si le contenu change
                self._rows = rows
                for w in self.data_grid.winfo_children():
                    w.destroy()
                filled = any(v != "—" for _, v in rows)
                for i, (lab, val) in enumerate(rows):
                    tk.Label(self.data_grid, text=lab, bg=PANEL, fg=MUTED, font=(UI, 9)).grid(
                        row=i, column=0, sticky="w")
                    tk.Label(self.data_grid, text=val, bg=PANEL, fg=TEXT if filled else DIM,
                             font=(UI, 10, "bold"), anchor="w", wraplength=210, justify="left").grid(
                        row=i, column=1, sticky="w", padx=(12, 0))
            res = self.state.result
            if res and res[0] == "notcv":             # le NIR ne concerne que la Vitale
                self.mask_cb.pack_forget()
            elif not self.mask_cb.winfo_ismapped():
                self.mask_cb.pack(anchor="w", pady=(4, 0))
            multi = len(benefs) > 1
            for b in (self.btn_prev, self.btn_next):
                if multi:
                    b.pack(side="left", padx=(6, 0))
                else:
                    b.pack_forget()
            has = any(v != "—" for _, v in rows)
            self.btn_copy._enabled = has
            self.btn_copy.configure(fg=TEXT if has else DIM)
            # lecteur USB
            i = self.state.info
            if i and i.get("READER") not in (None, "NONE"):
                txt = (f"USB {i.get('READER')} · {i.get('LEVEL', '?')} · features {i.get('FEAT', '?')}\n"
                       f"ATR {i.get('ATR', '?')}\n"
                       f"étape {i.get('STEP', '?')} · SW {i.get('SW', '?')} · lecture {i.get('MS', '?')} ms")
            elif i:
                txt = f"Aucun lecteur · dernier appareil USB : {i.get('LASTDEV', '?')}\nétape {i.get('STEP', '?')}"
            else:
                txt = "Lecteur USB : pas encore d'information (commande INFO)."
            self.lbl_info.configure(text=txt)

        # -- dessin du lecteur ---------------------------------------------------------
        def draw(self) -> None:
            c, s = self.cv, self.state
            c.delete("all")
            W = 320
            on = s.port_open
            present = on and s.reader is True
            absent = on and s.reader is False
            body = "#3b4250" if present else "#2b3039"
            edge = mix(body, "#ffffff", 0.12)
            BY1, BY2 = 116, 190                       # corps du lecteur
            SY1, SY2 = 126, 138                       # fente

            # LED du module (deduite) + mode DEBUG
            led = self.module_led()
            c.create_oval(10, 8, 24, 22, fill=led, outline=mix(led, "#000000", 0.3))
            c.create_text(32, 15, text="LED du module", anchor="w", fill=DIM, font=(UI, 8))
            if on and s.debug:
                rrect(c, W - 66, 5, W - 10, 25, 8, fill="#3f3a12", outline="")
                c.create_text(W - 38, 15, text="DEBUG", fill=YELLOW, font=(UI, 8, "bold"))

            # cable USB (coupe si lecteur absent)
            cy = 163
            if absent:
                c.create_line(0, cy, 20, cy, fill="#4b5260", width=6, capstyle="round")
                c.create_line(30, cy, 44, cy, fill="#4b5260", width=6, capstyle="round")
                c.create_line(19, cy - 9, 31, cy + 9, fill=RED, width=2)
            elif on:
                c.create_line(0, cy, 44, cy, fill="#5b6474" if present else "#4b5260", width=6, capstyle="round")

            # ombre + corps
            c.create_oval(54, BY2 - 4, W - 54, BY2 + 12, fill="#14171c", outline="")
            if not on:
                rrect(c, 44, BY1, W - 44, BY2, 16, fill=PANEL, outline=BORDER, width=2, dash=(6, 4))
            elif absent:
                rrect(c, 44, BY1, W - 44, BY2, 16, fill="#242830", outline=mix(RED, PANEL, 0.4), width=2,
                      dash=(6, 4))
            else:
                rrect(c, 44, BY1, W - 44, BY2, 16, fill=body, outline=edge, width=2)
                rrect(c, 52, BY1 + 6, W - 52, BY1 + 22, 8, fill=mix(body, "#ffffff", 0.05), outline="")
            rrect(c, 76, SY1, W - 76, SY2, 5, fill="#0b0d11" if on else BORDER, outline="")

            # carte
            res = s.result
            if present and s.card:
                if res and res[0] == "ok":
                    fill, label = "#15803d", "carte Vitale"
                elif res and res[0] == "notcv" and res[1] == "V1":
                    fill, label = "#166534", "carte Vitale 1"
                elif res and res[0] == "notcv" and res[1] in CARD_LOOK:
                    fill, label = CARD_LOOK[res[1]]
                elif res and res[0] == "notcv":
                    fill, label = "#475569", "carte à puce"
                elif res and res[0] == "error":
                    fill, label = "#7c2d12", "carte à puce"
                else:
                    fill, label = "#1d4ed8", "carte à puce"
                x1, x2, y1, y2 = 95, W - 95, 28, SY1 + 8
                rrect(c, x1, y1, x2, y2, 11, fill=fill, outline=mix(fill, "#000000", 0.35), width=1)
                rrect(c, x1 + 1, y1 + 1, x2 - 1, y1 + 26, 10, fill=mix(fill, "#ffffff", 0.08), outline="")
                rrect(c, x1 + 14, y1 + 28, x1 + 42, y1 + 50, 5, fill="#facc15", outline="#a16207")
                c.create_line(x1 + 14, y1 + 39, x1 + 42, y1 + 39, fill="#a16207")
                c.create_line(x1 + 28, y1 + 28, x1 + 28, y1 + 50, fill="#a16207")
                c.create_text((x1 + x2) / 2, y1 + 72, text=label, fill="#ffffff",
                              font=(UI, 10 if len(label) <= 12 else 9, "bold italic"))
                c.create_line(x1 + 14, y1 + 88, x2 - 14, y1 + 88, fill=mix(fill, "#ffffff", 0.25), width=3)
                # la carte entre dans la fente
                rrect(c, 76, SY2 - 6, W - 76, SY2, 3, fill="#0b0d11", outline="")
                c.create_rectangle(70, SY2, W - 70, SY2 + 6, fill=body, outline="")
                # badge
                bx, by, br = x2 - 4, y1 + 4, 18
                if s.reading:
                    c.create_oval(bx - br, by - br, bx + br, by + br, fill=PANEL, outline=BORDER, width=2)
                    ang = (self.phase * 360) % 360
                    c.create_arc(bx - br + 4, by - br + 4, bx + br - 4, by + br - 4, start=-ang, extent=100,
                                 style="arc", outline=ACCENT, width=4)
                elif res and res[0] == "ok":
                    c.create_oval(bx - br, by - br, bx + br, by + br, fill=GREEN, outline=PANEL, width=3)
                    c.create_line(bx - 8, by, bx - 2, by + 6, bx + 9, by - 6, fill="#ffffff", width=4,
                                  capstyle="round", joinstyle="round")
                elif res and res[0] == "error":
                    c.create_oval(bx - br, by - br, bx + br, by + br, fill=RED, outline=PANEL, width=3)
                    c.create_line(bx - 6, by - 6, bx + 6, by + 6, fill="#ffffff", width=4, capstyle="round")
                    c.create_line(bx - 6, by + 6, bx + 6, by - 6, fill="#ffffff", width=4, capstyle="round")
                elif res and res[0] == "notcv":
                    c.create_oval(bx - br, by - br, bx + br, by + br, fill=VIOLET, outline=PANEL, width=3)
                    badge = {"V1": "1", "CB": "CB", "CPS": "CPS", "OPENPGP": "PGP", "PIV": "PIV",
                             "ID": "ID"}.get(res[1], "?")
                    c.create_text(bx, by, text=badge, fill="#ffffff",
                                  font=(UI, 12 if len(badge) == 1 else 9 if len(badge) == 2 else 7, "bold"))
            elif present:
                # emplacement libre : carte fantome + fleche
                ghost = mix(GREEN, PANEL, 0.45)
                rrect(c, 100, 34, W - 100, 118, 11, fill="", outline=ghost, width=2, dash=(5, 4))
                c.create_line(W / 2, 48, W / 2, 94, fill=mix(GREEN, PANEL, 0.3), width=3, arrow="last",
                              arrowshape=(12, 14, 6))
                c.create_text(W / 2, 107, text="insérer la carte", fill=mix(GREEN, PANEL, 0.35), font=(UI, 8))
            elif absent:
                c.create_text(W / 2, 78, text="aucun lecteur", fill=mix(RED, PANEL, 0.25), font=(UI, 12, "bold"))
            elif on and s.booting:
                c.create_text(W / 2, 78, text="recherche…", fill=ACCENT, font=(UI, 12, "bold"))

            # voyant et marquage du lecteur
            if present:
                lc = GREEN if not s.reading else (GREEN if (self.phase % 0.3) < 0.15 else mix(GREEN, body, 0.7))
                c.create_oval(W - 76, 168, W - 62, 182, fill=lc, outline=mix(lc, "#000000", 0.3))
                c.create_text(62, 166, text="LECTEUR PC/SC", anchor="w", fill=mix(body, "#ffffff", 0.45),
                              font=(UI, 8, "bold"))
                name = self.state.info.get("NAME", "")
                if name and name != "?":
                    short = name if len(name) <= 24 else name[:23] + "…"
                    c.create_text(62, 180, text=short, anchor="w", fill=mix(body, "#ffffff", 0.3),
                                  font=(UI, 7))

        def animate(self) -> None:
            self.phase += 0.05
            self.state.tick()
            s = self.state
            if s.port_open and (s.booting or s.reading or (s.reader and not s.card)):
                if s.reading is False and self.lbl_state.cget("text").startswith("Lecture"):
                    self.update_side()
                else:
                    self.draw()
            self.root.after(50, self.animate)

        # -- auto-test (developpement) : PCSC_TERM_SELFTEST=<dossier> + --demo ---------
        def selftest(self) -> None:
            out_dir = os.environ["PCSC_TERM_SELFTEST"]
            os.makedirs(out_dir, exist_ok=True)
            self.root.attributes("-topmost", True)

            def shot(name):
                from PIL import ImageGrab
                self.root.update()
                x, y = self.root.winfo_rootx(), self.root.winfo_rooty()
                w, h = self.root.winfo_width(), self.root.winfo_height()
                ImageGrab.grab(bbox=(x, y, x + w, y + h)).save(os.path.join(out_dir, name + ".png"))

            steps = [
                (200, lambda: shot("01_port_ferme")),
                (400, self.connect),
                (2800, lambda: shot("02_pret")),
                (2900, lambda: self.send("DEMO VITALE")),
                (3500, lambda: shot("03_lecture")),
                (4600, lambda: shot("04_vitale_lue")),
                (4700, lambda: self.send("DEBUG ON")),
                (5000, lambda: self.send("DEMO BANK")),
                (6800, lambda: shot("05_carte_bancaire_debug")),
                (6850, lambda: self.send("DEMO CPS")),
                (8300, lambda: shot("05b_carte_cps")),
                (8400, lambda: self.send("DEMO ERROR")),
                (9900, lambda: shot("06_erreur")),
                (10000, lambda: self.send("DEMO UNPLUG")),
                (10800, lambda: shot("07_lecteur_absent")),
                (10900, lambda: self.send("DEMO PLUG")),
                (11700, lambda: self.send("INFO")),
                (12000, lambda: self.show_tab("usb")),
                (12300, lambda: shot("08_onglet_lecteur_usb")),
                (12500, self.on_close),
            ]
            if not self.demo:                         # vrai module : rien que des commandes du protocole
                steps = [
                    (200, lambda: shot("r1_port_ferme")),
                    (400, self.connect),
                    (3500, lambda: shot("r2_connecte")),
                    (3600, lambda: self.show_tab("usb")),
                    (3900, lambda: shot("r3_onglet_lecteur_usb")),
                    (4200, self.on_close),
                ]
            for ms, fn in steps:
                self.root.after(ms, fn)

        # -- fermeture ---------------------------------------------------------------
        def on_close(self) -> None:
            self.settings.update({
                "geometry": self.root.winfo_geometry(), "sync": self.sync_var.get(),
                "show_dbg": self.v_dbg.get(), "show_noise": self.v_noise.get(),
                "show_ts": self.v_ts.get(), "mask_nir": self.mask_var.get(),
            })
            if not os.environ.get("PCSC_TERM_SELFTEST"):
                save_settings(self.settings)
            self.disconnect("fermeture")
            self.root.destroy()

        def run(self) -> None:
            self.root.mainloop()

    App().run()
    return 0


# =============================================================================
def main() -> int:
    argv = sys.argv[1:]
    if argv and argv[0] == "--make-icon":                 # utilise par build_exe.bat
        write_ico(argv[1] if len(argv) > 1 else "pcsc_term.ico")
        return 0
    if "--demo" in argv:                                   # interface + module simule
        return run_gui(demo=True)
    if getattr(sys, "frozen", False) or not argv or argv[0] == "--gui":
        return run_gui()
    return run_cli(argv)


if __name__ == "__main__":
    sys.exit(main())
