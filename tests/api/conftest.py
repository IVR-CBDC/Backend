"""Fixtures for API tests running against the docker-compose stack.

`make test-api` brings the stack up with EMULATOR_MANUAL=true (deterministic:
the emulator only moves when a test calls POST /internal/emulator/tick) and
then runs pytest here. Nothing in this file starts or stops the stack.
"""

from __future__ import annotations

import os
import random
import time
import uuid

import httpx
import pytest

DEFAULT_AUTH_URL = "http://127.0.0.1:18080"
DEFAULT_CORE_URL = "http://127.0.0.1:18081"
READY_TIMEOUT_SEC = 60


@pytest.fixture(scope="session")
def auth_url() -> str:
    return os.environ.get("AUTH_URL", DEFAULT_AUTH_URL)


@pytest.fixture(scope="session")
def core_url() -> str:
    return os.environ.get("CORE_URL", DEFAULT_CORE_URL)


@pytest.fixture(scope="session", autouse=True)
def stack_ready(auth_url: str, core_url: str) -> None:
    """Polls /health on both services until they answer or the timeout
    expires. Failing fast with a clear message here is the whole point —
    without it, every test in the file would instead fail one-by-one with a
    generic connection-refused error."""
    deadline = time.monotonic() + READY_TIMEOUT_SEC
    services = {"service-auth": f"{auth_url}/health", "service-core": f"{core_url}/health"}
    pending = dict(services)

    while pending and time.monotonic() < deadline:
        for name, url in list(pending.items()):
            try:
                resp = httpx.get(url, timeout=2.0)
                if resp.status_code == 200:
                    del pending[name]
            except httpx.HTTPError:
                pass
        if pending:
            time.sleep(0.5)

    if pending:
        pytest.fail(
            "Стенд не поднялся за {}s: не отвечают {} (проверьте `docker compose up`, "
            "AUTH_URL/CORE_URL или `docker compose logs`)".format(READY_TIMEOUT_SEC, ", ".join(pending))
        )


def _random_inn() -> str:
    return "".join(str(random.randint(0, 9)) for _ in range(10))


def _register_company(auth_url: str) -> dict:
    login = f"user-{uuid.uuid4().hex[:12]}"
    resp = httpx.post(
        f"{auth_url}/api/auth/register",
        json={
            "login": login,
            "password": "hunter22",
            "name": "Тестовый пользователь",
            "company_name": f"ООО Тест {uuid.uuid4().hex[:8]}",
            "inn": _random_inn(),
        },
        timeout=10.0,
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    return {"token": body["token"], "company_id": body["company_id"]}


@pytest.fixture()
def company(auth_url: str) -> dict:
    return _register_company(auth_url)


@pytest.fixture()
def other_company(auth_url: str) -> dict:
    return _register_company(auth_url)
