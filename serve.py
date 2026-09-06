#!/usr/bin/env python3
# ===========================================================================
#  serve.py  —  Serveur HTTPS local (mock) pour le dossier WEB/
# ---------------------------------------------------------------------------
#  - Genere un certificat auto-signe dans WEB/ssl/ s'il manque (avec les IP
#    du PC en SAN, pour limiter les avertissements).
#  - Sert le dossier WEB/ en HTTPS (Web Bluetooth exige un contexte securise :
#    HTTPS ou localhost). Depuis un telephone Android, ouvrir https://<IP>:8443
#    puis accepter l'avertissement de certificat (auto-signe).
#
#  Usage : python serve.py [port]      (defaut 8443)
#  iOS reste impossible (aucun navigateur iOS n'a Web Bluetooth).
# ===========================================================================
import http.server
import ssl
import socket
import sys
import os
import datetime
import ipaddress

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
WEB_DIR  = os.path.join(BASE_DIR, "WEB")
SSL_DIR  = os.path.join(WEB_DIR, "ssl")
CERT     = os.path.join(SSL_DIR, "cert.pem")
KEY      = os.path.join(SSL_DIR, "key.pem")
PORT     = int(sys.argv[1]) if len(sys.argv) > 1 else 8443


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
        x509.NameAttribute(NameOID.COMMON_NAME, u"S3-KBD mock (self-signed)"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, u"POC"),
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


def main():
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
    print()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[serve] Arret.")
        httpd.server_close()


if __name__ == "__main__":
    main()
