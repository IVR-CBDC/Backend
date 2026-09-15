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
