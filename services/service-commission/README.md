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

## Изменения контракта относительно ivrpy

- Путь: `/calculate` → `/api/commission/calculate`.
- Формат ошибок: `{"detail": "CODE"}` → `{"code": "CODE", "error": "человеко-читаемое сообщение"}`.
- Требуется JWT (`Authorization: Bearer ...`), которого не было в ivrpy.
- `/api/commission/quotes` — новая ручка, которой в ivrpy не было (котировки по 4 сценариям расчёта).
