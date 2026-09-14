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
