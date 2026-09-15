from collections.abc import Callable
from typing import Annotated

import jwt
from cryptography.hazmat.primitives.asymmetric.rsa import RSAPublicKey
from cryptography.hazmat.primitives.serialization import load_pem_public_key
from fastapi import Header

from app.errors import ApiError

JWT_ISSUER = "service-auth"
JWT_AUDIENCE = "internal"


def make_require_user(public_key: bytes) -> Callable[..., str]:
    try:
        key = load_pem_public_key(public_key)
    except Exception as exc:
        raise ValueError("Некорректный публичный ключ JWT") from exc
    if not isinstance(key, RSAPublicKey):
        raise ValueError("Некорректный публичный ключ JWT")

    def require_user(authorization: Annotated[str, Header()] = "") -> str:
        if not authorization.lower().startswith("bearer "):
            raise ApiError(401, "UNAUTHORIZED", "Отсутствует токен авторизации")
        token = authorization[len("Bearer "):]
        try:
            claims = jwt.decode(
                token,
                key,
                algorithms=["RS256"],
                audience=JWT_AUDIENCE,
                issuer=JWT_ISSUER,
                options={"require": ["exp", "sub", "iss", "aud"]},
                leeway=30,
            )
        except jwt.ExpiredSignatureError:
            raise ApiError(401, "TOKEN_EXPIRED", "Срок действия токена истёк") from None
        except jwt.InvalidTokenError:
            raise ApiError(401, "INVALID_TOKEN", "Недействительный токен") from None
        return claims["sub"]

    return require_user
