# Создание нового Python сервиса (FastAPI)

## 1. Структура

```
services/service-<name>/
  pyproject.toml
  Dockerfile
  app/
    main.py
```

## 2. pyproject.toml

```toml
[project]
name = "service-<name>"
version = "0.1.0"
requires-python = ">=3.12"
dependencies = [
    "fastapi==0.115.0",
    "uvicorn[standard]==0.32.0",
    "pydantic==2.9.2",
    "sqlalchemy[asyncio]==2.0.36",
    "asyncpg==0.30.0",
    "pyjwt[crypto]==2.9.0",
    "cryptography==43.0.1",
]
```

Добавляй зависимости по мере необходимости. `alembic` — если нужны миграции из Python.

## 3. app/main.py

```python
from contextlib import asynccontextmanager
import os
from typing import Annotated

import jwt
from fastapi import FastAPI, Depends, HTTPException, Header
from pydantic import BaseModel
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy import text


# --- Конфигурация через env ---

PG_DSN = os.getenv(
    "PG_DSN",
    "postgresql+asyncpg://<name>:<name>@pg-<name>:5432/<name>",
)
JWT_PUBLIC_KEY_PATH = os.getenv("JWT_PUBLIC_KEY_PATH", "/etc/keys/jwt_public.pem")

with open(JWT_PUBLIC_KEY_PATH, "rb") as f:
    JWT_PUBLIC_KEY = f.read()


# --- БД ---

engine = create_async_engine(PG_DSN, pool_size=5, max_overflow=10)
SessionLocal = async_sessionmaker(engine, expire_on_commit=False)


async def get_db() -> AsyncSession:
    async with SessionLocal() as s:
        yield s


# --- JWT ---

def require_user(authorization: Annotated[str, Header()] = "") -> str:
    """Dependency: возвращает user_id или бросает 401."""
    if not authorization.startswith("Bearer "):
        raise HTTPException(401, "missing bearer token")
    token = authorization[len("Bearer "):]
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


# --- App ---

@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    await engine.dispose()


app = FastAPI(title="service-<name>", lifespan=lifespan)


# --- Healthcheck (обязателен) ---

@app.get("/health")
async def health(session: Annotated[AsyncSession, Depends(get_db)]):
    try:
        await session.execute(text("SELECT 1"))
        pg_ok = True
    except Exception:
        pg_ok = False
    return {
        "ok": pg_ok,
        "service": "service-<name>",
        "version": "0.1.0",
        "postgres_ok": pg_ok,
    }


# --- Ручки ---

class MyResponse(BaseModel):
    user_id: str
    message: str


@app.get("/api/<name>/hello", response_model=MyResponse)
async def hello(user_id: Annotated[str, Depends(require_user)]):
    """Пример защищённой ручки."""
    return MyResponse(user_id=user_id, message="hello from service-<name>")


@app.post("/api/<name>/items")
async def create_item(
    user_id: Annotated[str, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_db)],
):
    """Пример ручки с БД."""
    result = await session.execute(
        text("INSERT INTO items (owner_id) VALUES (:uid) RETURNING id"),
        {"uid": user_id},
    )
    await session.commit()
    row = result.fetchone()
    return {"id": str(row[0]), "owner": user_id}
```

## 4. Dockerfile

```dockerfile
FROM python:3.12-slim

WORKDIR /app

COPY services/service-<name>/pyproject.toml .
RUN pip install --no-cache-dir .

COPY services/service-<name>/app/ /app/

EXPOSE 8000
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"]
```

## 5. Helm values (infra/helm/values-service-<name>.yaml)

```yaml
replicaCount: 1

image:
  repository: localhost:5000/service-<name>
  tag: latest

service:
  port: 8000                    # FastAPI/uvicorn по умолчанию на 8000

config:
  enabled: false                # Python-сервисы конфигурируются через env

jwtKeys:
  private: false
  public: true
  publicFile: jwt_public.pem    # .pem для PyJWT (не .jwk)
  mountPath: /etc/keys

env:
  - name: PG_DSN
    value: "postgresql+asyncpg://<name>:<name>@pg-<name>-postgresql.data.svc.cluster.local:5432/<name>"
  - name: JWT_PUBLIC_KEY_PATH
    value: /etc/keys/jwt_public.pem

migrations:
  enabled: true
  image: postgres:16
  files:
    001_init.sql: |
      CREATE TABLE IF NOT EXISTS items (
          id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
          owner_id UUID NOT NULL,
          created_at TIMESTAMPTZ NOT NULL DEFAULT now()
      );
  db:
    host: pg-<name>-postgresql.data.svc.cluster.local
    port: "5432"
    name: <name>
    user: <name>
    password: <name>

ingress:
  enabled: true
  match: "PathPrefix(`/api/<name>`)"

cors:
  enabled: false

networkPolicy:
  enabled: true

healthcheck:
  path: /health
```

## 6. Регистрация в Makefile

В `K3S_SERVICES` добавь:
```makefile
K3S_SERVICES := service-auth service-core service-test-python service-<name>
```

В `k3s-deploy-data` добавь PG:
```makefile
helm upgrade --install pg-<name> oci://registry-1.docker.io/bitnamicharts/postgresql \
    -n data \
    --set auth.username=<name> --set auth.password=<name> --set auth.database=<name> \
    --set primary.persistence.size=1Gi
```

## 7. Отличия от C++ сервиса

| | C++ (Drogon) | Python (FastAPI) |
|---|---|---|
| Порт | 8080 | 8000 |
| Конфигурация | config.json (ConfigMap) | env vars |
| JWT формат | `.jwk` | `.pem` |
| config.enabled | true | false |
| Зависимости | CMakeLists.txt | pyproject.toml |

## 8. Добавление ручки в существующий сервис

В `app/main.py` добавь:

```python
# Публичная ручка (без авторизации)
@app.get("/api/<name>/public-data")
async def public_data(session: Annotated[AsyncSession, Depends(get_db)]):
    result = await session.execute(text("SELECT count(*) FROM items"))
    return {"count": result.scalar()}


# Защищённая ручка (с JWT)
@app.post("/api/<name>/my-action")
async def my_action(
    user_id: Annotated[str, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_db)],
):
    # user_id уже провалидирован через JWT
    return {"user_id": user_id, "status": "done"}
```

Пересборка: `make k3s-build-service-<name> && make k3s-deploy-service-<name>`

Uvicorn в dev-режиме поддерживает hot reload, но в k3s контейнер нужно пересобирать.
