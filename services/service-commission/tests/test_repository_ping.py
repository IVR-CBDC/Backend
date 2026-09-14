import logging

from app.repository import PgCorridorRepository


class _BrokenEngine:
    def connect(self):
        raise OSError("boom")


async def test_ping_logs_warning_on_failure(caplog):
    repo = PgCorridorRepository(_BrokenEngine())
    with caplog.at_level(logging.WARNING):
        assert await repo.ping() is False
    assert any(
        record.levelno == logging.WARNING and "postgres ping failed" in record.message
        for record in caplog.records
    )
