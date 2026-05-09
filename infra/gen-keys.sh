#!/usr/bin/env bash
# Генерирует пару RSA-ключей для подписи JWT.
# Запускать один раз перед первым `make up`.
set -euo pipefail

cd "$(dirname "$0")/.."

mkdir -p infra/keys

if [[ -f infra/keys/jwt_private.pem && -f infra/keys/jwt_private.jwk ]]; then
    echo "Ключи уже существуют, ничего не делаю"
    exit 0
fi

openssl genpkey -algorithm RSA -out infra/keys/jwt_private.pem -pkeyopt rsa_keygen_bits:2048
openssl rsa -pubout -in infra/keys/jwt_private.pem -out infra/keys/jwt_public.pem

# PEM -> JWK (libjwt 3.x принимает только JWK формат)
python3 -c "
import json, base64
from cryptography.hazmat.primitives.serialization import load_pem_private_key, load_pem_public_key
from cryptography.hazmat.primitives.asymmetric.rsa import RSAPrivateKey

def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()

def int_to_bytes(n):
    length = (n.bit_length() + 7) // 8
    return n.to_bytes(length, 'big')

with open('infra/keys/jwt_private.pem', 'rb') as f:
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

with open('infra/keys/jwt_private.jwk', 'w') as f:
    json.dump(jwk_private, f, indent=2)

with open('infra/keys/jwt_public.jwk', 'w') as f:
    json.dump(jwk_public, f, indent=2)
"

chmod 600 infra/keys/jwt_private.pem infra/keys/jwt_private.jwk
chmod 644 infra/keys/jwt_public.pem infra/keys/jwt_public.jwk

echo "Created:"
echo "  infra/keys/jwt_private.pem + .jwk  (only service-auth)"
echo "  infra/keys/jwt_public.pem  + .jwk  (раздаётся всем сервисам)"
