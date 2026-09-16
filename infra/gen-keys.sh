#!/usr/bin/env bash
# Генерирует пару RSA-ключей для подписи JWT.
# Запускать один раз перед первым `make up`.
set -euo pipefail

cd "$(dirname "$0")/.."

OUT_DIR="${1:-infra/keys}"
mkdir -p "$OUT_DIR"

if [[ -f "$OUT_DIR/jwt_private.pem" && -f "$OUT_DIR/jwt_private.jwk" ]]; then
    echo "Ключи уже существуют в $OUT_DIR, ничего не делаю"
    exit 0
fi

openssl genpkey -algorithm RSA -out "$OUT_DIR/jwt_private.pem" -pkeyopt rsa_keygen_bits:2048
openssl rsa -pubout -in "$OUT_DIR/jwt_private.pem" -out "$OUT_DIR/jwt_public.pem"

# PEM -> JWK (libjwt 3.x принимает только JWK формат)
export OUT_DIR
python3 -c "
import json, base64, os
from cryptography.hazmat.primitives.serialization import load_pem_private_key, load_pem_public_key
from cryptography.hazmat.primitives.asymmetric.rsa import RSAPrivateKey

def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()

def int_to_bytes(n):
    length = (n.bit_length() + 7) // 8
    return n.to_bytes(length, 'big')

out = os.environ['OUT_DIR']

with open(f'{out}/jwt_private.pem', 'rb') as f:
    priv = load_pem_private_key(f.read(), password=None)

pn = priv.private_numbers()
pub = pn.public_numbers

jwk_private = {
    'kty': 'RSA',
    'kid': 'service-auth-key',
    'use': 'sig',
    'alg': 'RS256',
    'n': b64url(int_to_bytes(pub.n)),
    'e': b64url(int_to_bytes(pub.e)),
    'd': b64url(int_to_bytes(pn.d)),
    'p': b64url(int_to_bytes(pn.p)),
    'q': b64url(int_to_bytes(pn.q)),
    'dp': b64url(int_to_bytes(pn.dmp1)),
    'dq': b64url(int_to_bytes(pn.dmq1)),
    'qi': b64url(int_to_bytes(pn.iqmp)),
}

jwk_public = {
    'kty': 'RSA',
    'kid': 'service-auth-key',
    'use': 'sig',
    'alg': 'RS256',
    'n': jwk_private['n'],
    'e': jwk_private['e'],
}

with open(f'{out}/jwt_private.jwk', 'w') as f:
    json.dump(jwk_private, f, indent=2)

with open(f'{out}/jwt_public.jwk', 'w') as f:
    json.dump(jwk_public, f, indent=2)
"

chmod 600 "$OUT_DIR/jwt_private.pem" "$OUT_DIR/jwt_private.jwk"
chmod 644 "$OUT_DIR/jwt_public.pem" "$OUT_DIR/jwt_public.jwk"

echo "Created:"
echo "  $OUT_DIR/jwt_private.pem + .jwk  (only service-auth)"
echo "  $OUT_DIR/jwt_public.pem  + .jwk  (раздаётся всем сервисам)"
