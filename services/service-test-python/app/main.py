from contextlib import asynccontextmanager
import os
from typing import Annotated

import jwt
from fastapi import FastAPI, Depends, HTTPException, Header
from pydantic import BaseModel
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy import text


PG_DSN = os.getenv(
    "PG_DSN",
    "postgresql+asyncpg://test_python:test_python@pg-test-python:5432/test_python",
)
JWT_PUBLIC_KEY_PATH = os.getenv(
    "JWT_PUBLIC_KEY_PATH", "/etc/service-test-python/jwt_public.pem"
)


with open(JWT_PUBLIC_KEY_PATH, "rb") as f:
    JWT_PUBLIC_KEY = f.read()


engine = create_async_engine(PG_DSN, pool_size=5, max_overflow=10)
SessionLocal = async_sessionmaker(engine, expire_on_commit=False)


async def get_db() -> AsyncSession:
    async with SessionLocal() as s:
        yield s


def require_user(authorization: Annotated[str, Header()] = "") -> str:
    """
    FastAPI dependency. Возвращает user_id если токен валиден,
    бросает 401 иначе.
    """
    if not authorization.startswith("Bearer "):
        raise HTTPException(401, "missing bearer token")
    token = authorization[len("Bearer ") :]
    try:
        claims = jwt.decode(
            token,
            JWT_PUBLIC_KEY,
            algorithms=["RS256"],
            audience="internal",
            issuer="service-auth",
        )
    except jwt.ExpiredSignatureError:
        raise HTTPException(401, "token expired")
    except jwt.InvalidTokenError as e:
        raise HTTPException(401, f"invalid token: {e}")
    return claims["sub"]


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    await engine.dispose()


app = FastAPI(title="service-test-python", lifespan=lifespan)


@app.get("/health2")
async def health2(session: Annotated[AsyncSession, Depends(get_db)]):
    """Health check с проверкой БД."""
    try:
        await session.execute(text("SELECT 1"))
        pg_ok = True
    except Exception:
        pg_ok = False
    return {
        "ok": pg_ok,
        "service": "service-test-python",
        "version": "0.1.0",
        "postgres_ok": pg_ok,
    }


class PingResponse(BaseModel):
    user_id: str
    message: str


@app.get("/api/test-python/ping", response_model=PingResponse)
async def ping(user_id: Annotated[str, Depends(require_user)]):
    """Защищённая ручка — пример как использовать JWT-авторизацию."""
    return PingResponse(user_id=user_id, message="hello from test-python")
