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
        assert result.subtotal == D("1575.00")
        assert result.clamped is None
        assert result.total == D("1575.00")

    def test_total_is_raised_to_min_fee(self):
        # 50 + 15 + 25 = 90 < min_fee 100
        result = calculate_commission(corridor("RU-CN-CNY"), D("1000"))
        assert result.subtotal == D("100.00")
        assert result.clamped == "min"
        assert result.total == D("100.00")

    def test_total_is_capped_by_max_fee(self):
        result = calculate_commission(corridor("RU-CN-CNY"), D("999999"))
        assert result.subtotal == D("5000.00")
        assert result.clamped == "max"
        assert result.total == D("5000.00")

    def test_rounds_half_up_to_cents(self):
        c = Corridor("X-Y-Z", "RU", "CN", "CNY", D("0"), D("0.005"), D("0"), D("1"), D("1000000"), D("0"), D("100"))
        result = calculate_commission(c, D("1001"))  # 5.005
        assert result.percentage_amount == D("5.01")
        assert result.total == D("5.01")

    def test_multiplier_applies_after_min_max_clamp(self):
        result = calculate_commission(corridor("RU-CN-CNY"), D("100000"), D("0.700"))
        assert result.multiplier == D("0.700")
        assert result.subtotal == D("1575.00")
        assert result.clamped is None
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
