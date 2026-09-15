from fastapi.testclient import TestClient

from app.main import create_app
from tests.factories import FakeRepository


def transfer(**overrides):
    body = {"from_country": "RU", "to_country": "CN", "currency": "CNY", "amount": 100000}
    body.update(overrides)
    return body


class TestCalculate:
    URL = "/api/commission/calculate"

    def test_response_is_compatible_with_ivrpy(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(), headers=auth_headers)
        assert response.status_code == 200
        assert response.json() == {
            "corridor_id": "RU-CN-CNY",
            "from_country": "RU",
            "to_country": "CN",
            "currency": "CNY",
            "amount": 100000.0,
            "base_fee": 50.0,
            "percentage_fee": 0.015,
            "percentage_amount": 1500.0,
            "fixed_fee": 25.0,
            "total_commission": 1575.0,
            "min_fee": 100.0,
            "max_fee": 5000.0,
        }

    def test_codes_are_case_insensitive(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(from_country="ru", to_country="cn", currency="cny"), headers=auth_headers)
        assert response.status_code == 200
        assert response.json()["corridor_id"] == "RU-CN-CNY"

    def test_same_country(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(to_country="RU"), headers=auth_headers)
        assert response.status_code == 400
        assert response.json() == {"code": "SAME_COUNTRY", "error": "Страны отправителя и получателя совпадают"}

    def test_invalid_amount(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(amount=0), headers=auth_headers)
        assert response.status_code == 400
        assert response.json()["code"] == "INVALID_AMOUNT"

    def test_corridor_not_found(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(to_country="US", currency="USD"), headers=auth_headers)
        assert response.status_code == 404
        assert response.json() == {"code": "CORRIDOR_NOT_FOUND", "error": "Коридор RU→US в USD не поддерживается"}

    def test_amount_below_limit(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(amount=500), headers=auth_headers)
        assert response.status_code == 400
        assert response.json()["code"] == "AMOUNT_BELOW_LIMIT"

    def test_amount_above_limit(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(amount=2_000_000), headers=auth_headers)
        assert response.status_code == 400
        assert response.json()["code"] == "AMOUNT_ABOVE_LIMIT"

    def test_validation_error_format(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(from_country="RUS"), headers=auth_headers)
        assert response.status_code == 422
        body = response.json()
        assert body["code"] == "VALIDATION_ERROR"
        assert body["error"] == "Некорректные параметры запроса"
        assert body["details"][0]["loc"] == ["body", "from_country"]


class TestQuotes:
    URL = "/api/commission/quotes"

    def test_returns_four_quotes_with_commission(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(), headers=auth_headers)
        assert response.status_code == 200
        body = response.json()
        assert body["corridor_id"] == "RU-CN-CNY"
        assert [q["scenario"] for q in body["quotes"]] == ["cbdc", "bank_transfer", "smart_contract", "trade_finance"]

        cbdc = body["quotes"][0]
        assert cbdc == {
            "scenario": "cbdc",
            "title": "Расчёт через ЦВЦБ",
            "description": "Перевод цифрового рубля напрямую между кошельками сторон, минуя корреспондентские счета.",
            "eta_label": "5–20 минут",
            "limitations": ["Доступно только для стран-участниц пилота ЦВЦБ: CN, AE, IN, BY, KZ", "Лимит 50 000 000 на сделку"],
            "available": True,
            "unavailable_reason": None,
            "commission": {
                "base_fee": 50.0,
                "percentage_fee": 0.015,
                "percentage_amount": 1500.0,
                "fixed_fee": 25.0,
                "subtotal": 1575.0,
                "clamped": None,
                "multiplier": 0.4,
                "total": 630.0,
            },
        }

        trade_finance = body["quotes"][3]
        assert trade_finance["available"] is False
        assert trade_finance["commission"] is None
        assert trade_finance["unavailable_reason"] == "Минимальная сумма для сценария: 5 000 000 CNY"

    def test_unknown_corridor_is_not_an_error(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(to_country="US", currency="USD"), headers=auth_headers)
        assert response.status_code == 200
        body = response.json()
        assert body["corridor_id"] is None
        assert all(q["available"] is False for q in body["quotes"])

    def test_same_country_is_still_an_error(self, client, auth_headers):
        response = client.post(self.URL, json=transfer(to_country="RU"), headers=auth_headers)
        assert response.status_code == 400
        assert response.json()["code"] == "SAME_COUNTRY"


class TestHealth:
    def test_healthy(self, client):
        response = client.get("/health")
        assert response.status_code == 200
        assert response.json() == {"ok": True, "service": "service-commission", "version": "1.0.0", "postgres_ok": True}

    def test_database_down_returns_503(self, rsa_keys):
        client = TestClient(create_app(FakeRepository(healthy=False), rsa_keys[1]))
        response = client.get("/health")
        assert response.status_code == 503
        assert response.json()["postgres_ok"] is False

    def test_health_is_hidden_from_openapi(self, client):
        paths = client.get("/openapi.json").json()["paths"]
        assert "/health" not in paths
        assert "/api/commission/quotes" in paths
