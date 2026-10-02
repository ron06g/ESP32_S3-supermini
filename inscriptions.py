#!/usr/bin/env python3
# ===========================================================================
#  inscriptions.py  —  Pré-inscriptions « Je veux mon S3-KBD » (SQLite)
# ---------------------------------------------------------------------------
#  Stocke les e-mails laissés sur la landing (WEB/Landing/landing.html) pour
#  prévenir les inscrits quand le produit sera prêt. Aucun service tiers : une
#  base SQLite locale, data/inscriptions.db (dossier NON versionné, cf. .gitignore).
#
#  Utilisé par serve.py (route POST /api/inscription) et en ligne de commande :
#    python inscriptions.py count              nombre d'inscrits
#    python inscriptions.py list               liste (e-mail, date, usage, applis)
#    python inscriptions.py export [f.csv]     export CSV (défaut : inscriptions.csv)
#    python inscriptions.py delete <e-mail>    effacement (droit à l'effacement RGPD)
#
#  Données minimales : e-mail + consentement (version du texte + date) ; usage,
#  applis souhaitées et message sont FACULTATIFS. Pas d'adresse IP stockée.
# ===========================================================================
import csv
import datetime
import os
import re
import sqlite3
import sys

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DB_PATH  = os.path.join(BASE_DIR, "data", "inscriptions.db")

# Version du texte de consentement affiché dans la popup (landing.html).
# À incrémenter si le texte change : on garde la preuve de CE qui a été accepté.
CONSENT_VERSION = "2026-09-v1"

USAGES   = {"salon", "presentations", "bornes", "escape", "tests", "accessibilite", "bios", "autre"}
APPS     = {"android", "iphone", "windows"}
MSG_MAX  = 500
EMAIL_RE = re.compile(r"^[^@\s]{1,64}@[^@\s]+\.[^@\s.]{2,}$")

SCHEMA = """
CREATE TABLE IF NOT EXISTS inscriptions (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  email        TEXT NOT NULL UNIQUE COLLATE NOCASE,
  usage        TEXT,             -- usage principal (facultatif)
  apps         TEXT,             -- applis hors ligne souhaitées, ex. "android,windows"
  message      TEXT,             -- besoin particulier (facultatif, <= 500 car.)
  consentement TEXT NOT NULL,    -- version du texte de consentement accepté
  source       TEXT,             -- page d'origine
  cree_le      TEXT NOT NULL,    -- ISO 8601 UTC
  maj_le       TEXT              -- dernière mise à jour (réinscription)
);
"""


def _now():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()


def connect():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    db = sqlite3.connect(DB_PATH)
    db.execute(SCHEMA)
    return db


def validate(data):
    """Normalise une demande. Renvoie (champs, None) ou (None, code_erreur)."""
    if not isinstance(data, dict):
        return None, "format"
    email = str(data.get("email", "")).strip().lower()
    if len(email) > 254 or not EMAIL_RE.match(email):
        return None, "email"
    if data.get("consent") is not True:
        return None, "consent"
    usage = str(data.get("usage") or "").strip()
    if usage and usage not in USAGES:
        return None, "usage"
    apps = data.get("apps") or []
    if not isinstance(apps, list) or any(a not in APPS for a in apps):
        return None, "apps"
    message = str(data.get("message") or "").strip()[:MSG_MAX]
    source = str(data.get("source") or "")[:40]
    return {
        "email": email,
        "usage": usage or None,
        "apps": ",".join(sorted(set(apps))) or None,
        "message": message or None,
        "source": source or None,
    }, None


def add(fields):
    """Insère, ou met à jour si l'e-mail existe déjà (réponse identique :
    on ne révèle pas si une adresse est déjà inscrite)."""
    now = _now()
    with connect() as db:
        db.execute(
            """INSERT INTO inscriptions (email, usage, apps, message, consentement, source, cree_le)
               VALUES (:email, :usage, :apps, :message, :consentement, :source, :now)
               ON CONFLICT(email) DO UPDATE SET
                 usage   = COALESCE(excluded.usage, usage),
                 apps    = COALESCE(excluded.apps, apps),
                 message = COALESCE(excluded.message, message),
                 consentement = excluded.consentement,
                 maj_le  = :now""",
            dict(fields, consentement=CONSENT_VERSION, now=now))


# ---------------------------------------------------------------------------
#  Ligne de commande (administration)
# ---------------------------------------------------------------------------
def _rows():
    with connect() as db:
        return db.execute(
            "SELECT email, cree_le, usage, apps, message, consentement, source, maj_le "
            "FROM inscriptions ORDER BY cree_le").fetchall()


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "count"
    if cmd == "count":
        print(len(_rows()), "inscrit(s)")
    elif cmd == "list":
        for email, cree, usage, apps, msg, *_ in _rows():
            print(f"{cree}  {email:<40} {usage or '-':<14} {apps or '-':<22} {msg or ''}")
    elif cmd == "export":
        out = argv[2] if len(argv) > 2 else "inscriptions.csv"
        with open(out, "w", newline="", encoding="utf-8-sig") as f:   # BOM : ouverture propre dans Excel
            w = csv.writer(f, delimiter=";")
            w.writerow(["email", "cree_le", "usage", "apps", "message", "consentement", "source", "maj_le"])
            w.writerows(_rows())
        print("Export ->", out)
    elif cmd == "delete" and len(argv) > 2:
        with connect() as db:
            n = db.execute("DELETE FROM inscriptions WHERE email = ?", (argv[2].strip().lower(),)).rowcount
        print("Supprimé" if n else "Adresse introuvable")
    else:
        print("Usage : python inscriptions.py count|list|export [f.csv]|delete <e-mail>")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
