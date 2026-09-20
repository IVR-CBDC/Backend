"""One HTTP-level contract check per test, against the real compose stack
(service-auth + service-core + service-commission), not a mock of any of
them. Run via `make test-api` (brings the stack up with EMULATOR_MANUAL=true
first) — never invoked directly by CI in this plan (see the roadmap's
"Перенесено из плана 03" note: plan 04 wires this into CI)."""

from __future__ import annotations

import re
import time

import httpx
import pytest

from conftest import outbox_published_event

DISPLAY_ID_RE = re.compile(r"^DEAL-\d{4}-\d{4,}$")

# Mirrors services/service-core/src/emulator.cc's fnv1a/documentApproved
# exactly, attempt included: the emulator's verdict for attempt 0 is a
# deterministic function of (deal_id, kind); attempt >= 1 (a resubmission)
# always approves — that's the whole point of F2, the recovery flow this
# module's main test exercises. Since deal_id is a server-generated UUID we
# can't choose, tests instead predict the verdict client-side and draw deals
# until they get the combination they want to exercise.
_FNV_OFFSET = 14695981039346656037
_FNV_PRIME = 1099511628211
_MASK64 = (1 << 64) - 1


def _fnv1a(data: bytes) -> int:
    h = _FNV_OFFSET
    for byte in data:
        h ^= byte
        h = (h * _FNV_PRIME) & _MASK64
    return h


def _document_approved(deal_id: str, kind: str, attempt: int = 0) -> bool:
    if attempt >= 1:
        return True
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


def submit_document(core_url: str, token: str, deal_id: str, doc_id: str, version: int) -> httpx.Response:
    return httpx.post(
        f"{core_url}/api/core/deals/{deal_id}/documents/{doc_id}/submit",
        headers=auth_headers(token),
        json={"version": version},
        timeout=10.0,
    )


def tick_until(core_url: str, token: str, deal_id: str, predicate, *, timeout_sec: float = 60) -> dict:
    """Ticks the (EMULATOR_MANUAL) emulator, waiting out real EMULATOR_SPEED=demo
    delays, until `predicate(deal_json)` is true or `timeout_sec` elapses.
    Returns the last-fetched deal; callers assert on the predicate outcome
    themselves so a timeout produces an assertion with the stuck state."""
    deadline = time.monotonic() + timeout_sec
    deal = get_deal(core_url, token, deal_id).json()["deal"]
    while not predicate(deal) and time.monotonic() < deadline:
        tick_emulator(core_url)
        time.sleep(1)
        deal = get_deal(core_url, token, deal_id).json()["deal"]
    return deal


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


def test_rejected_document_recovers_via_resubmission_to_completed(core_url: str, company: dict):
    """The product's recovery story (F2/F3/F5): a document rejected on its
    first submission is not a dead end — resubmitting it always succeeds
    (documentApproved's attempt>=1 rule), and the deal proceeds all the way
    to completed. Draw a deal_id where "contract" is rejected on attempt 0
    and "invoice" is approved on attempt 0, so the test only has to recover
    from exactly one rejection and stays deterministic."""
    token = company["token"]
    for _ in range(50):
        deal = create_deal(core_url, token)
        if not _document_approved(deal["id"], "contract", 0) and _document_approved(deal["id"], "invoice", 0):
            break
    else:
        raise AssertionError("couldn't draw a deal_id with contract rejected / invoice approved on attempt 0")

    deal = choose_scenario(core_url, token, deal["id"], "cbdc", deal["version"]).json()["deal"]
    deal_id = deal["id"]
    contract_doc = next(d for d in deal["documents"] if d["kind"] == "contract")

    submit = submit_document(core_url, token, deal_id, contract_doc["id"], deal["version"])
    assert submit.status_code == 200, submit.text
    deal = submit.json()["deal"]

    # Two ticks, doc_review_sec apart: uploaded -> under_review -> rejected.
    deal = tick_until(core_url, token, deal_id, lambda d: d["stage"] == "blocked")
    assert deal["stage"] == "blocked"
    assert deal.get("attention_reason")
    contract_doc = next(d for d in deal["documents"] if d["kind"] == "contract")
    assert contract_doc["status"] == "rejected"
    assert contract_doc["reject_reason"]

    # Resubmit the same document — onDocumentSubmitted's blocked -> documents
    # path (F3's rearm branch), attempt is now 1.
    resubmit = submit_document(core_url, token, deal_id, contract_doc["id"], deal["version"])
    assert resubmit.status_code == 200, resubmit.text
    deal = resubmit.json()["deal"]
    assert deal["stage"] == "documents"

    deal = tick_until(
        core_url, token, deal_id, lambda d: next(doc for doc in d["documents"] if doc["kind"] == "contract")["status"] == "approved"
    )
    contract_doc = next(d for d in deal["documents"] if d["kind"] == "contract")
    assert contract_doc["status"] == "approved", "resubmission (attempt 1) must never be rejected again"
    # Invoice was never touched by the block/resubmit above — still on its
    # own first attempt (drawn to approve).
    assert deal["stage"] == "documents"

    invoice_doc = next(d for d in deal["documents"] if d["kind"] == "invoice")
    submit_invoice = submit_document(core_url, token, deal_id, invoice_doc["id"], deal["version"])
    assert submit_invoice.status_code == 200, submit_invoice.text

    deal = tick_until(core_url, token, deal_id, lambda d: d["stage"] == "completed", timeout_sec=90)
    assert deal["stage"] == "completed", f"deal stuck at stage={deal['stage']!r}"

    notifications = httpx.get(f"{core_url}/api/core/notifications", headers=auth_headers(token), timeout=10.0).json()
    items = notifications["items"]
    assert any(item["deal_id"] == deal_id and item["severity"] == "critical" for item in items), "the rejection notification"
    assert any(item["deal_id"] == deal_id and item["severity"] == "info" for item in items), "the completion notification"

    # F5: the deal.updated event that moved the deal to `blocked` reached the
    # transactional outbox and was actually published (picked up by
    # OutboxPublisher, PUBLISHed to Redis) — not just written to Postgres.
    assert outbox_published_event(deal_id, "deal.updated")


def test_notifications_are_isolated_by_company(core_url: str, company: dict, other_company: dict):
    # A rejected document is the cheapest deterministic way to get a
    # notification: draw a deal_id the emulator rejects on the first
    # required document, submit it, and tick until the critical
    # notification exists.
    token = company["token"]
    for _ in range(50):
        deal = create_deal(core_url, token)
        if not _document_approved(deal["id"], "contract", 0):
            break
    else:
        raise AssertionError("couldn't draw a deal_id the emulator rejects on the first attempt")

    deal = choose_scenario(core_url, token, deal["id"], "cbdc", deal["version"]).json()["deal"]
    deal_id = deal["id"]
    contract_doc = next(d for d in deal["documents"] if d["kind"] == "contract")
    submit_document(core_url, token, deal_id, contract_doc["id"], deal["version"])
    tick_until(core_url, token, deal_id, lambda d: d["stage"] == "blocked")

    own = httpx.get(f"{core_url}/api/core/notifications", headers=auth_headers(token), timeout=10.0).json()
    own_notification = next(item for item in own["items"] if item["deal_id"] == deal_id)

    other_resp = httpx.get(
        f"{core_url}/api/core/notifications", headers=auth_headers(other_company["token"]), timeout=10.0
    )
    assert other_resp.status_code == 200
    other_ids = {item["deal_id"] for item in other_resp.json()["items"]}
    assert deal_id not in other_ids

    read_by_other = httpx.post(
        f"{core_url}/api/core/notifications/{own_notification['id']}/read",
        headers=auth_headers(other_company["token"]),
        timeout=10.0,
    )
    assert read_by_other.status_code == 404
    assert read_by_other.json()["code"] == "NOT_FOUND"


def test_mark_notification_read_then_404_on_second_call(core_url: str, company: dict):
    token = company["token"]
    for _ in range(50):
        deal = create_deal(core_url, token)
        if not _document_approved(deal["id"], "contract", 0):
            break
    else:
        raise AssertionError("couldn't draw a deal_id the emulator rejects on the first attempt")

    deal = choose_scenario(core_url, token, deal["id"], "cbdc", deal["version"]).json()["deal"]
    deal_id = deal["id"]
    contract_doc = next(d for d in deal["documents"] if d["kind"] == "contract")
    submit_document(core_url, token, deal_id, contract_doc["id"], deal["version"])
    tick_until(core_url, token, deal_id, lambda d: d["stage"] == "blocked")

    items = httpx.get(f"{core_url}/api/core/notifications", headers=auth_headers(token), timeout=10.0).json()["items"]
    notification = next(item for item in items if item["deal_id"] == deal_id)
    assert notification["read"] is False

    first = httpx.post(f"{core_url}/api/core/notifications/{notification['id']}/read", headers=auth_headers(token), timeout=10.0)
    assert first.status_code == 204

    items = httpx.get(f"{core_url}/api/core/notifications", headers=auth_headers(token), timeout=10.0).json()["items"]
    updated = next(item for item in items if item["id"] == notification["id"])
    assert updated["read"] is True

    second = httpx.post(f"{core_url}/api/core/notifications/{notification['id']}/read", headers=auth_headers(token), timeout=10.0)
    assert second.status_code == 404
    assert second.json()["code"] == "NOT_FOUND"


def test_create_deal_amount_over_numeric_limit_returns_400(core_url: str, company: dict):
    # NUMERIC(18,2)'s magnitude limit: 16 integer digits (F6).
    resp = httpx.post(
        f"{core_url}/api/core/deals",
        headers=auth_headers(company["token"]),
        json={
            "counterparty_country": "CN",
            "counterparty_name": "Trading Partner Co",
            "operation_type": "import",
            "amount": 1e16,
            "currency": "CNY",
        },
        timeout=10.0,
    )
    assert resp.status_code == 400, resp.text
    assert resp.json()["code"] == "VALIDATION_ERROR"


def test_missing_token_returns_401_unauthorized(core_url: str):
    resp = httpx.get(f"{core_url}/api/core/deals", timeout=10.0)

    assert resp.status_code == 401
    assert resp.json()["code"] == "UNAUTHORIZED"


def test_garbage_token_returns_401_invalid_token(core_url: str):
    resp = httpx.get(f"{core_url}/api/core/deals", headers=auth_headers("not-a-real-jwt"), timeout=10.0)

    assert resp.status_code == 401
    assert resp.json()["code"] == "INVALID_TOKEN"


# Тот же дефект «ответ раньше COMMIT», что и в test_auth_commit.py, но на
# стороне сделок: DealRepository::create() возвращает DealDetail, пока его
# транзакция ещё жива, и 201 с телом сделки может обогнать её COMMIT.
# Следующий GET берёт из пула другое соединение и под READ COMMITTED видит
# 404 на только что «созданную» сделку. Ни пауз, ни ретраев здесь нет
# намеренно: окно измеряется миллисекундами, и любой sleep его спрячет.
CREATE_THEN_READ_ATTEMPTS = 15


@pytest.mark.parametrize("attempt", range(CREATE_THEN_READ_ATTEMPTS))
def test_created_deal_is_readable_immediately(core_url: str, company: dict, attempt: int):
    # Один keep-alive клиент на POST и GET — без него между ними встаёт
    # TCP-рукопожатие, которого хватает, чтобы COMMIT успел долететь, и
    # тест зеленеет на сломанном коде (проверено: с новым соединением на
    # каждый запрос 0 из 45, с keep-alive — 55 из 60 падений).
    with httpx.Client(
        base_url=core_url, timeout=10.0, headers=auth_headers(company["token"])
    ) as client:
        created = client.post(
            "/api/core/deals",
            json={
                "counterparty_country": "CN",
                "counterparty_name": "Trading Partner Co",
                "operation_type": "import",
                "amount": 100000,
                "currency": "CNY",
            },
        )
        assert created.status_code == 201, created.text
        deal = created.json()["deal"]

        # Никаких пауз и ретраев между запросами — в этом весь смысл теста.
        resp = client.get("/api/core/deals/{}".format(deal["id"]))

    assert resp.status_code == 200, (
        "GET сразу после создания получил {}: 201 ушёл раньше COMMIT "
        "транзакции create(). Тело: {}".format(resp.status_code, resp.text)
    )
    fetched = resp.json()["deal"]
    assert fetched["id"] == deal["id"]
    assert fetched["stage"] == "created"
    assert fetched["version"] == deal["version"]
