#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Génère S3/hid_firmware/web_assets.h à partir de WEB/Keyboard/.

- Les fichiers texte (html/js/css/manifest/json/svg) sont gzippés ; les binaires
  déjà compressés (png/ico) sont embarqués tels quels.
- Sortie : tableaux PROGMEM + une table de routage { path, data, len, mime, gzip }.
- L'app est servie sous le préfixe /Keyboard/ (les références de l'app sont relatives).

Usage :  python S3/hid_firmware/tools/gen_web_assets.py
"""
import gzip
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SRC_DIR = os.path.join(REPO, "WEB", "Keyboard")
OUT = os.path.join(REPO, "S3", "hid_firmware", "web_assets.h")

URL_PREFIX = "/Keyboard/"

# Extensions embarquées (le reste, ex. README.md, est ignoré).
MIME = {
    ".html": "text/html; charset=utf-8",
    ".js":   "application/javascript; charset=utf-8",
    ".css":  "text/css; charset=utf-8",
    ".webmanifest": "application/manifest+json; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg":  "image/svg+xml",
    ".png":  "image/png",
    ".ico":  "image/x-icon",
}
# Types gzippés (texte). Les png/ico sont déjà compressés.
GZIP_EXT = {".html", ".js", ".css", ".webmanifest", ".json", ".svg"}

# Routes supplémentaires pointant sur le même fichier.
ALIASES = {
    "index.html":  [URL_PREFIX],            # /Keyboard/  -> index.html
    "accept.html": [URL_PREFIX + "accept"], # /Keyboard/accept -> page « Accepter »
}


def collect():
    files = []
    for root, _dirs, names in os.walk(SRC_DIR):
        for n in sorted(names):
            ext = os.path.splitext(n)[1].lower()
            if ext not in MIME:
                continue
            full = os.path.join(root, n)
            rel = os.path.relpath(full, SRC_DIR).replace(os.sep, "/")
            files.append((rel, full, ext))
    files.sort(key=lambda t: t[0])
    return files


def c_array(name, data):
    out = ["static const uint8_t %s[] PROGMEM = {" % name]
    line = "  "
    for i, b in enumerate(data):
        line += "0x%02x," % b
        if (i + 1) % 16 == 0:
            out.append(line)
            line = "  "
    if line.strip():
        out.append(line)
    out.append("};")
    return "\n".join(out)


def main():
    if not os.path.isdir(SRC_DIR):
        sys.exit("Dossier source introuvable : %s" % SRC_DIR)

    files = collect()
    if not files:
        sys.exit("Aucun asset trouvé dans %s" % SRC_DIR)

    arrays = []
    rows = []
    total_raw = total_emb = 0

    for idx, (rel, full, ext) in enumerate(files):
        with open(full, "rb") as f:
            raw = f.read()
        do_gzip = ext in GZIP_EXT
        data = gzip.compress(raw, 9, mtime=0) if do_gzip else raw
        name = "asset_%03d" % idx
        arrays.append(c_array(name, data))

        mime = MIME[ext]
        gz = "true" if do_gzip else "false"
        url = URL_PREFIX + rel
        rows.append('  { "%s", %s, sizeof(%s), "%s", %s },' % (url, name, name, mime, gz))
        for alias in ALIASES.get(rel, []):
            rows.append('  { "%s", %s, sizeof(%s), "%s", %s },' % (alias, name, name, mime, gz))

        total_raw += len(raw)
        total_emb += len(data)
        print("  %-28s %6d o -> %6d o %s" % (rel, len(raw), len(data), "(gz)" if do_gzip else ""))

    header = []
    header.append("// ============================================================================")
    header.append("//  web_assets.h — GÉNÉRÉ par tools/gen_web_assets.py — NE PAS ÉDITER À LA MAIN.")
    header.append("//  App WEB/Keyboard/ embarquée (gzip pour le texte), servie sous /Keyboard/.")
    header.append("// ============================================================================")
    header.append("#pragma once")
    header.append("#include <stddef.h>")
    header.append("#include <stdint.h>")
    header.append("#include <pgmspace.h>")
    header.append("")
    header.append("\n\n".join(arrays))
    header.append("")
    header.append("struct WebAsset { const char* path; const uint8_t* data; size_t len; const char* mime; bool gzip; };")
    header.append("")
    header.append("static const WebAsset WEB_ASSETS[] = {")
    header.append("\n".join(rows))
    header.append("};")
    header.append("static const size_t WEB_ASSETS_COUNT = sizeof(WEB_ASSETS) / sizeof(WEB_ASSETS[0]);")
    header.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(header))

    print("-" * 60)
    print("  %d fichiers, %d routes" % (len(files), len(rows)))
    print("  brut %d o -> embarqué %d o" % (total_raw, total_emb))
    print("  écrit : %s" % OUT)


if __name__ == "__main__":
    main()
