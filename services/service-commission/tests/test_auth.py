import pytest

from app.main import create_app
from tests.factories import FakeRepository

BODY = {"from_country": "RU", "to_country": "CN", "currency": "CNY", "amount": 100000}
URL = "/api/commission/quotes"


def test_valid_token_is_accepted(client, auth_headers):
    assert client.post(URL, json=BODY, headers=auth_headers).status_code == 200


def test_missing_token(client):
    response = client.post(URL, json=BODY)
    assert response.status_code == 401
    assert response.json() == {"code": "UNAUTHORIZED", "error": "Отсутствует токен авторизации"}


def test_expired_token(client, make_token):
    response = client.post(URL, json=BODY, headers={"Authorization": f"Bearer {make_token(ttl=-120)}"})
    assert response.status_code == 401
    assert response.json()["code"] == "TOKEN_EXPIRED"


def test_token_expired_within_leeway_is_still_accepted(client, make_token):
    response = client.post(URL, json=BODY, headers={"Authorization": f"Bearer {make_token(ttl=-10)}"})
    assert response.status_code == 200


def test_lowercase_bearer_scheme_is_accepted(client, make_token):
    response = client.post(URL, json=BODY, headers={"Authorization": f"bearer {make_token()}"})
    assert response.status_code == 200


@pytest.mark.parametrize(
    "token_kwargs",
    [{"audience": "public"}, {"issuer": "someone-else"}, {"sub": None}],
    ids=["wrong-audience", "wrong-issuer", "no-sub"],
)
def test_invalid_claims(client, make_token, token_kwargs):
    response = client.post(URL, json=BODY, headers={"Authorization": f"Bearer {make_token(**token_kwargs)}"})
    assert response.status_code == 401
    assert response.json()["code"] == "INVALID_TOKEN"


def test_token_signed_by_foreign_key(client, make_token, foreign_private_key):
    token = make_token(key=foreign_private_key)
    response = client.post(URL, json=BODY, headers={"Authorization": f"Bearer {token}"})
    assert response.status_code == 401
    assert response.json()["code"] == "INVALID_TOKEN"


def test_health_does_not_require_token(client):
    assert client.get("/health").status_code == 200


def test_broken_public_key_fails_at_construction():
    with pytest.raises(ValueError):
        create_app(FakeRepository(), b"garbage")
