from collections.abc import Callable
from typing import Annotated

import jwt
from fastapi import Header

from app.errors import ApiError

JWT_ISSUER = "service-auth"
JWT_AUDIENCE = "internal"


def make_require_user(public_key: bytes) -> Callable[..., str]:
    def require_user(authorization: Annotated[str, Header()] = "") -> str:
        if not authorization.startswith("Bearer "):
            raise ApiError(401, "UNAUTHORIZED", "Отсутствует токен авторизации")
        token = authorization.removeprefix("Bearer ")
        try:
            claims = jwt.decode(
                token,
                public_key,
                algorithms=["RS256"],
                audience=JWT_AUDIENCE,
                issuer=JWT_ISSUER,
                options={"require": ["exp", "sub", "iss", "aud"]},
            )
        except jwt.ExpiredSignatureError:
            raise ApiError(401, "TOKEN_EXPIRED", "Срок действия токена истёк") from None
        except jwt.InvalidTokenError:
            raise ApiError(401, "INVALID_TOKEN", "Недействительный токен") from None
        return claims["sub"]

    return require_user
