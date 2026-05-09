#!/usr/bin/env bash
# Создание нового Python (FastAPI) сервиса
# Использование: bash infra/new-service-python.sh <name>
# Пример:       bash infra/new-service-python.sh analytics
set -euo pipefail

if [ -z "${1:-}" ]; then
  echo "Использование: $0 <name>"
  echo "Пример: $0 analytics"
  exit 1
fi

NAME="$1"
SVC="service-${NAME}"
SVC_DIR="services/${SVC}"
# PG user/db: дефисы -> подчёркивания (PostgreSQL не любит дефисы)
PG_NAME="$(echo "${NAME}" | tr '-' '_')"

cd "$(dirname "$0")/.."

if [ -d "${SVC_DIR}" ]; then
  echo "ОШИБКА: ${SVC_DIR} уже существует"
  exit 1
fi

# --- 1. Структура ---
echo "[1/6] Создаю структуру ${SVC_DIR}/..."
mkdir -p "${SVC_DIR}/app"

# --- 2. pyproject.toml ---
echo "[2/6] Генерирую pyproject.toml..."
cat > "${SVC_DIR}/pyproject.toml" << TOMLEOF
[project]
name = "${SVC}"
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
TOMLEOF

# --- 3. main.py ---
echo "[3/6] Генерирую app/main.py..."
cat > "${SVC_DIR}/app/main.py" << PYEOF
from contextlib import asynccontextmanager
import os
from typing import Annotated

import jwt
from fastapi import FastAPI, Depends, HTTPException, Header
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy import text


PG_DSN = os.getenv(
    "PG_DSN",
    "postgresql+asyncpg://${PG_NAME}:${PG_NAME}@pg-${NAME}:5432/${PG_NAME}",
)
JWT_PUBLIC_KEY_PATH = os.getenv("JWT_PUBLIC_KEY_PATH", "/etc/keys/jwt_public.pem")

with open(JWT_PUBLIC_KEY_PATH, "rb") as f:
    JWT_PUBLIC_KEY = f.read()

engine = create_async_engine(PG_DSN, pool_size=5, max_overflow=10)
SessionLocal = async_sessionmaker(engine, expire_on_commit=False)


async def get_db() -> AsyncSession:
    async with SessionLocal() as s:
        yield s


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


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    await engine.dispose()


app = FastAPI(title="${SVC}", lifespan=lifespan)


@app.get("/health")
async def health(session: Annotated[AsyncSession, Depends(get_db)]):
    try:
        await session.execute(text("SELECT 1"))
        pg_ok = True
    except Exception:
        pg_ok = False
    return {
        "ok": pg_ok,
        "service": "${SVC}",
        "version": "0.1.0",
        "postgres_ok": pg_ok,
    }
PYEOF

# --- 4. Dockerfile ---
echo "[4/6] Генерирую Dockerfile..."
cat > "${SVC_DIR}/Dockerfile" << DEOF
FROM python:3.12-slim

WORKDIR /app

COPY services/${SVC}/pyproject.toml .
RUN pip install --no-cache-dir .

COPY services/${SVC}/app/ /app/

EXPOSE 8000
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"]
DEOF

# --- 5. Helm values ---
echo "[5/6] Генерирую Helm values..."
cat > "infra/helm/values-${SVC}.yaml" << VEOF
replicaCount: 1

image:
  repository: localhost:5000/${SVC}
  tag: latest

service:
  port: 8000

config:
  enabled: false

jwtKeys:
  private: false
  public: true
  publicFile: jwt_public.pem
  mountPath: /etc/keys

env:
  - name: PG_DSN
    value: "postgresql+asyncpg://${PG_NAME}:${PG_NAME}@pg-${NAME}-postgresql.data.svc.cluster.local:5432/${PG_NAME}"
  - name: JWT_PUBLIC_KEY_PATH
    value: /etc/keys/jwt_public.pem

migrations:
  enabled: true
  image: postgres:16
  files:
    001_init.sql: |
      CREATE TABLE IF NOT EXISTS _placeholder (id SERIAL PRIMARY KEY);
  db:
    host: pg-${NAME}-postgresql.data.svc.cluster.local
    port: "5432"
    name: ${PG_NAME}
    user: ${PG_NAME}
    password: ${PG_NAME}

ingress:
  enabled: true
  match: "PathPrefix(\`/api/${NAME}\`)"

cors:
  enabled: false

networkPolicy:
  enabled: true

healthcheck:
  path: /health
VEOF

# --- 6. Обновляю Makefile ---
echo "[6/6] Обновляю Makefile..."

echo "  [+] K3S_SERVICES..."
sed -i "s/^K3S_SERVICES := .*/& ${SVC}/" Makefile

echo "  [+] k3s-deploy-data..."
sed -i "/helm upgrade --install redis/i\\
\\thelm upgrade --install pg-${NAME} oci://registry-1.docker.io/bitnamicharts/postgresql \\\\\\n\\t\\t-n data \\\\\\n\\t\\t--set auth.username=${PG_NAME} --set auth.password=${PG_NAME} --set auth.database=${PG_NAME} \\\\\\n\\t\\t--set primary.persistence.size=1Gi" Makefile

echo ""
echo "=== Готово: ${SVC} (Python / FastAPI) ==="
echo ""
echo "Файлы:"
echo "  ${SVC_DIR}/app/main.py"
echo "  ${SVC_DIR}/pyproject.toml"
echo "  ${SVC_DIR}/Dockerfile"
echo "  infra/helm/values-${SVC}.yaml"
echo ""
echo "Следующие шаги:"
echo "  1. Отредактируй миграцию в values (migrations.files.001_init.sql)"
echo "  2. Добавь ручки в app/main.py"
echo "  3. make k3s-build-${SVC} && make k3s-deploy-${SVC}"
