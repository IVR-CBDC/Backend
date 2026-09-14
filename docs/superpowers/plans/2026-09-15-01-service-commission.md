# service-commission Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Перенести `ivrpy` в монорепо `IVR` как `services/service-commission`: Postgres вместо CSV, котировки по 4 сценариям расчёта, JWT-авторизация, SQL-миграции (compose и helm из одного источника), pytest. Пустой `service-test-python` удаляется.

**Architecture:** Чистый доменный модуль (`app/domain.py`, Decimal, без I/O) + репозиторий на SQLAlchemy async + FastAPI-роутер, собираемый фабрикой `create_app(repo, jwt_public_key)` — это позволяет тестировать API с фейковым репозиторием и сгенерированным RSA-ключом. Миграции — plain SQL, применяются существующим `infra/migrate.sh` (compose) и helm init-контейнером (values генерируются скриптом).

**Tech Stack:** Python 3.12, FastAPI, Pydantic v2, SQLAlchemy 2 async + asyncpg, PyJWT[crypto], pytest (+pytest-asyncio, httpx), uv, Postgres 16, Docker Compose, Helm.

**Spec:** `docs/superpowers/specs/2026-09-15-cbdc-hub-integration-design.md` (§2.3, §5, §8 миграции в helm, §9 unit Python)

## Global Constraints

- Все команды — из корня репо `/home/legors/Documents/IVR`, ветка `feat/cbdc-hub-integration`.
- Python: `requires-python = ">=3.12"`, образ `python:3.12-slim`, локально `uv` (есть `uv 0.11.x`, cpython 3.12 установлен).
- Деньги — только `decimal.Decimal`, округление `ROUND_HALF_UP` до `0.01`. В JSON-ответах — числа (float), не строки.
- Формат ошибок сервиса: `{"code": "<UPPER_SNAKE>", "error": "<сообщение по-русски>"}`; валидация: `422 {"code": "VALIDATION_ERROR", "error": "Некорректные параметры запроса", "details": [...]}`.
- Коды ошибок ivrpy сохраняются: `SAME_COUNTRY`, `INVALID_AMOUNT`, `CORRIDOR_NOT_FOUND` (404), `AMOUNT_BELOW_LIMIT`, `AMOUNT_ABOVE_LIMIT` (400).
- JWT: `RS256`, `iss = "service-auth"`, `aud = "internal"`, обязательные `sub`, `exp`. Публичный ключ — PEM по пути `JWT_PUBLIC_KEY_PATH`.
- Порядок сценариев везде: `cbdc`, `bank_transfer`, `smart_contract`, `trade_finance`.
- Домашняя страна клиента — `RU`.
- Сервис **не** публикуется через Traefik (внутренний, спека §3).
- Порт сервиса `8000`, health — `GET /health` (без JWT, не входит в OpenAPI).
- Каждый коммит заканчивается строками:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01UJR72d6wzQrg47iW3EQuXH
  ```

## File Structure

```
services/service-commission/
├── pyproject.toml                 зависимости, pytest-конфиг
├── uv.lock                        фиксированные версии (генерирует uv)
├── Dockerfile                     сборка из корня репо
├── .gitignore                     (из ivrpy)
├── README.md                      (переписан под сервис)
├── app/
│   ├── __init__.py
│   ├── domain.py                  сущности + расчёт + котировки (чистые функции)
│   ├── errors.py                  ApiError + обработчики ошибок FastAPI
│   ├── auth.py                    make_require_user(public_key)
│   ├── repository.py              CorridorRepository (Protocol) + PgCorridorRepository
│   ├── schemas.py                 Pydantic-запрос + сериализация ответов
│   ├── api.py                     create_router(repo, require_user)
│   ├── config.py                  Settings.from_env()
│   └── main.py                    create_app(...) / build_default_app()
├── migrations/
│   ├── 001_corridors.sql
│   ├── 002_scenario_profiles.sql
│   └── 003_seed.sql
└── tests/
    ├── __init__.py
    ├── factories.py               CORRIDORS, PROFILES, FakeRepository
    ├── conftest.py                RSA-ключи, make_token, client
    ├── test_domain.py
    ├── test_auth.py
    ├── test_api.py
    └── test_repository_pg.py      (@db, против реального Postgres)

infra/gen-migration-values.sh      services/<svc>/migrations/*.sql → infra/helm/generated/migrations-<svc>.yaml
infra/helm/values-service-commission.yaml
Удаляются: services/service-test-python/, infra/helm/values-service-test-python.yaml
Меняются: docker-compose.yml, infra/traefik/dynamic.yml, Makefile, README.md, .gitignore,
          infra/gen-openapi.sh, docs/openapi.yml (перегенерация), docs/new-service-*.md,
          infra/helm/values-service-{auth,core}.yaml, .github/workflows/deploy.yml
```

---

### Task 1: Импорт истории ivrpy через git subtree

**Files:**
- Create (subtree): `services/service-commission/**` (содержимое `IVR-CBDC/commission-service`)
- Delete: `services/service-commission/.github/`, `services/service-commission/.DS_Store`, `services/service-commission/docker-compose.yml`

**Interfaces:**
- Consumes: локальный клон `/home/legors/Documents/ivrpy`, ветка `main` (рабочее дерево чистое).
- Produces: каталог `services/service-commission/` с историей коммитов ivrpy.

- [ ] **Step 1: Убедиться, что рабочие деревья чистые**

Run: `git status --short && git -C /home/legors/Documents/ivrpy status --short`
Expected: пустой вывод в обоих. Если не пусто — остановиться и сообщить.

- [ ] **Step 2: Добавить subtree**

Run:
```bash
git subtree add --prefix=services/service-commission /home/legors/Documents/ivrpy main \
  -m "chore(commission): import commission-service history via git subtree

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UJR72d6wzQrg47iW3EQuXH"
```
Expected: `Added dir 'services/service-commission'`.

- [ ] **Step 3: Проверить, что история перенесена**

Run: `git log --oneline -- services/service-commission | head -5`
Expected: среди строк есть коммиты ivrpy (`the end`, `ci: fix workflow trigger`).

- [ ] **Step 4: Удалить артефакты, не нужные в монорепо**

```bash
git rm -r -q services/service-commission/.github services/service-commission/docker-compose.yml
git rm -q services/service-commission/.DS_Store
```

- [ ] **Step 5: Commit**

```bash
git commit -q -m "chore(commission): drop standalone CI and compose from imported service

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UJR72d6wzQrg47iW3EQuXH"
```

---

### Task 2: Доменная логика на Decimal + котировки по сценариям

**Files:**
- Create: `services/service-commission/pyproject.toml`
- Create: `services/service-commission/app/__init__.py` (пустой)
- Create: `services/service-commission/app/domain.py`
- Create: `services/service-commission/tests/__init__.py` (пустой)
- Create: `services/service-commission/tests/factories.py`
- Create: `services/service-commission/tests/test_domain.py`
- Delete: `services/service-commission/{calculator.py,main.py,test_calculator.py,requirements.txt}`
- Generated: `services/service-commission/uv.lock`

**Interfaces:**
- Consumes: ничего.
- Produces (`app.domain`):
  - `HOME_COUNTRY: str = "RU"`, `SCENARIO_ORDER: tuple[str, ...]`
  - `money(value: Decimal) -> Decimal`, `format_amount(value: Decimal) -> str`
  - `class DomainError(Exception)` с атрибутами `code: str`, `message: str`
  - `@dataclass(frozen=True) Corridor(corridor_id, from_country, to_country, currency, base_fee, percentage_fee, fixed_fee, min_limit, max_limit, min_fee, max_fee)` — все суммы `Decimal`
  - `@dataclass(frozen=True) ScenarioProfile(scenario, title, description, fee_multiplier: Decimal, eta_label, limitations: tuple[str, ...], min_amount: Decimal | None = None, max_amount: Decimal | None = None, countries: tuple[str, ...] | None = None)`
  - `@dataclass(frozen=True) Commission(base_fee, percentage_fee, percentage_amount, fixed_fee, multiplier, total)` — `Decimal`
  - `@dataclass(frozen=True) Quote(profile: ScenarioProfile, available: bool, unavailable_reason: str | None, commission: Commission | None)`
  - `validate_transfer(from_country: str, to_country: str, amount: Decimal) -> None` (raises `DomainError`)
  - `corridor_limit_error(corridor: Corridor, amount: Decimal) -> DomainError | None`
  - `calculate_commission(corridor: Corridor, amount: Decimal, multiplier: Decimal = Decimal("1")) -> Commission`
  - `counterparty_country(from_country: str, to_country: str) -> str`
  - `build_quotes(corridor: Corridor | None, profiles: list[ScenarioProfile], from_country: str, to_country: str, currency: str, amount: Decimal) -> list[Quote]`
- Produces (`tests.factories`): `CORRIDORS: list[Corridor]`, `PROFILES: list[ScenarioProfile]`, `corridor(corridor_id: str) -> Corridor`, `profile(scenario: str) -> ScenarioProfile` (FakeRepository добавляется в Task 4).

- [ ] **Step 1: Создать `pyproject.toml`**

```toml
[project]
name = "service-commission"
version = "1.0.0"
description = "Расчёт комиссии трансграничных платежей и котировки сценариев расчёта"
requires-python = ">=3.12"
dependencies = [
    "fastapi>=0.128",
    "uvicorn[standard]>=0.39",
    "pydantic>=2.13",
    "sqlalchemy[asyncio]>=2.0.44",
    "asyncpg>=0.31",
    "pyjwt[crypto]>=2.10",
]

[project.optional-dependencies]
dev = [
    "pytest>=8.4",
    "pytest-asyncio>=1.2",
    "httpx>=0.28",
]

[build-system]
requires = ["setuptools>=75"]
build-backend = "setuptools.build_meta"

[tool.setuptools.packages.find]
include = ["app*"]

[tool.pytest.ini_options]
testpaths = ["tests"]
pythonpath = ["."]
asyncio_mode = "auto"
markers = ["db: требует TEST_PG_DSN с применёнными миграциями"]
```

- [ ] **Step 2: Удалить старый код ivrpy и поставить зависимости**

```bash
git rm -q services/service-commission/calculator.py services/service-commission/main.py \
          services/service-commission/test_calculator.py services/service-commission/requirements.txt
mkdir -p services/service-commission/app services/service-commission/tests
touch services/service-commission/app/__init__.py services/service-commission/tests/__init__.py
cd services/service-commission && uv sync --python 3.12 --extra dev
```
Expected: создан `.venv` и `uv.lock`, без ошибок. `.venv/` уже игнорируется корневым `.gitignore` (`**/.venv/`).

- [ ] **Step 3: Создать `tests/factories.py`**

```python
"""Тестовые данные. Значения совпадают с migrations/003_seed.sql."""

from decimal import Decimal as D

from app.domain import Corridor, ScenarioProfile

CORRIDORS: list[Corridor] = [
    Corridor("RU-CN-CNY", "RU", "CN", "CNY", D("50.00"), D("0.015"), D("25.00"), D("1000"), D("1000000"), D("100.00"), D("5000.00")),
    Corridor("RU-TR-TRY", "RU", "TR", "TRY", D("30.00"), D("0.016"), D("12.00"), D("500"), D("400000"), D("75.00"), D("2500.00")),
    Corridor("RU-CN-RUB", "RU", "CN", "RUB", D("500.00"), D("0.015"), D("250.00"), D("10000"), D("500000000"), D("1000.00"), D("500000.00")),
    Corridor("CN-RU-RUB", "CN", "RU", "RUB", D("45.00"), D("0.014"), D("20.00"), D("1000"), D("800000"), D("95.00"), D("4500.00")),
]

PROFILES: list[ScenarioProfile] = [
    ScenarioProfile(
        scenario="cbdc",
        title="Расчёт через ЦВЦБ",
        description="Перевод цифрового рубля напрямую между кошельками сторон, минуя корреспондентские счета.",
        fee_multiplier=D("0.400"),
        eta_label="5–20 минут",
        limitations=("Доступно только для стран-участниц пилота ЦВЦБ: CN, AE, IN, BY, KZ", "Лимит 50 000 000 на сделку"),
        max_amount=D("50000000"),
        countries=("CN", "AE", "IN", "BY", "KZ"),
    ),
    ScenarioProfile(
        scenario="bank_transfer",
        title="Классический банковский перевод",
        description="SWIFT/корреспондентский перевод через сеть банков-партнёров.",
        fee_multiplier=D("1.000"),
        eta_label="1–3 рабочих дня",
        limitations=("Требует полного комплекта валютных документов", "Возможны задержки на стороне банка контрагента"),
    ),
    ScenarioProfile(
        scenario="smart_contract",
        title="Смарт-контракт",
        description="Условное депонирование в блокчейне: средства списываются при подтверждении поставки.",
        fee_multiplier=D("0.700"),
        eta_label="до 24 часов после подтверждения условий",
        limitations=("Нужна цифровая подпись обеих сторон", "Не поддерживает частичные поставки", "Доступно для CN, AE, TR"),
        countries=("CN", "AE", "TR"),
    ),
    ScenarioProfile(
        scenario="trade_finance",
        title="Торговое финансирование",
        description="Аккредитив или банковская гарантия для сделок с длинным циклом поставки.",
        fee_multiplier=D("1.800"),
        eta_label="3–10 рабочих дней на оформление",
        limitations=("Требуется кредитный лимит в банке", "Подходит для сумм от 5 000 000"),
        min_amount=D("5000000"),
    ),
]


def corridor(corridor_id: str) -> Corridor:
    return next(c for c in CORRIDORS if c.corridor_id == corridor_id)


def profile(scenario: str) -> ScenarioProfile:
    return next(p for p in PROFILES if p.scenario == scenario)
```

- [ ] **Step 4: Написать падающие тесты `tests/test_domain.py`**

```python
from decimal import Decimal as D

import pytest

from app.domain import (
    Corridor,
    DomainError,
    build_quotes,
    calculate_commission,
    counterparty_country,
    format_amount,
    validate_transfer,
)
from tests.factories import PROFILES, corridor, profile


class TestCalculateCommission:
    def test_formula_matches_ivrpy(self):
        result = calculate_commission(corridor("RU-CN-CNY"), D("100000"))
        assert result.base_fee == D("50.00")
        assert result.percentage_amount == D("1500.00")
        assert result.fixed_fee == D("25.00")
        assert result.total == D("1575.00")

    def test_total_is_raised_to_min_fee(self):
        # 50 + 15 + 25 = 90 < min_fee 100
        assert calculate_commission(corridor("RU-CN-CNY"), D("1000")).total == D("100.00")

    def test_total_is_capped_by_max_fee(self):
        assert calculate_commission(corridor("RU-CN-CNY"), D("999999")).total == D("5000.00")

    def test_rounds_half_up_to_cents(self):
        c = Corridor("X-Y-Z", "RU", "CN", "CNY", D("0"), D("0.005"), D("0"), D("1"), D("1000000"), D("0"), D("100"))
        result = calculate_commission(c, D("1001"))  # 5.005
        assert result.percentage_amount == D("5.01")
        assert result.total == D("5.01")

    def test_multiplier_applies_after_min_max_clamp(self):
        result = calculate_commission(corridor("RU-CN-CNY"), D("100000"), D("0.700"))
        assert result.multiplier == D("0.700")
        assert result.total == D("1102.50")


class TestValidateTransfer:
    def test_same_country_is_rejected(self):
        with pytest.raises(DomainError) as exc:
            validate_transfer("RU", "RU", D("100"))
        assert exc.value.code == "SAME_COUNTRY"

    @pytest.mark.parametrize("amount", [D("0"), D("-1")])
    def test_non_positive_amount_is_rejected(self, amount):
        with pytest.raises(DomainError) as exc:
            validate_transfer("RU", "CN", amount)
        assert exc.value.code == "INVALID_AMOUNT"


def test_format_amount_uses_space_thousands_separator():
    assert format_amount(D("5000000.00")) == "5 000 000"


@pytest.mark.parametrize(
    ("from_country", "to_country", "expected"),
    [("RU", "CN", "CN"), ("CN", "RU", "CN"), ("CN", "AE", "AE")],
)
def test_counterparty_country(from_country, to_country, expected):
    assert counterparty_country(from_country, to_country) == expected


class TestBuildQuotes:
    def test_returns_all_scenarios_in_fixed_order(self):
        quotes = build_quotes(corridor("RU-CN-CNY"), list(reversed(PROFILES)), "RU", "CN", "CNY", D("100000"))
        assert [q.profile.scenario for q in quotes] == ["cbdc", "bank_transfer", "smart_contract", "trade_finance"]

    def test_prices_available_scenarios_with_multiplier(self):
        quotes = {q.profile.scenario: q for q in build_quotes(corridor("RU-CN-CNY"), PROFILES, "RU", "CN", "CNY", D("100000"))}
        assert quotes["cbdc"].available and quotes["cbdc"].commission.total == D("630.00")
        assert quotes["bank_transfer"].commission.total == D("1575.00")
        assert quotes["smart_contract"].commission.total == D("1102.50")

    def test_scenario_min_amount_makes_quote_unavailable(self):
        quotes = {q.profile.scenario: q for q in build_quotes(corridor("RU-CN-CNY"), PROFILES, "RU", "CN", "CNY", D("100000"))}
        tf = quotes["trade_finance"]
        assert tf.available is False
        assert tf.commission is None
        assert tf.unavailable_reason == "Минимальная сумма для сценария: 5 000 000 CNY"

    def test_scenario_country_restriction(self):
        quotes = {q.profile.scenario: q for q in build_quotes(corridor("RU-TR-TRY"), PROFILES, "RU", "TR", "TRY", D("100000"))}
        assert quotes["cbdc"].unavailable_reason == "Сценарий недоступен для страны контрагента TR"
        assert quotes["smart_contract"].available is True
        assert quotes["bank_transfer"].commission.total == D("1642.00")

    def test_scenario_max_amount(self):
        quotes = {q.profile.scenario: q for q in build_quotes(corridor("RU-CN-RUB"), PROFILES, "RU", "CN", "RUB", D("60000000"))}
        assert quotes["cbdc"].unavailable_reason == "Максимальная сумма для сценария: 50 000 000 RUB"
        assert quotes["trade_finance"].available is True

    def test_missing_corridor_marks_everything_unavailable(self):
        quotes = build_quotes(None, PROFILES, "RU", "US", "USD", D("100000"))
        assert len(quotes) == 4
        assert all(not q.available for q in quotes)
        assert {q.unavailable_reason for q in quotes} == {"Коридор RU→US в USD не поддерживается"}

    def test_corridor_limits_apply_to_every_scenario(self):
        quotes = build_quotes(corridor("RU-CN-CNY"), PROFILES, "RU", "CN", "CNY", D("500"))
        assert {q.unavailable_reason for q in quotes} == {"Сумма ниже лимита коридора: минимум 1 000 CNY"}

    def test_export_direction_uses_sender_as_counterparty(self):
        quotes = {q.profile.scenario: q for q in build_quotes(corridor("CN-RU-RUB"), PROFILES, "CN", "RU", "RUB", D("100000"))}
        assert quotes["cbdc"].available is True

    def test_validates_transfer_first(self):
        with pytest.raises(DomainError) as exc:
            build_quotes(corridor("RU-CN-CNY"), PROFILES, "RU", "RU", "CNY", D("100000"))
        assert exc.value.code == "SAME_COUNTRY"


def test_profile_factory_lookup():
    assert profile("cbdc").fee_multiplier == D("0.400")
```

- [ ] **Step 5: Запустить тесты — должны упасть**

Run: `cd services/service-commission && uv run pytest tests/test_domain.py -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'app.domain'`.

- [ ] **Step 6: Реализовать `app/domain.py`**

```python
"""Бизнес-логика расчёта комиссии: чистые функции без БД, HTTP и фреймворков."""

from __future__ import annotations

from dataclasses import dataclass
from decimal import ROUND_HALF_UP, Decimal

CENT = Decimal("0.01")
HOME_COUNTRY = "RU"
SCENARIO_ORDER: tuple[str, ...] = ("cbdc", "bank_transfer", "smart_contract", "trade_finance")


def money(value: Decimal) -> Decimal:
    return value.quantize(CENT, rounding=ROUND_HALF_UP)


def format_amount(value: Decimal) -> str:
    return f"{value:,.0f}".replace(",", " ")


class DomainError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass(frozen=True)
class Corridor:
    corridor_id: str
    from_country: str
    to_country: str
    currency: str
    base_fee: Decimal
    percentage_fee: Decimal
    fixed_fee: Decimal
    min_limit: Decimal
    max_limit: Decimal
    min_fee: Decimal
    max_fee: Decimal


@dataclass(frozen=True)
class ScenarioProfile:
    scenario: str
    title: str
    description: str
    fee_multiplier: Decimal
    eta_label: str
    limitations: tuple[str, ...]
    min_amount: Decimal | None = None
    max_amount: Decimal | None = None
    countries: tuple[str, ...] | None = None


@dataclass(frozen=True)
class Commission:
    base_fee: Decimal
    percentage_fee: Decimal
    percentage_amount: Decimal
    fixed_fee: Decimal
    multiplier: Decimal
    total: Decimal


@dataclass(frozen=True)
class Quote:
    profile: ScenarioProfile
    available: bool
    unavailable_reason: str | None
    commission: Commission | None


def validate_transfer(from_country: str, to_country: str, amount: Decimal) -> None:
    if from_country == to_country:
        raise DomainError("SAME_COUNTRY", "Страны отправителя и получателя совпадают")
    if amount <= 0:
        raise DomainError("INVALID_AMOUNT", "Сумма должна быть больше нуля")


def corridor_limit_error(corridor: Corridor, amount: Decimal) -> DomainError | None:
    if amount < corridor.min_limit:
        return DomainError(
            "AMOUNT_BELOW_LIMIT",
            f"Сумма ниже лимита коридора: минимум {format_amount(corridor.min_limit)} {corridor.currency}",
        )
    if amount > corridor.max_limit:
        return DomainError(
            "AMOUNT_ABOVE_LIMIT",
            f"Сумма выше лимита коридора: максимум {format_amount(corridor.max_limit)} {corridor.currency}",
        )
    return None


def calculate_commission(corridor: Corridor, amount: Decimal, multiplier: Decimal = Decimal("1")) -> Commission:
    percentage_amount = amount * corridor.percentage_fee
    raw_total = corridor.base_fee + percentage_amount + corridor.fixed_fee
    clamped = min(max(raw_total, corridor.min_fee), corridor.max_fee)
    return Commission(
        base_fee=corridor.base_fee,
        percentage_fee=corridor.percentage_fee,
        percentage_amount=money(percentage_amount),
        fixed_fee=corridor.fixed_fee,
        multiplier=multiplier,
        total=money(clamped * multiplier),
    )


def counterparty_country(from_country: str, to_country: str) -> str:
    return to_country if from_country == HOME_COUNTRY else from_country


def build_quotes(
    corridor: Corridor | None,
    profiles: list[ScenarioProfile],
    from_country: str,
    to_country: str,
    currency: str,
    amount: Decimal,
) -> list[Quote]:
    validate_transfer(from_country, to_country, amount)
    by_scenario = {p.scenario: p for p in profiles}
    quotes: list[Quote] = []
    for scenario in SCENARIO_ORDER:
        scenario_profile = by_scenario.get(scenario)
        if scenario_profile is None:
            continue
        reason = _unavailable_reason(corridor, scenario_profile, from_country, to_country, currency, amount)
        if reason is not None or corridor is None:
            quotes.append(Quote(scenario_profile, False, reason, None))
        else:
            commission = calculate_commission(corridor, amount, scenario_profile.fee_multiplier)
            quotes.append(Quote(scenario_profile, True, None, commission))
    return quotes


def _unavailable_reason(
    corridor: Corridor | None,
    scenario_profile: ScenarioProfile,
    from_country: str,
    to_country: str,
    currency: str,
    amount: Decimal,
) -> str | None:
    if corridor is None:
        return f"Коридор {from_country}→{to_country} в {currency} не поддерживается"
    limit_error = corridor_limit_error(corridor, amount)
    if limit_error is not None:
        return limit_error.message
    country = counterparty_country(from_country, to_country)
    if scenario_profile.countries is not None and country not in scenario_profile.countries:
        return f"Сценарий недоступен для страны контрагента {country}"
    if scenario_profile.min_amount is not None and amount < scenario_profile.min_amount:
        return f"Минимальная сумма для сценария: {format_amount(scenario_profile.min_amount)} {currency}"
    if scenario_profile.max_amount is not None and amount > scenario_profile.max_amount:
        return f"Максимальная сумма для сценария: {format_amount(scenario_profile.max_amount)} {currency}"
    return None
```

- [ ] **Step 7: Запустить тесты — должны пройти**

Run: `cd services/service-commission && uv run pytest tests/test_domain.py -q`
Expected: все тесты PASS.

- [ ] **Step 8: Commit**

```bash
git add services/service-commission/pyproject.toml services/service-commission/uv.lock \
        services/service-commission/app services/service-commission/tests
git commit -q -m "feat(commission): Decimal commission domain with per-scenario quotes

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UJR72d6wzQrg47iW3EQuXH"
```

---

### Task 3: Схема БД, seed-миграции и Postgres-репозиторий

**Files:**
- Create: `services/service-commission/migrations/001_corridors.sql`
- Create: `services/service-commission/migrations/002_scenario_profiles.sql`
- Create: `services/service-commission/migrations/003_seed.sql`
- Create: `services/service-commission/app/repository.py`
- Create: `services/service-commission/tests/test_repository_pg.py`
- Delete: `services/service-commission/data/corridors.csv`
- Modify: `docker-compose.yml` (добавить `name: ivr`, `pg-commission`, `migrate-commission`, volume)
- Modify: `Makefile` (цель `test-commission-db`)

**Interfaces:**
- Consumes: `app.domain.Corridor`, `app.domain.ScenarioProfile` (Task 2).
- Produces (`app.repository`):
  - `class CorridorRepository(Protocol)`: `async find_corridor(from_country: str, to_country: str, currency: str) -> Corridor | None`; `async list_profiles() -> list[ScenarioProfile]`; `async ping() -> bool`
  - `class PgCorridorRepository(engine: AsyncEngine)` — реализация протокола.
- Produces (compose): сервисы `pg-commission` (порт хоста `127.0.0.1:5434`), `migrate-commission`; сеть compose-проекта называется `ivr_backend-net`.

- [ ] **Step 1: Написать миграцию `001_corridors.sql`**

```sql
CREATE TABLE corridors (
    corridor_id    TEXT PRIMARY KEY,
    from_country   CHAR(2)       NOT NULL,
    to_country     CHAR(2)       NOT NULL,
    currency       CHAR(3)       NOT NULL,
    base_fee       NUMERIC(18,2) NOT NULL CHECK (base_fee >= 0),
    percentage_fee NUMERIC(8,5)  NOT NULL CHECK (percentage_fee >= 0 AND percentage_fee < 1),
    fixed_fee      NUMERIC(18,2) NOT NULL CHECK (fixed_fee >= 0),
    min_limit      NUMERIC(18,2) NOT NULL,
    max_limit      NUMERIC(18,2) NOT NULL,
    min_fee        NUMERIC(18,2) NOT NULL,
    max_fee        NUMERIC(18,2) NOT NULL,
    CHECK (from_country <> to_country),
    CHECK (min_limit > 0 AND min_limit <= max_limit),
    CHECK (min_fee >= 0 AND min_fee <= max_fee),
    UNIQUE (from_country, to_country, currency)
);
```

- [ ] **Step 2: Написать миграцию `002_scenario_profiles.sql`**

```sql
CREATE TABLE scenario_profiles (
    scenario       TEXT PRIMARY KEY
                   CHECK (scenario IN ('cbdc', 'bank_transfer', 'smart_contract', 'trade_finance')),
    title          TEXT         NOT NULL,
    description    TEXT         NOT NULL,
    fee_multiplier NUMERIC(6,3) NOT NULL CHECK (fee_multiplier > 0),
    eta_label      TEXT         NOT NULL,
    limitations    TEXT[]       NOT NULL DEFAULT '{}',
    min_amount     NUMERIC(18,2),
    max_amount     NUMERIC(18,2),
    countries      CHAR(2)[]
);
```

- [ ] **Step 3: Написать миграцию `003_seed.sql`**

```sql
-- 1. Коридоры в национальных валютах: первые 10 строк — перенос 1:1 из ivrpy data/corridors.csv,
--    далее обратные направления и KZ/BY.
INSERT INTO corridors (corridor_id, from_country, to_country, currency, base_fee, percentage_fee, fixed_fee,
                       min_limit, max_limit, min_fee, max_fee) VALUES
    ('RU-CN-CNY', 'RU', 'CN', 'CNY',   50.00, 0.015,   25.00,  1000,   1000000,  100.00,    5000.00),
    ('RU-AE-AED', 'RU', 'AE', 'AED',   40.00, 0.012,   20.00,   500,    500000,   80.00,    3000.00),
    ('RU-IN-INR', 'RU', 'IN', 'INR',   35.00, 0.018,   15.00,  1000,    750000,   90.00,    4000.00),
    ('CN-RU-RUB', 'CN', 'RU', 'RUB',   45.00, 0.014,   20.00,  1000,    800000,   95.00,    4500.00),
    ('AE-RU-RUB', 'AE', 'RU', 'RUB',   38.00, 0.013,   18.00,   500,    600000,   85.00,    3500.00),
    ('RU-TR-TRY', 'RU', 'TR', 'TRY',   30.00, 0.016,   12.00,   500,    400000,   75.00,    2500.00),
    ('TR-RU-RUB', 'TR', 'RU', 'RUB',   32.00, 0.015,   14.00,   500,    400000,   78.00,    2800.00),
    ('CN-AE-AED', 'CN', 'AE', 'AED',   42.00, 0.011,   22.00,  1000,    900000,   88.00,    4200.00),
    ('AE-IN-INR', 'AE', 'IN', 'INR',   36.00, 0.017,   16.00,   500,    700000,   82.00,    3800.00),
    ('IN-CN-CNY', 'IN', 'CN', 'CNY',   48.00, 0.013,   24.00,  1000,    850000,   92.00,    4800.00),
    ('CN-RU-CNY', 'CN', 'RU', 'CNY',   45.00, 0.014,   20.00,  1000,    800000,   95.00,    4500.00),
    ('AE-RU-AED', 'AE', 'RU', 'AED',   38.00, 0.013,   18.00,   500,    600000,   85.00,    3500.00),
    ('IN-RU-INR', 'IN', 'RU', 'INR',   35.00, 0.017,   15.00,  1000,    700000,   88.00,    3900.00),
    ('TR-RU-TRY', 'TR', 'RU', 'TRY',   32.00, 0.015,   14.00,   500,    400000,   78.00,    2800.00),
    ('RU-KZ-KZT', 'RU', 'KZ', 'KZT', 2000.00, 0.010, 1000.00, 50000, 500000000, 5000.00, 2000000.00),
    ('KZ-RU-KZT', 'KZ', 'RU', 'KZT', 2000.00, 0.010, 1000.00, 50000, 500000000, 5000.00, 2000000.00),
    ('RU-BY-BYN', 'RU', 'BY', 'BYN',   15.00, 0.008,    8.00,   100,   1500000,   30.00,    1500.00),
    ('BY-RU-BYN', 'BY', 'RU', 'BYN',   15.00, 0.008,    8.00,   100,   1500000,   30.00,    1500.00);

-- 2. RUB и USD для всех стран-партнёров в обе стороны.
--    Строки из блока 1 имеют приоритет (например, CN-RU-RUB остаётся с лимитом 800 000).
INSERT INTO corridors (corridor_id, from_country, to_country, currency, base_fee, percentage_fee, fixed_fee,
                       min_limit, max_limit, min_fee, max_fee)
SELECT r.from_country || '-' || r.to_country || '-' || v.currency,
       r.from_country, r.to_country, v.currency,
       v.base_fee, r.percentage_fee, v.fixed_fee, v.min_limit, v.max_limit, v.min_fee, v.max_fee
FROM (VALUES
        ('RU', 'CN', 0.015), ('CN', 'RU', 0.014),
        ('RU', 'AE', 0.012), ('AE', 'RU', 0.013),
        ('RU', 'IN', 0.018), ('IN', 'RU', 0.017),
        ('RU', 'TR', 0.016), ('TR', 'RU', 0.015),
        ('RU', 'KZ', 0.010), ('KZ', 'RU', 0.010),
        ('RU', 'BY', 0.008), ('BY', 'RU', 0.008)
     ) AS r(from_country, to_country, percentage_fee)
CROSS JOIN (VALUES
        ('RUB', 500.00, 250.00, 10000, 500000000, 1000.00, 500000.00),
        ('USD',   5.00,   3.00,   100,   5000000,   10.00,   5000.00)
     ) AS v(currency, base_fee, fixed_fee, min_limit, max_limit, min_fee, max_fee)
ON CONFLICT (corridor_id) DO NOTHING;

-- 3. Профили сценариев расчёта (тексты карточек экрана 3).
INSERT INTO scenario_profiles (scenario, title, description, fee_multiplier, eta_label, limitations,
                               min_amount, max_amount, countries) VALUES
    ('cbdc', 'Расчёт через ЦВЦБ',
     'Перевод цифрового рубля напрямую между кошельками сторон, минуя корреспондентские счета.',
     0.400, '5–20 минут',
     ARRAY['Доступно только для стран-участниц пилота ЦВЦБ: CN, AE, IN, BY, KZ', 'Лимит 50 000 000 на сделку'],
     NULL, 50000000, ARRAY['CN', 'AE', 'IN', 'BY', 'KZ']::CHAR(2)[]),
    ('bank_transfer', 'Классический банковский перевод',
     'SWIFT/корреспондентский перевод через сеть банков-партнёров.',
     1.000, '1–3 рабочих дня',
     ARRAY['Требует полного комплекта валютных документов', 'Возможны задержки на стороне банка контрагента'],
     NULL, NULL, NULL),
    ('smart_contract', 'Смарт-контракт',
     'Условное депонирование в блокчейне: средства списываются при подтверждении поставки.',
     0.700, 'до 24 часов после подтверждения условий',
     ARRAY['Нужна цифровая подпись обеих сторон', 'Не поддерживает частичные поставки', 'Доступно для CN, AE, TR'],
     NULL, NULL, ARRAY['CN', 'AE', 'TR']::CHAR(2)[]),
    ('trade_finance', 'Торговое финансирование',
     'Аккредитив или банковская гарантия для сделок с длинным циклом поставки.',
     1.800, '3–10 рабочих дней на оформление',
     ARRAY['Требуется кредитный лимит в банке', 'Подходит для сумм от 5 000 000'],
     5000000, NULL, NULL);
```

Удалить CSV (данные теперь в миграции): `git rm -q services/service-commission/data/corridors.csv`

- [ ] **Step 4: Добавить Postgres и миграции commission в `docker-compose.yml`**

В самое начало файла (перед `networks:`) добавить строку:
```yaml
name: ivr

```
В `volumes:` добавить `pg_commission_data:` (строку `pg_test_python_data:` пока не трогать — её удалит Task 5).

После блока `pg-test-python:` добавить:
```yaml
  pg-commission:
    image: postgres:16-alpine
    environment:
      POSTGRES_USER: commission
      POSTGRES_PASSWORD: commission
      POSTGRES_DB: commission
    ports:
      - "127.0.0.1:5434:5432"   # только для локальных db-тестов
    volumes:
      - pg_commission_data:/var/lib/postgresql/data
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U commission"]
      interval: 2s
      retries: 20
    networks: [backend-net]
```
После блока `migrate-core:` добавить:
```yaml
  migrate-commission:
    image: postgres:16-alpine
    entrypoint: ["/bin/sh", "/app/migrate.sh"]
    volumes:
      - ./infra/migrate.sh:/app/migrate.sh:ro
      - ./services/service-commission/migrations:/app/migrations:ro
    environment:
      DB_HOST: pg-commission
      DB_PORT: "5432"
      DB_NAME: commission
      DB_USER: commission
      DB_PASSWORD: commission
    depends_on:
      pg-commission: { condition: service_healthy }
    networks: [backend-net]
```

- [ ] **Step 5: Добавить цель в `Makefile`**

В `.PHONY` добавить `test-commission test-commission-db`. После цели `test-core:` добавить:
```makefile
test-commission:
	cd services/service-commission && uv run pytest -q -m "not db"

test-commission-db:
	docker compose up -d pg-commission migrate-commission
	docker compose wait migrate-commission
	cd services/service-commission && \
		TEST_PG_DSN=postgresql+asyncpg://commission:commission@127.0.0.1:5434/commission \
		uv run pytest -q -m db
```

- [ ] **Step 6: Написать падающий db-тест `tests/test_repository_pg.py`**

```python
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
```

- [ ] **Step 7: Запустить — должно упасть**

Run: `make test-commission-db`
Expected: миграции применяются (`Done. Applied 3 migration(s).` в `docker compose logs migrate-commission`), тесты FAIL — `ModuleNotFoundError: No module named 'app.repository'`.

- [ ] **Step 8: Реализовать `app/repository.py`**

```python
from typing import Protocol

from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncEngine

from app.domain import Corridor, ScenarioProfile


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
            return False
```

- [ ] **Step 9: Запустить — должно пройти**

Run: `make test-commission-db && make test-commission`
Expected: db-тесты — 6 passed; unit — все passed (db-тесты там deselected).

- [ ] **Step 10: Commit**

```bash
git add services/service-commission docker-compose.yml Makefile
git commit -q -m "feat(commission): postgres schema, seed migrations and repository

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UJR72d6wzQrg47iW3EQuXH"
```

---

### Task 4: HTTP API, JWT-авторизация и фабрика приложения

**Files:**
- Create: `services/service-commission/app/errors.py`
- Create: `services/service-commission/app/auth.py`
- Create: `services/service-commission/app/schemas.py`
- Create: `services/service-commission/app/api.py`
- Create: `services/service-commission/app/config.py`
- Create: `services/service-commission/app/main.py`
- Modify: `services/service-commission/tests/factories.py` (добавить `FakeRepository`)
- Create: `services/service-commission/tests/conftest.py`
- Create: `services/service-commission/tests/test_auth.py`
- Create: `services/service-commission/tests/test_api.py`

**Interfaces:**
- Consumes: `app.domain.*` (Task 2), `app.repository.CorridorRepository`, `PgCorridorRepository` (Task 3).
- Produces:
  - `app.errors.ApiError(status: int, code: str, message: str)`, `install_error_handlers(app: FastAPI) -> None`
  - `app.auth.make_require_user(public_key: bytes) -> Callable[..., str]` — FastAPI-зависимость, возвращает `sub`
  - `app.api.create_router(repo: CorridorRepository, require_user: Callable[..., str]) -> APIRouter`
  - `app.main.create_app(repo: CorridorRepository, jwt_public_key: bytes, lifespan=None) -> FastAPI`
  - `app.main.build_default_app() -> FastAPI` — точка входа uvicorn (`--factory`), читает env `PG_DSN`, `JWT_PUBLIC_KEY_PATH`
  - HTTP: `GET /health`, `POST /api/commission/calculate`, `POST /api/commission/quotes` (контракты в тестах ниже)
  - `tests.factories.FakeRepository(corridors=CORRIDORS, profiles=PROFILES, healthy=True)`

- [ ] **Step 1: Добавить `FakeRepository` в конец `tests/factories.py`**

```python
class FakeRepository:
    def __init__(
        self,
        corridors: list[Corridor] | None = None,
        profiles: list[ScenarioProfile] | None = None,
        healthy: bool = True,
    ) -> None:
        self._corridors = CORRIDORS if corridors is None else corridors
        self._profiles = PROFILES if profiles is None else profiles
        self._healthy = healthy

    async def find_corridor(self, from_country: str, to_country: str, currency: str) -> Corridor | None:
        key = (from_country, to_country, currency)
        return next((c for c in self._corridors if (c.from_country, c.to_country, c.currency) == key), None)

    async def list_profiles(self) -> list[ScenarioProfile]:
        return list(self._profiles)

    async def ping(self) -> bool:
        return self._healthy
```

- [ ] **Step 2: Создать `tests/conftest.py`**

```python
import time
from collections.abc import Callable

import jwt
import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from fastapi.testclient import TestClient

from app.main import create_app
from tests.factories import FakeRepository


def _generate_rsa_pem() -> tuple[bytes, bytes]:
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    private_pem = key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    )
    public_pem = key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo
    )
    return private_pem, public_pem


@pytest.fixture(scope="session")
def rsa_keys() -> tuple[bytes, bytes]:
    return _generate_rsa_pem()


@pytest.fixture(scope="session")
def foreign_private_key() -> bytes:
    return _generate_rsa_pem()[0]


@pytest.fixture
def make_token(rsa_keys) -> Callable[..., str]:
    def _make(
        sub: str | None = "user-1",
        ttl: int = 300,
        issuer: str = "service-auth",
        audience: str = "internal",
        key: bytes | None = None,
    ) -> str:
        now = int(time.time())
        claims = {"iss": issuer, "aud": audience, "iat": now, "exp": now + ttl}
        if sub is not None:
            claims["sub"] = sub
        return jwt.encode(claims, key or rsa_keys[0], algorithm="RS256")

    return _make


@pytest.fixture
def auth_headers(make_token) -> dict[str, str]:
    return {"Authorization": f"Bearer {make_token()}"}


@pytest.fixture
def client(rsa_keys) -> TestClient:
    return TestClient(create_app(FakeRepository(), rsa_keys[1]))
```

- [ ] **Step 3: Написать падающие тесты `tests/test_auth.py`**

```python
import pytest

BODY = {"from_country": "RU", "to_country": "CN", "currency": "CNY", "amount": 100000}
URL = "/api/commission/quotes"


def test_valid_token_is_accepted(client, auth_headers):
    assert client.post(URL, json=BODY, headers=auth_headers).status_code == 200


def test_missing_token(client):
    response = client.post(URL, json=BODY)
    assert response.status_code == 401
    assert response.json() == {"code": "UNAUTHORIZED", "error": "Отсутствует токен авторизации"}


def test_expired_token(client, make_token):
    response = client.post(URL, json=BODY, headers={"Authorization": f"Bearer {make_token(ttl=-10)}"})
    assert response.status_code == 401
    assert response.json()["code"] == "TOKEN_EXPIRED"


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
```

- [ ] **Step 4: Написать падающие тесты `tests/test_api.py`**

```python
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
```

- [ ] **Step 5: Запустить — должно упасть**

Run: `cd services/service-commission && uv run pytest tests/test_auth.py tests/test_api.py -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'app.main'`.

- [ ] **Step 6: Реализовать `app/errors.py`**

```python
from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from app.domain import DomainError

_DOMAIN_STATUS = {"CORRIDOR_NOT_FOUND": 404}


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


def _error(status: int, code: str, message: str, **extra) -> JSONResponse:
    return JSONResponse(status_code=status, content={"code": code, "error": message, **extra})


def install_error_handlers(app: FastAPI) -> None:
    @app.exception_handler(ApiError)
    async def _api_error(_: Request, exc: ApiError) -> JSONResponse:
        return _error(exc.status, exc.code, exc.message)

    @app.exception_handler(DomainError)
    async def _domain_error(_: Request, exc: DomainError) -> JSONResponse:
        return _error(_DOMAIN_STATUS.get(exc.code, 400), exc.code, exc.message)

    @app.exception_handler(RequestValidationError)
    async def _validation_error(_: Request, exc: RequestValidationError) -> JSONResponse:
        details = [{"loc": list(e["loc"]), "msg": e["msg"]} for e in exc.errors()]
        return _error(422, "VALIDATION_ERROR", "Некорректные параметры запроса", details=details)
```

- [ ] **Step 7: Реализовать `app/auth.py`**

```python
from collections.abc import Callable
from typing import Annotated

import jwt
from fastapi import Header

from app.errors import ApiError

JWT_ISSUER = "service-auth"
JWT_AUDIENCE = "internal"


def make_require_user(public_key: bytes) -> Callable[..., str]:
    def require_user(authorization: Annotated[str, Header()] = "") -> str:
        if not authorization.startswith("Bearer "):
            raise ApiError(401, "UNAUTHORIZED", "Отсутствует токен авторизации")
        token = authorization.removeprefix("Bearer ")
        try:
            claims = jwt.decode(
                token,
                public_key,
                algorithms=["RS256"],
                audience=JWT_AUDIENCE,
                issuer=JWT_ISSUER,
                options={"require": ["exp", "sub", "iss", "aud"]},
            )
        except jwt.ExpiredSignatureError:
            raise ApiError(401, "TOKEN_EXPIRED", "Срок действия токена истёк") from None
        except jwt.InvalidTokenError:
            raise ApiError(401, "INVALID_TOKEN", "Недействительный токен") from None
        return claims["sub"]

    return require_user
```

- [ ] **Step 8: Реализовать `app/schemas.py`**

```python
from decimal import Decimal
from typing import Annotated, Any

from pydantic import BaseModel, StringConstraints

from app.domain import Commission, Corridor, Quote

CountryCode = Annotated[str, StringConstraints(strip_whitespace=True, to_upper=True, pattern=r"^[A-Z]{2}$")]
CurrencyCode = Annotated[str, StringConstraints(strip_whitespace=True, to_upper=True, pattern=r"^[A-Z]{3}$")]


class TransferRequest(BaseModel):
    from_country: CountryCode
    to_country: CountryCode
    currency: CurrencyCode
    amount: Decimal


def commission_json(commission: Commission) -> dict[str, float]:
    return {
        "base_fee": float(commission.base_fee),
        "percentage_fee": float(commission.percentage_fee),
        "percentage_amount": float(commission.percentage_amount),
        "fixed_fee": float(commission.fixed_fee),
        "multiplier": float(commission.multiplier),
        "total": float(commission.total),
    }


def calculate_json(corridor: Corridor, amount: Decimal, commission: Commission) -> dict[str, Any]:
    return {
        "corridor_id": corridor.corridor_id,
        "from_country": corridor.from_country,
        "to_country": corridor.to_country,
        "currency": corridor.currency,
        "amount": float(amount),
        "base_fee": float(commission.base_fee),
        "percentage_fee": float(commission.percentage_fee),
        "percentage_amount": float(commission.percentage_amount),
        "fixed_fee": float(commission.fixed_fee),
        "total_commission": float(commission.total),
        "min_fee": float(corridor.min_fee),
        "max_fee": float(corridor.max_fee),
    }


def quotes_json(corridor: Corridor | None, quotes: list[Quote]) -> dict[str, Any]:
    return {
        "corridor_id": corridor.corridor_id if corridor else None,
        "quotes": [
            {
                "scenario": q.profile.scenario,
                "title": q.profile.title,
                "description": q.profile.description,
                "eta_label": q.profile.eta_label,
                "limitations": list(q.profile.limitations),
                "available": q.available,
                "unavailable_reason": q.unavailable_reason,
                "commission": commission_json(q.commission) if q.commission else None,
            }
            for q in quotes
        ],
    }
```

- [ ] **Step 9: Реализовать `app/api.py`**

```python
from collections.abc import Callable
from typing import Annotated, Any

from fastapi import APIRouter, Depends
from fastapi.responses import JSONResponse

from app.domain import DomainError, build_quotes, calculate_commission, corridor_limit_error, validate_transfer
from app.repository import CorridorRepository
from app.schemas import TransferRequest, calculate_json, quotes_json

SERVICE_NAME = "service-commission"
SERVICE_VERSION = "1.0.0"


def create_router(repo: CorridorRepository, require_user: Callable[..., str]) -> APIRouter:
    router = APIRouter()
    User = Annotated[str, Depends(require_user)]

    @router.get("/health", include_in_schema=False)
    async def health() -> JSONResponse:
        postgres_ok = await repo.ping()
        return JSONResponse(
            status_code=200 if postgres_ok else 503,
            content={"ok": postgres_ok, "service": SERVICE_NAME, "version": SERVICE_VERSION, "postgres_ok": postgres_ok},
        )

    @router.post("/api/commission/calculate", summary="Комиссия по коридору (совместимо с ivrpy)")
    async def calculate(body: TransferRequest, _user: User) -> dict[str, Any]:
        validate_transfer(body.from_country, body.to_country, body.amount)
        corridor = await repo.find_corridor(body.from_country, body.to_country, body.currency)
        if corridor is None:
            raise DomainError(
                "CORRIDOR_NOT_FOUND",
                f"Коридор {body.from_country}→{body.to_country} в {body.currency} не поддерживается",
            )
        limit_error = corridor_limit_error(corridor, body.amount)
        if limit_error is not None:
            raise limit_error
        return calculate_json(corridor, body.amount, calculate_commission(corridor, body.amount))

    @router.post("/api/commission/quotes", summary="Котировки по всем сценариям расчёта")
    async def quotes(body: TransferRequest, _user: User) -> dict[str, Any]:
        validate_transfer(body.from_country, body.to_country, body.amount)
        corridor = await repo.find_corridor(body.from_country, body.to_country, body.currency)
        profiles = await repo.list_profiles()
        return quotes_json(
            corridor,
            build_quotes(corridor, profiles, body.from_country, body.to_country, body.currency, body.amount),
        )

    return router
```

- [ ] **Step 10: Реализовать `app/config.py` и `app/main.py`**

`app/config.py`:
```python
import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    pg_dsn: str
    jwt_public_key_path: str

    @classmethod
    def from_env(cls) -> "Settings":
        return cls(pg_dsn=os.environ["PG_DSN"], jwt_public_key_path=os.environ["JWT_PUBLIC_KEY_PATH"])
```

`app/main.py`:
```python
from collections.abc import AsyncIterator, Callable
from contextlib import AbstractAsyncContextManager, asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from sqlalchemy.ext.asyncio import create_async_engine

from app.api import SERVICE_NAME, SERVICE_VERSION, create_router
from app.auth import make_require_user
from app.config import Settings
from app.errors import install_error_handlers
from app.repository import CorridorRepository, PgCorridorRepository

Lifespan = Callable[[FastAPI], AbstractAsyncContextManager[None]]


def create_app(repo: CorridorRepository, jwt_public_key: bytes, lifespan: Lifespan | None = None) -> FastAPI:
    app = FastAPI(title=SERVICE_NAME, version=SERVICE_VERSION, lifespan=lifespan)
    install_error_handlers(app)
    app.include_router(create_router(repo, make_require_user(jwt_public_key)))
    return app


def build_default_app() -> FastAPI:
    settings = Settings.from_env()
    engine = create_async_engine(settings.pg_dsn, pool_size=5, max_overflow=10, pool_pre_ping=True)

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncIterator[None]:
        yield
        await engine.dispose()

    public_key = Path(settings.jwt_public_key_path).read_bytes()
    return create_app(PgCorridorRepository(engine), public_key, lifespan)
```

- [ ] **Step 11: Запустить все unit-тесты**

Run: `make test-commission`
Expected: все PASS (test_domain, test_auth, test_api), db-тесты deselected.

- [ ] **Step 12: Commit**

```bash
git add services/service-commission/app services/service-commission/tests
git commit -q -m "feat(commission): JWT-protected calculate and quotes API

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UJR72d6wzQrg47iW3EQuXH"
```

---

### Task 5: Docker-образ, compose-стенд и удаление service-test-python

**Files:**
- Modify: `services/service-commission/Dockerfile` (полная замена)
- Modify: `services/service-commission/README.md` (полная замена)
- Delete: `services/service-test-python/`
- Modify: `docker-compose.yml`, `infra/traefik/dynamic.yml`, `Makefile`, `README.md`, `infra/gen-openapi.sh`, `docs/openapi.yml` (перегенерация), `docs/new-service-python.md:226`, `docs/new-service-cpp.md:305`

**Interfaces:**
- Consumes: `app.main.build_default_app` (Task 4), `pg-commission`/`migrate-commission` (Task 3).
- Produces: compose-сервис `service-commission` (внутренний, `http://service-commission:8000`); Makefile-цель `smoke-commission`.

- [ ] **Step 1: Заменить `services/service-commission/Dockerfile`**

Проверить версию uv для пина образа: `uv --version` (ожидается `0.11.x`). Использовать тот же minor-тег:
```dockerfile
FROM python:3.12-slim

COPY --from=ghcr.io/astral-sh/uv:0.11 /uv /usr/local/bin/uv

ENV UV_COMPILE_BYTECODE=1 \
    UV_LINK_MODE=copy \
    UV_PROJECT_ENVIRONMENT=/app/.venv \
    PATH="/app/.venv/bin:$PATH"

WORKDIR /app

COPY services/service-commission/pyproject.toml services/service-commission/uv.lock ./
RUN uv sync --locked --no-dev --no-install-project

COPY services/service-commission/app ./app

RUN useradd --system --no-create-home app
USER app

EXPOSE 8000
CMD ["uvicorn", "app.main:build_default_app", "--factory", "--host", "0.0.0.0", "--port", "8000"]
```

- [ ] **Step 2: Заменить сервис в `docker-compose.yml`**

Удалить блоки `pg-test-python:` и `service-test-python:` целиком и строку `pg_test_python_data:` в `volumes:`. Добавить в секцию «Сервисы»:
```yaml
  service-commission:
    build:
      context: .
      dockerfile: services/service-commission/Dockerfile
    environment:
      PG_DSN: "postgresql+asyncpg://commission:commission@pg-commission:5432/commission"
      JWT_PUBLIC_KEY_PATH: /etc/keys/jwt_public.pem
    volumes:
      - ./infra/keys/jwt_public.pem:/etc/keys/jwt_public.pem:ro
    depends_on:
      migrate-commission: { condition: service_completed_successfully }
    healthcheck:
      test: ["CMD", "python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/health')"]
      interval: 5s
      retries: 20
    networks: [backend-net]
```

- [ ] **Step 3: Убрать test-python из `infra/traefik/dynamic.yml`**

Удалить роутер `test-python:` (4 строки: `rule`, `service`, `middlewares`, `entryPoints`) и сервис `test-python:` (`loadBalancer` + `servers` + `url`). commission в Traefik не добавлять.

- [ ] **Step 4: Обновить `Makefile` (compose-часть)**

- В `.PHONY` заменить `test-test-python` на `smoke-commission`, удалить `test-health2` если присутствует.
- В цели `up` заменить строки
  ```
  	@echo "  http://localhost/api/test-python/...  -> service-test-python"
  	@echo "  http://localhost/health2             -> service-test-python health"
  ```
  на
  ```
  	@echo "  service-commission                   -> только внутри сети (make smoke-commission)"
  ```
- В `logs` заменить `service-test-python` на `service-commission`.
- Удалить цели `test-test-python:` и `test-health2:` целиком, вместо них добавить:
  ```makefile
  smoke-commission:
  	@if [ -z "$$TOKEN" ]; then echo "set TOKEN=..."; exit 1; fi
  	docker run --rm --network ivr_backend-net curlimages/curl:8.10.1 -s \
  		-X POST http://service-commission:8000/api/commission/quotes \
  		-H "Authorization: Bearer $$TOKEN" -H 'Content-Type: application/json' \
  		-d '{"from_country":"RU","to_country":"CN","currency":"CNY","amount":100000}'
  ```

- [ ] **Step 5: Удалить service-test-python**

Run: `git rm -r -q services/service-test-python`

- [ ] **Step 6: Переключить `infra/gen-openapi.sh` на commission**

- Строку `FASTAPI_APP="services/service-test-python/app"` заменить на `FASTAPI_APP="services/service-commission"`.
- Всё тело функции `generate_fastapi_paths` от `local json_spec=""` до закрывающего `fi` fallback-блока (оба способа: `docker compose exec …` и python с мок-зависимостями) заменить на:
  ```bash
  local json_spec=""
  json_spec="$(cd "${FASTAPI_APP}" && uv run --quiet python -c \
    'import json; from app.main import create_app; print(json.dumps(create_app(None, b"").openapi()))' \
    2>/dev/null)" || true
  ```
- `# service-test-python: не удалось извлечь OpenAPI` (оба места) → `# service-commission: не удалось извлечь OpenAPI`; строку `# (нужен запущенный контейнер или python3 + fastapi + pydantic)` → `# (нужен uv и зависимости services/service-commission)`.
- `grep -c '^  /api/test-python\|^  /health2'` → `grep -c '^  /api/commission'`.

Run: `make openapi`
Expected: `FastAPI endpoints: 2`; в `docs/openapi.yml` есть `/api/commission/calculate` и `/api/commission/quotes`, нет `/api/test-python`.

- [ ] **Step 7: Обновить документацию**

- `docs/new-service-python.md:226` и `docs/new-service-cpp.md:305`: `service-test-python` → `service-commission`.
- `README.md`:
  - Первую строку описания заменить на: `Микросервисная платформа Alfa Global CBDC Hub: сервисы за общим ingress, каждый со своей БД, объединённые через JWT.`
  - В ASCII-схеме заменить `/api/test-python/*` на `(внутр.)`, `service-test-python│` на `service-commission │`, `пишет: напарница` на `комиссии/котировки`, `pg-test-python     ` на `pg-commission      `.
  - В quick start удалить строки `make test-test-python` и `make test-health2`, добавить `make smoke-commission`.
  - В структуре: `│   └── service-test-python/            # Python/FastAPI, валидирует JWT` → `│   └── service-commission/           # Python/FastAPI, комиссии и котировки, валидирует JWT`.
  - Раздел `### В service-test-python (Python)` с примером кода заменить на:
    ````markdown
    ### В service-commission (Python)

    Логика — чистые функции в `app/domain.py` (тесты в `tests/test_domain.py`), доступ к БД — `app/repository.py`,
    ручки — `app/api.py` внутри `create_router`. Авторизация — зависимость `User` (JWT, возвращает `sub`).
    Миграции — `migrations/NNN_*.sql`. Тесты: `make test-commission` и `make test-commission-db`.
    ````
  - Удалить строку `- Alembic migrations для test-python (сейчас просто заглушка БД)` и строку `- Тесты (Catch2 для C++, pytest для Python)` заменить на `- Тесты Catch2 для C++`.
- `services/service-commission/README.md` заменить на:
  ````markdown
  # service-commission

  Расчёт комиссии трансграничных платежей и котировки сценариев расчёта (ЦВЦБ, банковский перевод,
  смарт-контракт, торговое финансирование). Внутренний сервис платформы, наружу не публикуется.

  Формула: `clamp(base_fee + amount × percentage_fee + fixed_fee, min_fee, max_fee) × multiplier сценария`.

  | Метод | Путь | |
  |---|---|---|
  | GET | `/health` | без JWT |
  | POST | `/api/commission/calculate` | комиссия по коридору (контракт ivrpy) |
  | POST | `/api/commission/quotes` | 4 котировки; недоступные — с причиной |

  ```bash
  make test-commission       # unit + API
  make test-commission-db    # репозиторий против Postgres с миграциями
  ```
  ````

- [ ] **Step 8: Поднять стенд и проверить руками**

```bash
make up
docker compose ps service-commission          # STATUS: healthy
TOKEN=$(curl -s -X POST http://localhost/api/auth/register -H 'Content-Type: application/json' \
  -d '{"login":"smoke-commission","password":"hunter22","name":"Smoke"}' | jq -r .token)
[ "$TOKEN" = "null" ] && TOKEN=$(curl -s -X POST http://localhost/api/auth/login -H 'Content-Type: application/json' \
  -d '{"login":"smoke-commission","password":"hunter22"}' | jq -r .token)
TOKEN=$TOKEN make smoke-commission
```
Expected: JSON с `"corridor_id":"RU-CN-CNY"` и 4 котировками; у `bank_transfer` `"total":1575.0`. Если сборка C++-сервисов не укладывается в таймаут, запустить `docker compose up -d --build service-commission service-auth traefik` отдельно (фоном) и дождаться.

- [ ] **Step 9: Commit**

```bash
git add -A docker-compose.yml infra/traefik/dynamic.yml Makefile README.md infra/gen-openapi.sh docs \
          services/service-commission services/service-test-python
git commit -q -m "feat(commission): run service in compose stack, remove service-test-python

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UJR72d6wzQrg47iW3EQuXH"
```

---

### Task 6: Helm: миграции из SQL-файлов, values commission, CD

**Files:**
- Create: `infra/gen-migration-values.sh`
- Create: `infra/helm/values-service-commission.yaml`
- Delete: `infra/helm/values-service-test-python.yaml`
- Modify: `infra/helm/values-service-auth.yaml`, `infra/helm/values-service-core.yaml` (убрать `migrations.files`)
- Modify: `.gitignore` (добавить `infra/helm/generated/`)
- Modify: `Makefile` (k3s-часть)
- Modify: `.github/workflows/deploy.yml`

**Interfaces:**
- Consumes: `services/<svc>/migrations/*.sql`; chart `infra/helm/generic-service` (ключ `migrations.files: map[filename]content`).
- Produces: `bash infra/gen-migration-values.sh <service-name>` → печатает путь и пишет `infra/helm/generated/migrations-<service-name>.yaml`.

- [ ] **Step 1: Написать `infra/gen-migration-values.sh`**

```bash
#!/usr/bin/env bash
# Генерирует helm values с SQL-миграциями сервиса из services/<svc>/migrations/*.sql.
# Единственный источник правды — .sql файлы; вручную миграции в values не пишутся.
set -euo pipefail

svc="${1:?usage: gen-migration-values.sh <service-name>}"
root="$(cd "$(dirname "$0")/.." && pwd)"
src="${root}/services/${svc}/migrations"
out_dir="${root}/infra/helm/generated"
out="${out_dir}/migrations-${svc}.yaml"

mkdir -p "${out_dir}"
shopt -s nullglob
files=("${src}"/*.sql)

{
  echo "# СГЕНЕРИРОВАНО infra/gen-migration-values.sh — не редактировать"
  echo "migrations:"
  if [ "${#files[@]}" -eq 0 ]; then
    echo "  files: {}"
  else
    echo "  files:"
    for f in "${files[@]}"; do
      echo "    $(basename "${f}"): |"
      sed 's/^/      /' "${f}"
      echo ""
    done
  fi
} > "${out}"

echo "${out}"
```
Run: `chmod +x infra/gen-migration-values.sh && echo 'infra/helm/generated/' >> .gitignore`

- [ ] **Step 2: Проверить генерацию и рендер чарта**

Run:
```bash
for s in service-auth service-core service-commission; do bash infra/gen-migration-values.sh "$s"; done
grep -c '^    [0-9].*\.sql: |$' infra/helm/generated/migrations-service-commission.yaml
```
Expected: три пути; счётчик `3`.

- [ ] **Step 3: Удалить ручные SQL из values auth/core**

В `infra/helm/values-service-auth.yaml` и `infra/helm/values-service-core.yaml` удалить ключ `files:` вместе со всем вложенным SQL-текстом под `migrations:` (остаются `enabled`, `image`, `db`).

- [ ] **Step 4: Создать `infra/helm/values-service-commission.yaml`, удалить values test-python**

```yaml
replicaCount: 1

image:
  repository: localhost:5000/service-commission
  tag: latest

service:
  port: 8000

config:
  enabled: false

jwtKeys:
  private: false
  public: true
  publicFile: jwt_public.pem
  mountPath: /etc/keys

env:
  - name: PG_DSN
    value: "postgresql+asyncpg://commission:commission@pg-commission-postgresql.data.svc.cluster.local:5432/commission"
  - name: JWT_PUBLIC_KEY_PATH
    value: /etc/keys/jwt_public.pem

migrations:
  enabled: true
  image: postgres:16
  db:
    host: pg-commission-postgresql.data.svc.cluster.local
    port: "5432"
    name: commission
    user: commission
    password: commission

ingress:
  enabled: false

cors:
  enabled: false

networkPolicy:
  enabled: true

healthcheck:
  path: /health
```
Run: `git rm -q infra/helm/values-service-test-python.yaml`

- [ ] **Step 5: Проверить рендер helm**

Run:
```bash
for s in service-auth service-core service-commission; do
  helm template "$s" infra/helm/generic-service -f "infra/helm/values-$s.yaml" \
    -f "infra/helm/generated/migrations-$s.yaml" > /tmp/claude-render-$s.yaml && echo "$s ok"
done
grep -c '003_seed.sql' /tmp/claude-render-service-commission.yaml
grep -c 'CREATE TABLE IF NOT EXISTS users' /tmp/claude-render-service-auth.yaml
```
Expected: `service-auth ok`, `service-core ok`, `service-commission ok`; оба счётчика ≥ 1 (миграции попали в ConfigMap из файлов).

- [ ] **Step 6: Обновить k3s-часть `Makefile`**

- `K3S_SERVICES := service-auth service-core service-test-python` → `K3S_SERVICES := service-auth service-core service-commission`.
- В `k3s-deploy-data` блок `helm upgrade --install pg-test-python …` заменить на:
  ```makefile
  	helm upgrade --install pg-commission oci://registry-1.docker.io/bitnamicharts/postgresql \
  		-n data \
  		--set auth.username=commission --set auth.password=commission \
  		--set auth.database=commission --set primary.persistence.size=1Gi
  ```
- В `k3s-deploy-%:` первой строкой рецепта добавить `	bash infra/gen-migration-values.sh $*`, и в обоих вызовах `helm upgrade --install $* $(HELM_CHART) -f …values…` добавить после `-f` с values: `-f infra/helm/generated/migrations-$*.yaml`.
- В `down-k3s`: `service-test-python` → `service-commission`, `pg-test-python` → `pg-commission`.
- В `k3s-test-health` удалить строку `@curl -sf $(BASE_URL)/health2 | python3 -m json.tool`.

Run: `make -n k3s-deploy-service-commission | head -5`
Expected: в выводе есть `gen-migration-values.sh service-commission` и `-f infra/helm/generated/migrations-service-commission.yaml`.

- [ ] **Step 7: Обновить `.github/workflows/deploy.yml`**

- `IMAGE_NAME_TEST: ${{ github.repository }}/service-test-python` → `IMAGE_NAME_COMMISSION: ${{ github.repository }}/service-commission`.
- Шаги metadata/build для test-python: `id: meta-test` → `id: meta-commission`, `images: …IMAGE_NAME_TEST` → `…IMAGE_NAME_COMMISSION`, `file: services/service-test-python/Dockerfile` → `file: services/service-commission/Dockerfile`, `steps.meta-test.outputs.*` → `steps.meta-commission.outputs.*`, имена шагов — `commission`.
- В job `deploy` перед шагом `Copy Helm charts to server` добавить:
  ```yaml
      - name: Generate migration values
        run: |
          for s in service-auth service-core service-commission; do
            bash infra/gen-migration-values.sh "$s"
          done
  ```
- В скрипте деплоя к каждому `helm upgrade` добавить `-f generated/migrations-<svc>.yaml \` сразу после строки `-f values-<svc>.yaml \`; блок `# Test Python` заменить на:
  ```bash
              # Commission
              helm upgrade --install service-commission ./generic-service \
                -f values-service-commission.yaml \
                -f generated/migrations-service-commission.yaml \
                --set image.repository=${{ env.REGISTRY }}/${{ env.IMAGE_NAME_COMMISSION }} \
                --set image.tag=${{ needs.build-and-push.outputs.tag }} \
                --set "imagePullSecrets[0].name=ghcr-secret" \
                -n backend
  ```

Run: `grep -n "test-python\|IMAGE_NAME_TEST\|meta-test" .github/workflows/deploy.yml; python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/deploy.yml'))" && echo yaml-ok`
Expected: grep ничего не находит; `yaml-ok`.

- [ ] **Step 7b: Финальная проверка отсутствия test-python**

Run: `git grep -n "test-python\|test_python" -- ':!docs/superpowers'`
Expected: пустой вывод.

- [ ] **Step 8: Commit**

```bash
git add -A infra .gitignore Makefile .github/workflows/deploy.yml
git commit -q -m "feat(infra): helm migrations generated from SQL files, deploy service-commission

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UJR72d6wzQrg47iW3EQuXH"
```
