import logging
from collections.abc import Callable
from typing import Annotated, Any

from fastapi import APIRouter, Depends
from fastapi.responses import JSONResponse

from app.domain import (
    SCENARIO_ORDER,
    DomainError,
    build_quotes,
    calculate_commission,
    corridor_limit_error,
    validate_transfer,
)
from app.repository import CorridorRepository
from app.schemas import TransferRequest, calculate_json, quotes_json

logger = logging.getLogger(__name__)

SERVICE_NAME = "service-commission"
SERVICE_VERSION = "1.0.0"


def create_router(repo: CorridorRepository, require_user: Callable[..., str]) -> APIRouter:
    router = APIRouter()
    User = Annotated[str, Depends(require_user)]

    @router.get("/health", include_in_schema=False)
    async def health() -> JSONResponse:
        postgres_ok = await repo.ping()
        return JSONResponse(
            status_code=200 if postgres_ok else 503,
            content={"ok": postgres_ok, "service": SERVICE_NAME, "version": SERVICE_VERSION, "postgres_ok": postgres_ok},
        )

    @router.post("/api/commission/calculate", summary="Комиссия по коридору (совместимо с ivrpy)")
    async def calculate(body: TransferRequest, _user: User) -> dict[str, Any]:
        validate_transfer(body.from_country, body.to_country, body.amount)
        corridor = await repo.find_corridor(body.from_country, body.to_country, body.currency)
        if corridor is None:
            raise DomainError(
                "CORRIDOR_NOT_FOUND",
                f"Коридор {body.from_country}→{body.to_country} в {body.currency} не поддерживается",
            )
        limit_error = corridor_limit_error(corridor, body.amount)
        if limit_error is not None:
            raise limit_error
        return calculate_json(corridor, body.amount, calculate_commission(corridor, body.amount))

    @router.post("/api/commission/quotes", summary="Котировки по всем сценариям расчёта")
    async def quotes(body: TransferRequest, _user: User) -> dict[str, Any]:
        validate_transfer(body.from_country, body.to_country, body.amount)
        corridor = await repo.find_corridor(body.from_country, body.to_country, body.currency)
        profiles = await repo.list_profiles()
        quotes = build_quotes(corridor, profiles, body.from_country, body.to_country, body.currency, body.amount)
        if len(quotes) != len(SCENARIO_ORDER):
            missing = set(SCENARIO_ORDER) - {q.profile.scenario for q in quotes}
            for scenario in missing:
                logger.error("scenario profile missing: %s", scenario)
        return quotes_json(corridor, quotes)

    return router
