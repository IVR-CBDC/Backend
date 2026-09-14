from decimal import Decimal
from typing import Annotated, Any

from pydantic import BaseModel, StringConstraints, field_validator

from app.domain import Commission, Corridor, Quote

CountryCode = Annotated[str, StringConstraints(strip_whitespace=True, to_upper=True, pattern=r"^[A-Z]{2}$")]
CurrencyCode = Annotated[str, StringConstraints(strip_whitespace=True, to_upper=True, pattern=r"^[A-Z]{3}$")]


class TransferRequest(BaseModel):
    from_country: CountryCode
    to_country: CountryCode
    currency: CurrencyCode
    amount: Decimal

    @field_validator("from_country", "to_country", "currency", mode="before")
    @classmethod
    def _uppercase(cls, value: Any) -> Any:
        if isinstance(value, str):
            return value.strip().upper()
        return value


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
