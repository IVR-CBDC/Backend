"""Fixtures for API tests running against the docker-compose stack.

`make test-api` brings the stack up with EMULATOR_MANUAL=true (deterministic:
the emulator only moves when a test calls POST /internal/emulator/tick) and
then runs pytest here. Nothing in this file starts or stops the stack.
"""

from __future__ import annotations

import os
import random
import subprocess
import time
import uuid

import httpx
import pytest

DEFAULT_AUTH_URL = "http://127.0.0.1:18080"
DEFAULT_CORE_URL = "http://127.0.0.1:18081"
READY_TIMEOUT_SEC = 60

# pg-core's host-published port (implementer-rules.md). Used instead of
# subscribing to Redis pub/sub from pytest for the outbox->Redis assertion
# (F5): the `redis` container's 6379 isn't published to the host, only
# reachable on ivr_backend-net, which this host-side pytest process isn't
# attached to — the brief's own fallback for exactly this case.
PG_CORE_DSN = os.environ.get("PG_CORE_DSN", "postgresql://core:core@127.0.0.1:5435/core")


def outbox_published_event(deal_id: str, event_type: str) -> bool:
    """True once an outbox row for `deal_id`/`event_type` exists and has been
    published (published_at IS NOT NULL) — proof the row was both written by
    DealRepository::apply() and picked up by OutboxPublisher and PUBLISHed to
    Redis (publishOnce() only sets published_at after a successful PUBLISH)."""
    query = (
        "SELECT count(*) FROM outbox WHERE payload->>'deal_id' = '{}' "
        "AND payload->>'type' = '{}' AND published_at IS NOT NULL".format(deal_id, event_type)
    )
    result = subprocess.run(["psql", PG_CORE_DSN, "-tAc", query], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    return int(result.stdout.strip() or "0") > 0


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
                # 200 alone isn't enough: /health can answer 200 with
                # ok=false (e.g. redis_ok=false — see F1) when a dependency
                # is misconfigured, and running the suite against a
                # half-broken stand should fail loudly here, not with a
                # confusing pile of per-test connection errors.
                if resp.status_code == 200 and resp.json().get("ok") is True:
                    del pending[name]
            except httpx.HTTPError:
                pass
        if pending:
            time.sleep(0.5)

    if pending:
        pytest.fail(
            "Стенд не поднялся за {}s: не отвечают {} (подними стенд командой `make test-api` — "
            "она включает docker-compose.dev.yml с проброшенными портами; либо проверь "
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
