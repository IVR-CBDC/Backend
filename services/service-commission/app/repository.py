import logging
from typing import Protocol

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncEngine

from app.domain import Corridor, ScenarioProfile

logger = logging.getLogger(__name__)


class CorridorRepository(Protocol):
    async def find_corridor(self, from_country: str, to_country: str, currency: str) -> Corridor | None: ...

    async def list_profiles(self) -> list[ScenarioProfile]: ...

    async def ping(self) -> bool: ...


_FIND_CORRIDOR = text(
    """
    SELECT corridor_id, from_country, to_country, currency, base_fee, percentage_fee, fixed_fee,
           min_limit, max_limit, min_fee, max_fee
    FROM corridors
    WHERE from_country = :from_country AND to_country = :to_country AND currency = :currency
    """
)

_LIST_PROFILES = text(
    """
    SELECT scenario, title, description, fee_multiplier, eta_label, limitations, min_amount, max_amount, countries
    FROM scenario_profiles
    """
)


class PgCorridorRepository:
    def __init__(self, engine: AsyncEngine) -> None:
        self._engine = engine

    async def find_corridor(self, from_country: str, to_country: str, currency: str) -> Corridor | None:
        async with self._engine.connect() as conn:
            result = await conn.execute(
                _FIND_CORRIDOR,
                {"from_country": from_country, "to_country": to_country, "currency": currency},
            )
            row = result.mappings().first()
        return Corridor(**row) if row else None

    async def list_profiles(self) -> list[ScenarioProfile]:
        async with self._engine.connect() as conn:
            rows = (await conn.execute(_LIST_PROFILES)).mappings().all()
        return [
            ScenarioProfile(
                scenario=row["scenario"],
                title=row["title"],
                description=row["description"],
                fee_multiplier=row["fee_multiplier"],
                eta_label=row["eta_label"],
                limitations=tuple(row["limitations"]),
                min_amount=row["min_amount"],
                max_amount=row["max_amount"],
                countries=tuple(row["countries"]) if row["countries"] is not None else None,
            )
            for row in rows
        ]

    async def ping(self) -> bool:
        try:
            async with self._engine.connect() as conn:
                await conn.execute(text("SELECT 1"))
            return True
        except Exception:
            logger.warning("postgres ping failed", exc_info=True)
            return False
