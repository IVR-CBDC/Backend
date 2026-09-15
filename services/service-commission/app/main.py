from collections.abc import AsyncIterator, Callable
from contextlib import AbstractAsyncContextManager, asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from sqlalchemy.ext.asyncio import create_async_engine

from app.api import SERVICE_NAME, SERVICE_VERSION, create_router
from app.auth import make_require_user
from app.config import Settings
from app.errors import install_error_handlers
from app.repository import CorridorRepository, PgCorridorRepository

Lifespan = Callable[[FastAPI], AbstractAsyncContextManager[None]]


def create_app(repo: CorridorRepository, jwt_public_key: bytes, lifespan: Lifespan | None = None) -> FastAPI:
    app = FastAPI(title=SERVICE_NAME, version=SERVICE_VERSION, lifespan=lifespan)
    install_error_handlers(app)
    app.include_router(create_router(repo, make_require_user(jwt_public_key)))
    return app


def build_default_app() -> FastAPI:
    settings = Settings.from_env()
    engine = create_async_engine(
        settings.pg_dsn,
        pool_size=5,
        max_overflow=10,
        pool_pre_ping=True,
        connect_args={"timeout": 3, "command_timeout": 5},
    )

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncIterator[None]:
        yield
        await engine.dispose()

    public_key = Path(settings.jwt_public_key_path).read_bytes()
    return create_app(PgCorridorRepository(engine), public_key, lifespan)
