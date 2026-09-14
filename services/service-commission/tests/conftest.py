import time
from collections.abc import Callable

import jwt
import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from fastapi.testclient import TestClient

from app.main import create_app
from tests.factories import FakeRepository


def _generate_rsa_pem() -> tuple[bytes, bytes]:
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    private_pem = key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    )
    public_pem = key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
    )
    return private_pem, public_pem


@pytest.fixture(scope="session")
def rsa_keys() -> tuple[bytes, bytes]:
    return _generate_rsa_pem()


@pytest.fixture(scope="session")
def foreign_private_key() -> bytes:
    return _generate_rsa_pem()[0]


@pytest.fixture
def make_token(rsa_keys) -> Callable[..., str]:
    def _make(
        sub: str | None = "user-1",
        ttl: int = 300,
        issuer: str = "service-auth",
        audience: str = "internal",
        key: bytes | None = None,
    ) -> str:
        now = int(time.time())
        claims = {"iss": issuer, "aud": audience, "iat": now, "exp": now + ttl}
        if sub is not None:
            claims["sub"] = sub
        return jwt.encode(claims, key or rsa_keys[0], algorithm="RS256")

    return _make


@pytest.fixture
def auth_headers(make_token) -> dict[str, str]:
    return {"Authorization": f"Bearer {make_token()}"}


@pytest.fixture
def client(rsa_keys) -> TestClient:
    return TestClient(create_app(FakeRepository(), rsa_keys[1]))
