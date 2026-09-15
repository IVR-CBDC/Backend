from fastapi.testclient import TestClient

from app.domain import Corridor, ScenarioProfile
from app.main import create_app
from tests.factories import CORRIDORS, PROFILES


class RaisingRepository:
    """FakeRepository variant whose find_corridor raises a given exception."""

    def __init__(self, exc: Exception) -> None:
        self._exc = exc

    async def find_corridor(self, from_country: str, to_country: str, currency: str) -> Corridor | None:
        raise self._exc

    async def list_profiles(self) -> list[ScenarioProfile]:
        return list(PROFILES)

    async def ping(self) -> bool:
        return True


def transfer(**overrides):
    body = {"from_country": "RU", "to_country": "CN", "currency": "CNY", "amount": 100000}
    body.update(overrides)
    return body


class TestUnknownPath:
    def test_unknown_path_is_404_in_uniform_format(self, client):
        response = client.get("/does-not-exist")
        assert response.status_code == 404
        assert response.json() == {"code": "NOT_FOUND", "error": "Ресурс не найден"}

    def test_wrong_method_is_405_in_uniform_format(self, client):
        response = client.get("/api/commission/calculate")
        assert response.status_code == 405
        assert response.json() == {"code": "METHOD_NOT_ALLOWED", "error": "Метод не поддерживается"}


class TestDbUnavailable:
    def test_oserror_from_repository_is_503_db_unavailable(self, rsa_keys, auth_headers):
        client = TestClient(create_app(RaisingRepository(OSError("connection refused")), rsa_keys[1]))
        response = client.post("/api/commission/calculate", json=transfer(), headers=auth_headers)
        assert response.status_code == 503
        assert response.json() == {"code": "DB_UNAVAILABLE", "error": "База данных недоступна"}

    def test_timeouterror_from_repository_is_503_db_unavailable(self, rsa_keys, auth_headers):
        client = TestClient(create_app(RaisingRepository(TimeoutError("timed out")), rsa_keys[1]))
        response = client.post("/api/commission/calculate", json=transfer(), headers=auth_headers)
        assert response.status_code == 503
        assert response.json() == {"code": "DB_UNAVAILABLE", "error": "База данных недоступна"}


class TestUnexpectedError:
    def test_unhandled_exception_is_500_internal_error(self, rsa_keys, auth_headers):
        client = TestClient(
            create_app(RaisingRepository(RuntimeError("boom")), rsa_keys[1]),
            raise_server_exceptions=False,
        )
        response = client.post("/api/commission/calculate", json=transfer(), headers=auth_headers)
        assert response.status_code == 500
        assert response.json() == {"code": "INTERNAL_ERROR", "error": "Внутренняя ошибка сервиса"}
