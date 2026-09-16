"""One HTTP-level contract check per test, against the real compose stack
(service-auth + service-core + service-commission), not a mock of any of
them. Run via `make test-api` (brings the stack up with EMULATOR_MANUAL=true
first) — never invoked directly by CI in this plan (see the roadmap's
"Перенесено из плана 03" note: plan 04 wires this into CI)."""

from __future__ import annotations

import re
import time

import httpx

DISPLAY_ID_RE = re.compile(r"^DEAL-\d{4}-\d{4,}$")

# Mirrors services/service-core/src/emulator.cc's fnv1a/documentApproved
# exactly: the emulator's document verdict is a deterministic function of
# (deal_id, kind) with no retry — a document that hashes to "rejected" for a
# given deal_id stays rejected forever for that deal. Since deal_id is a
# server-generated UUID we can't choose, the completion test instead predicts
# the verdict client-side and keeps creating deals until it draws one where
# every required document is going to be approved.
_FNV_OFFSET = 14695981039346656037
_FNV_PRIME = 1099511628211
_MASK64 = (1 << 64) - 1


def _fnv1a(data: bytes) -> int:
    h = _FNV_OFFSET
    for byte in data:
        h ^= byte
        h = (h * _FNV_PRIME) & _MASK64
    return h


def _document_approved(deal_id: str, kind: str) -> bool:
    return _fnv1a(f"{deal_id}:{kind}".encode()) % 10 != 0


def auth_headers(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


def create_deal(core_url: str, token: str, *, country: str = "CN", amount: float = 100000, currency: str = "CNY") -> dict:
    resp = httpx.post(
        f"{core_url}/api/core/deals",
        headers=auth_headers(token),
        json={
            "counterparty_country": country,
            "counterparty_name": "Trading Partner Co",
            "operation_type": "import",
            "amount": amount,
            "currency": currency,
        },
        timeout=10.0,
    )
    assert resp.status_code == 201, resp.text
    return resp.json()["deal"]


def choose_scenario(core_url: str, token: str, deal_id: str, scenario: str, version: int) -> httpx.Response:
    return httpx.post(
        f"{core_url}/api/core/deals/{deal_id}/scenario",
        headers=auth_headers(token),
        json={"scenario": scenario, "version": version},
        timeout=10.0,
    )


def tick_emulator(core_url: str) -> int:
    resp = httpx.post(f"{core_url}/internal/emulator/tick", timeout=10.0)
    assert resp.status_code == 200, resp.text
    return resp.json()["processed"]


def get_deal(core_url: str, token: str, deal_id: str) -> httpx.Response:
    return httpx.get(f"{core_url}/api/core/deals/{deal_id}", headers=auth_headers(token), timeout=10.0)


def test_create_deal_returns_201_with_initial_state(core_url: str, company: dict):
    deal = create_deal(core_url, company["token"])

    assert deal["stage"] == "created"
    assert deal["version"] == 0
    assert deal["display_id"]
    assert DISPLAY_ID_RE.match(deal["display_id"]), deal["display_id"]


def test_list_deals_is_isolated_by_company(core_url: str, company: dict, other_company: dict):
    deal = create_deal(core_url, company["token"])

    own_resp = httpx.get(f"{core_url}/api/core/deals", headers=auth_headers(company["token"]), timeout=10.0)
    assert own_resp.status_code == 200
    own_ids = {item["id"] for item in own_resp.json()["items"]}
    assert deal["id"] in own_ids

    other_resp = httpx.get(f"{core_url}/api/core/deals", headers=auth_headers(other_company["token"]), timeout=10.0)
    assert other_resp.status_code == 200
    other_ids = {item["id"] for item in other_resp.json()["items"]}
    assert deal["id"] not in other_ids


def test_get_foreign_deal_returns_404(core_url: str, company: dict, other_company: dict):
    deal = create_deal(core_url, company["token"])

    resp = get_deal(core_url, other_company["token"], deal["id"])

    assert resp.status_code == 404
    assert resp.json()["code"] == "NOT_FOUND"


def test_choose_scenario_reprices_and_creates_base_documents(core_url: str, company: dict):
    deal = create_deal(core_url, company["token"])

    resp = choose_scenario(core_url, company["token"], deal["id"], "cbdc", deal["version"])

    assert resp.status_code == 200, resp.text
    updated = resp.json()["deal"]
    assert updated["commission_total"] > 0
    assert updated["stage"] == "documents"

    doc_status = {doc["kind"]: doc["status"] for doc in updated["documents"]}
    assert doc_status["contract"] == "missing"
    assert doc_status["invoice"] == "missing"


def test_choose_scenario_twice_with_same_version_conflicts(core_url: str, company: dict):
    deal = create_deal(core_url, company["token"])
    first = choose_scenario(core_url, company["token"], deal["id"], "cbdc", deal["version"])
    assert first.status_code == 200, first.text

    retry = choose_scenario(core_url, company["token"], deal["id"], "cbdc", deal["version"])

    assert retry.status_code == 409
    assert retry.json()["code"] == "VERSION_CONFLICT"


def test_scenario_unavailable_for_small_trade_finance_amount(core_url: str, company: dict):
    # trade_finance requires amount >= 5,000,000 (service-commission seed
    # data, migrations/003_seed.sql) — this deal's amount is far below that.
    deal = create_deal(core_url, company["token"], amount=1000)

    resp = choose_scenario(core_url, company["token"], deal["id"], "trade_finance", deal["version"])

    assert resp.status_code == 409
    assert resp.json()["code"] == "SCENARIO_UNAVAILABLE"


def test_submit_document_and_emulator_ticks_complete_the_deal(core_url: str, company: dict):
    # cbdc only requires contract + invoice (see spec §4.2) — draw deals
    # until we get one the emulator will approve both of deterministically.
    for _ in range(50):
        deal = create_deal(core_url, company["token"])
        if _document_approved(deal["id"], "contract") and _document_approved(deal["id"], "invoice"):
            break
    else:
        raise AssertionError("couldn't draw a deal_id the emulator approves both documents for")

    scenario_resp = choose_scenario(core_url, company["token"], deal["id"], "cbdc", deal["version"])
    deal = scenario_resp.json()["deal"]

    for doc in deal["documents"]:
        submit = httpx.post(
            f"{core_url}/api/core/deals/{deal['id']}/documents/{doc['id']}/submit",
            headers=auth_headers(company["token"]),
            json={"version": deal["version"]},
            timeout=10.0,
        )
        assert submit.status_code == 200, submit.text
        deal = submit.json()["deal"]

    submitted_doc = next(d for d in deal["documents"] if d["kind"] == "contract")
    assert submitted_doc["status"] == "uploaded"

    # EMULATOR_MANUAL=true: nothing advances on its own. Tick repeatedly,
    # waiting out EMULATOR_SPEED=demo's real delays (next_action_at is a
    # real timestamp, so a tick before it elapses is a no-op) until the
    # deal reaches its terminal stage or we give up.
    deadline = time.monotonic() + 60
    stage = deal["stage"]
    while stage != "completed" and time.monotonic() < deadline:
        tick_emulator(core_url)
        time.sleep(1)
        stage = get_deal(core_url, company["token"], deal["id"]).json()["deal"]["stage"]

    assert stage == "completed", f"deal stuck at stage={stage!r}"

    notifications = httpx.get(
        f"{core_url}/api/core/notifications", headers=auth_headers(company["token"]), timeout=10.0
    )
    assert notifications.status_code == 200
    items = notifications.json()["items"]
    assert any(item["deal_id"] == deal["id"] for item in items)


def test_missing_token_returns_401_unauthorized(core_url: str):
    resp = httpx.get(f"{core_url}/api/core/deals", timeout=10.0)

    assert resp.status_code == 401
    assert resp.json()["code"] == "UNAUTHORIZED"


def test_garbage_token_returns_401_invalid_token(core_url: str):
    resp = httpx.get(f"{core_url}/api/core/deals", headers=auth_headers("not-a-real-jwt"), timeout=10.0)

    assert resp.status_code == 401
    assert resp.json()["code"] == "INVALID_TOKEN"
