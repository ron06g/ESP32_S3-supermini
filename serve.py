#!/usr/bin/env python3
# ===========================================================================
#  serve.py  —  Serveur HTTPS local pour le dossier WEB/
# ---------------------------------------------------------------------------
#  - Genere un certificat auto-signe dans WEB/ssl/ s'il manque (avec les IP
#    du PC en SAN, pour limiter les avertissements).
#  - Sert le dossier WEB/ en HTTPS (Web Bluetooth exige un contexte securise :
#    HTTPS ou localhost). Depuis un telephone Android, ouvrir https://<IP>:8443
#    puis accepter l'avertissement de certificat (auto-signe).
#
#  - Recoit les pre-inscriptions de la landing (POST /api/inscription) et les
#    range dans data/inscriptions.db (SQLite, cf. inscriptions.py).
#
#  Usage : python serve.py [port] [--http]   (defaut 8443, HTTPS)
#          --http : HTTP simple sur localhost uniquement (test local de la landing)
#  iOS reste impossible (aucun navigateur iOS n'a Web Bluetooth).
# ===========================================================================
import http.server
import ssl
import socket
import sys
import os
import datetime
import ipaddress
import json
import threading
import time

import inscriptions

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
WEB_DIR  = os.path.join(BASE_DIR, "WEB")
SSL_DIR  = os.path.join(WEB_DIR, "ssl")
CERT     = os.path.join(SSL_DIR, "cert.pem")
KEY      = os.path.join(SSL_DIR, "key.pem")
ARGS     = [a for a in sys.argv[1:] if not a.startswith("--")]
PORT     = int(ARGS[0]) if ARGS else 8443
HTTP     = "--http" in sys.argv[1:]

# Anti-abus de /api/inscription : N envois max par adresse IP et par fenetre.
SIGNUP_PATH   = "/api/inscription"
SIGNUP_MAX    = 8
SIGNUP_WIN_S  = 600
BODY_MAX      = 4096
_hits, _hits_lock = {}, threading.Lock()


def local_ipv4s():
    """Toutes les IPv4 de la machine (pour les mettre dans le certificat)."""
    ips = set(["127.0.0.1"])
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except Exception:
        pass
    try:  # IP de sortie principale
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80)); ips.add(s.getsockname()[0]); s.close()
    except Exception:
        pass
    return sorted(ips)


def generate_cert():
    """Certificat auto-signe (RSA 2048, ~10 ans) avec SAN = localhost + IP."""
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    os.makedirs(SSL_DIR, exist_ok=True)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    alt_names = [x509.DNSName("localhost")]
    for ip in local_ipv4s():
        try:
            alt_names.append(x509.IPAddress(ipaddress.ip_address(ip)))
        except ValueError:
            pass

    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, u"S3-KBD (self-signed)"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, u"S3-KBD"),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.SubjectAlternativeName(alt_names), critical=False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    with open(KEY, "wb") as f:
        f.write(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
    with open(CERT, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    print(f"[serve] Certificat auto-signe genere -> {os.path.relpath(CERT, BASE_DIR)}")
    print(f"[serve] SAN : {', '.join(local_ipv4s())}, localhost")


class Handler(http.server.SimpleHTTPRequestHandler):
    """Sert WEB/ et refuse l'acces au dossier des certificats (WEB/ssl)."""
    def __init__(self, *a, **k):
        super().__init__(*a, directory=WEB_DIR, **k)

    def send_head(self):
        path = self.path.split("?", 1)[0].split("#", 1)[0]
        if path.lower().startswith("/ssl"):
            self.send_error(403, "Forbidden")
            return None
        return super().send_head()

    # --- Pre-inscriptions (landing) ---------------------------------------
    def _json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _too_many(self):
        ip, now = self.client_address[0], time.monotonic()
        with _hits_lock:
            recent = [t for t in _hits.get(ip, []) if now - t < SIGNUP_WIN_S]
            recent.append(now)
            _hits[ip] = recent
            return len(recent) > SIGNUP_MAX

    def do_POST(self):
        if self.path.split("?", 1)[0] != SIGNUP_PATH:
            self.send_error(404, "Not Found")
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = -1
        if n <= 0 or n > BODY_MAX:
            return self._json(413 if n > BODY_MAX else 400, {"ok": False, "err": "format"})
        if self._too_many():
            return self._json(429, {"ok": False, "err": "rate"})
        try:
            data = json.loads(self.rfile.read(n).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return self._json(400, {"ok": False, "err": "format"})
        if isinstance(data, dict) and data.get("site"):       # pot de miel rempli : robot
            return self._json(200, {"ok": True})               # reponse neutre, rien n'est stocke
        fields, err = inscriptions.validate(data)
        if err:
            return self._json(400, {"ok": False, "err": err})
        try:
            inscriptions.add(fields)
        except Exception as e:                                  # base verrouillee, disque plein...
            print(f"[serve] inscription : erreur base ({e})")
            return self._json(500, {"ok": False, "err": "db"})
        print(f"[serve] inscription enregistree ({inscriptions.DB_PATH})")
        return self._json(200, {"ok": True})


def main():
    if HTTP:                                  # test local de la landing : pas de TLS, localhost seul
        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
        print(f"\n[serve] HTTP (test local) sur le port {PORT} — Ctrl+C pour arreter.")
        print(f"        Landing : http://localhost:{PORT}/Landing/landing.html\n")
    else:
        if not (os.path.exists(CERT) and os.path.exists(KEY)):
            print("[serve] Pas de certificat, generation...")
            generate_cert()
        else:
            print("[serve] Certificat existant reutilise (WEB/ssl/).")

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(CERT, KEY)

        httpd = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

        print(f"\n[serve] HTTPS actif sur le port {PORT} — Ctrl+C pour arreter.")
        print(f"        Sur ce PC     : https://localhost:{PORT}/")
        for ip in local_ipv4s():
            if ip != "127.0.0.1":
                print(f"        Sur le tel.   : https://{ip}:{PORT}/   (accepter l'avertissement)")
        print(f"        Landing       : https://localhost:{PORT}/Landing/landing.html")
        print()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[serve] Arret.")
        httpd.server_close()


if __name__ == "__main__":
    main()
