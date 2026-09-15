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
    subtotal: Decimal
    clamped: str | None
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
    clamped_value = min(max(raw_total, corridor.min_fee), corridor.max_fee)
    if clamped_value > raw_total:
        clamped = "min"
    elif clamped_value < raw_total:
        clamped = "max"
    else:
        clamped = None
    return Commission(
        base_fee=corridor.base_fee,
        percentage_fee=corridor.percentage_fee,
        percentage_amount=money(percentage_amount),
        fixed_fee=corridor.fixed_fee,
        subtotal=money(clamped_value),
        clamped=clamped,
        multiplier=multiplier,
        total=money(clamped_value * multiplier),
    )


def counterparty_country(from_country: str, to_country: str) -> str:
    if from_country == HOME_COUNTRY:
        return to_country
    if to_country == HOME_COUNTRY:
        return from_country
    return to_country


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
