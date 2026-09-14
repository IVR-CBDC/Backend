import os
from decimal import Decimal as D

import pytest
from sqlalchemy.ext.asyncio import create_async_engine

from app.domain import Corridor
from app.repository import PgCorridorRepository

pytestmark = [
    pytest.mark.db,
    pytest.mark.skipif(not os.getenv("TEST_PG_DSN"), reason="TEST_PG_DSN не задан"),
]


@pytest.fixture
async def repo():
    engine = create_async_engine(os.environ["TEST_PG_DSN"])
    yield PgCorridorRepository(engine)
    await engine.dispose()


async def test_csv_corridor_is_migrated_one_to_one(repo):
    assert await repo.find_corridor("RU", "CN", "CNY") == Corridor(
        "RU-CN-CNY", "RU", "CN", "CNY",
        D("50.00"), D("0.015"), D("25.00"), D("1000"), D("1000000"), D("100.00"), D("5000.00"),
    )


async def test_generated_rub_corridor_exists(repo):
    corridor = await repo.find_corridor("RU", "CN", "RUB")
    assert corridor is not None
    assert corridor.base_fee == D("500.00")
    assert corridor.max_limit == D("500000000")


async def test_csv_row_has_priority_over_generated(repo):
    corridor = await repo.find_corridor("CN", "RU", "RUB")
    assert corridor.base_fee == D("45.00")
    assert corridor.max_limit == D("800000")


async def test_unknown_corridor_is_none(repo):
    assert await repo.find_corridor("RU", "US", "EUR") is None


async def test_profiles_are_seeded(repo):
    profiles = {p.scenario: p for p in await repo.list_profiles()}
    assert set(profiles) == {"cbdc", "bank_transfer", "smart_contract", "trade_finance"}
    assert profiles["cbdc"].countries == ("CN", "AE", "IN", "BY", "KZ")
    assert profiles["cbdc"].max_amount == D("50000000")
    assert profiles["bank_transfer"].countries is None
    assert profiles["trade_finance"].min_amount == D("5000000")
    assert isinstance(profiles["smart_contract"].limitations, tuple)


async def test_ping(repo):
    assert await repo.ping() is True
