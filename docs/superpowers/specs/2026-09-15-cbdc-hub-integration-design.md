# Alfa Global CBDC Hub — объединение в единую систему

Дата: 2026-09-15 · Статус: утверждено к планированию · Контекст: защита ИВР, «всё по-взрослому»
(деплой в k3s, тесты, миграции).

## 1. Цель и границы

Сейчас три несвязанных куска:

| Проект | Что есть | Чего нет |
|---|---|---|
| `alfa-cbdc-hub` (SPA + BFF, ~1.7k строк TS) | 5 экранов, BFF с моками в памяти, WS | авторизации, реальных данных, тестов, git |
| `IVR` (`IVR-CBDC/Backend`) | Traefik, JWT RS256, auth на C++, Helm/k3s, CD | доменной логики: core — заглушка `compute` |
| `ivrpy` (`IVR-CBDC/commission-service`) | расчёт комиссии по коридорам (CSV + pandas), pytest | JWT, БД, интеграции, сценариев расчёта |

**Результат:** один работающий продукт — пользователь юрлица логинится, создаёт сделку, видит
реальную комиссию по 4 сценариям, подаёт документы, наблюдает живой трекинг с задержками
внешних проверок. Разворачивается одной командой локально (`make up`) и в k3s через CD.

**Критерий готовности:** Playwright-сценарий «регистрация → создание сделки → выбор сценария с
комиссией из commission-service → подача документов → сделка доходит до `completed` в трекинге
без перезагрузки страницы» зелёный в CI против поднятого стенда.

**Вне границ (YAGNI):** хранение файлов (документ = статус), реальные ФНС/банк/блокчейн
(эмулятор), refresh-токены, мультиязычность, Prometheus-метрики, HTTPS/Let's Encrypt,
админ-панель, роли внутри компании.

## 2. Репозитории

### 2.1 `IVR-CBDC/Backend` — монорепо бэкенда и единая точка деплоя

```
Backend/
├── services/
│   ├── service-auth/         C++/Drogon — +company, company_id в JWT
│   ├── service-core/         C++/Drogon — домен сделок (compute/status удаляются)
│   └── service-commission/   Python/FastAPI — перенос ivrpy через git subtree
│   (service-test-python удаляется целиком)
├── libs/common/              jwt_verifier (+company_id), jwt_filter, api_error
├── infra/
│   ├── traefik/              compose-роутинг
│   ├── helm/                 generic-service + values-{auth,core,commission,bff,frontend}.yaml
│   └── gen-migration-values.sh   SQL-миграции → values для helm (один источник правды)
├── tests/api/                pytest-сценарии против поднятого compose (контракт всех сервисов)
├── docs/openapi/             core.yml, commission.yml, auth.yml
├── docs/adr/                 архитектурные решения
├── docker-compose.yml        весь стенд; bff/frontend — образы из ghcr (override для локальной сборки)
└── .github/workflows/        ci.yml (PR: build + unit + api-tests), deploy.yml (main → ghcr → k3s)
```

### 2.2 `IVR-CBDC/Frontend` — новый репозиторий из `alfa-cbdc-hub`

```
Frontend/
├── frontend/   React SPA → образ nginx со статикой
├── bff/        бывший backend/ — моки удалены, клиенты к auth/core/commission, Redis → WS
├── e2e/        Playwright
└── .github/workflows/ci.yml   typecheck, vitest, build, e2e (checkout Backend + compose), push в ghcr,
                              repository_dispatch → Backend deploy
```

### 2.3 `IVR-CBDC/commission-service`

История переносится subtree в `Backend/services/service-commission`, репозиторий архивируется
с README-ссылкой на Backend. Создание/архивирование репозиториев на GitHub выполняет владелец
организации (действия на внешнем сервисе); локально всё готовится заранее.

## 3. Архитектура

```
Browser ──> Traefik ──┬─ /api/*, /ws ──> bff (Node, Express, ws)
                      └─ /*          ──> frontend (nginx, SPA)

bff ──HTTP, Bearer JWT──> service-auth        /api/auth/*       (выпуск JWT)
    ──HTTP, Bearer JWT──> service-core        /api/core/*       (сделки)
    ──HTTP, Bearer JWT──> service-commission  /api/commission/* (котировки)
    <─Redis SUBSCRIBE──── deal-events:{company_id}

service-core ──HTTP, Bearer JWT──> service-commission   (только при подтверждении сценария)
service-core ──outbox → Redis PUBLISH
```

Наружу через Traefik публикуются **только** `frontend` и `bff`. auth/core/commission доступны
лишь внутри сети (compose-сеть / NetworkPolicy в k3s). Прямые пути `/api/auth`, `/api/core`
из текущего `dynamic.yml` убираются.

### 3.1 Аутентификация

- BFF проксирует `POST /api/auth/register|login` в service-auth, кладёт JWT в cookie
  `session` (`HttpOnly`, `SameSite=Strict`, `Secure` при `COOKIE_SECURE=true`, `Max-Age` = TTL токена).
  Токен в JS не попадает → XSS не крадёт сессию.
- `POST /api/auth/logout` — стирает cookie. `GET /api/auth/me` — данные из service-auth `/me`.
- BFF на каждый запрос читает cookie и передаёт `Authorization: Bearer <jwt>` в upstream.
  BFF подпись не проверяет — это делают сервисы (единственный источник правды), BFF лишь
  отвечает 401, если cookie нет.
- WS `/ws`: при upgrade BFF верифицирует JWT из cookie публичным ключом (`jose`), чтобы знать
  `company_id` для подписки. Без валидного токена — `close(4401)`.
- CSRF: `SameSite=Strict` + проверка `Origin` на мутирующих запросах в BFF.

### 3.2 Изменения service-auth

- Миграция `002_company.sql`: таблица `companies(id UUID PK, name TEXT NOT NULL, inn TEXT UNIQUE
  NOT NULL CHECK (inn ~ '^[0-9]{10}$'), created_at)`, `users.company_id UUID REFERENCES companies`.
- `register` принимает `{login, password, name, company_name, inn}`. Если компания с таким ИНН
  есть — пользователь присоединяется к ней, иначе создаётся. Всё в одной транзакции.
- JWT получает claim `company_id`. `common::Claims` расширяется полем `company_id`;
  `JwtFilter` кладёт в attributes `user_id` и `company_id`.
- `/me` реализуется через `common::JwtVerifier` (закрывает TODO из README): возвращает
  `{user_id, login, name, company: {id, name, inn}}`.

**Известное допущение (не продакшн-изоляция).** Регистрация открытая, а ИНН — публичные данные
(ЕГРЮЛ), поэтому любой, кто знает ИНН, присоединяется к чужой компании и видит её сделки.
`company_id` — единственная граница арендатора во всей системе (§4), так что в реальной
эксплуатации вход в существующую компанию должен идти через инвайт или подтверждение
администратором. Для ИВР осознанно оставлено как есть: решение зафиксировано 2026-09-16,
план 03 строит домен поверх этого допущения.

## 4. service-core — домен сделки

### 4.1 Схема pg-core (миграции `001..`, старый `_placeholder` удаляется миграцией)

```sql
document_kinds (kind TEXT PK, title TEXT, purpose TEXT, required_for TEXT[])  -- справочник, seed
deals (
  id UUID PK, company_id UUID NOT NULL, display_no BIGINT GENERATED ALWAYS AS IDENTITY,
  counterparty_country CHAR(2) NOT NULL, counterparty_name TEXT NOT NULL,
  operation_type TEXT NOT NULL CHECK (operation_type IN ('import','export')),
  amount NUMERIC(18,2) NOT NULL CHECK (amount > 0), currency CHAR(3) NOT NULL,
  scenario TEXT NULL CHECK (scenario IN ('cbdc','bank_transfer','smart_contract','trade_finance')),
  commission_total NUMERIC(18,2) NULL, commission_breakdown JSONB NULL,
  stage TEXT NOT NULL CHECK (stage IN ('created','documents','compliance_check','settlement','completed','blocked')),
  blocked_from TEXT NULL, blocker_reason TEXT NULL,
  next_action_at TIMESTAMPTZ NULL,           -- когда эмулятору трогать сделку
  version INT NOT NULL DEFAULT 0,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(), updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
)
deal_documents (id UUID PK, deal_id UUID FK ON DELETE CASCADE, kind TEXT FK,
  status TEXT CHECK (status IN ('missing','uploaded','under_review','approved','rejected')),
  reject_reason TEXT NULL, next_action_at TIMESTAMPTZ NULL, updated_at TIMESTAMPTZ)
timeline_events (id UUID PK, deal_id UUID FK, seq INT, step TEXT, actor TEXT,
  status TEXT CHECK (status IN ('pending','in_progress','done','delayed')),
  delay_reason TEXT NULL, started_at TIMESTAMPTZ NULL, finished_at TIMESTAMPTZ NULL,
  UNIQUE (deal_id, seq))
notifications (id UUID PK, company_id UUID, deal_id UUID NULL, severity TEXT
  CHECK (severity IN ('info','warning','critical')), message TEXT, read_at TIMESTAMPTZ NULL, created_at)
outbox (id BIGINT GENERATED ALWAYS AS IDENTITY PK, topic TEXT, payload JSONB,
  created_at TIMESTAMPTZ DEFAULT now(), published_at TIMESTAMPTZ NULL)
```

Индексы: `deals(company_id, updated_at DESC)`, `deals(next_action_at) WHERE stage NOT IN ('completed')`,
`deal_documents(next_action_at) WHERE status = 'under_review' OR status = 'uploaded'`,
`notifications(company_id, created_at DESC)`, `outbox(id) WHERE published_at IS NULL`.

`display_id` формируется при чтении: `DEAL-<год created_at>-<display_no, 4 цифры>`.

### 4.2 Жизненный цикл

```
created ──confirm scenario──> documents ──все обязательные approved──> compliance_check
                                                                            │ emulator
                                                                            ▼
                                                    settlement ──emulator──> completed
любая из {documents, compliance_check, settlement} ──отказ──> blocked
blocked ──устранение (переподача документа / повтор проверки)──> blocked_from
```

- Вся логика переходов — `DealStateMachine` (header+cc без Drogon/БД): функции вида
  `Result<Transition> onScenarioConfirmed(const DealSnapshot&)`, `onDocumentApproved`,
  `onDocumentRejected`, `onComplianceResult`, `onSettlementDone`, `onDocumentResubmitted`.
  Возвращает новую стадию + изменения таймлайна + уведомления, либо ошибку `INVALID_TRANSITION`.
  Репозиторий применяет результат в одной транзакции.
- Таймлайн создаётся при создании сделки: 1 «Сделка создана» (Вы) → 2 «Выбор сценария» (Вы) →
  3 «Документы» (Вы) → 4 «Комплаенс-проверка» (Банк) → 5 «Проверка ФНС» (ФНС) →
  6 «Расчёт: <сценарий>» (Банк / Платформа ЦВЦБ / Смарт-контракт / Банк-гарант) →
  7 «Завершение сделки» (Система).
- Обязательные документы создаются при подтверждении сценария из `document_kinds.required_for`:
  все — «Контракт», «Инвойс»; `bank_transfer`, `trade_finance` — «Паспорт сделки (УНК)»;
  `trade_finance` — «Заявление на аккредитив»; `smart_contract` — «Подтверждение ЭЦП сторон».
- Пользователь может менять статус документа только `missing|rejected → uploaded`.
  `under_review/approved/rejected` ставит только эмулятор.
- Прогресс для дашборда: `done / total * 100` по таймлайну, считается в core.
- Смена сценария разрешена только в `created` и `documents` (до подачи хотя бы одного документа).

### 4.3 Эмулятор внешних систем

Внутри service-core, `app().getLoop()->runEvery(EMULATOR_TICK_SEC)`, отключается
`EMULATOR_ENABLED=false` (в api-тестах управляется явно).

- Выбор работы: `SELECT … WHERE next_action_at <= now() … FOR UPDATE SKIP LOCKED LIMIT 50` —
  безопасно при нескольких репликах.
- Задержки: `EMULATOR_SPEED` = `demo` (секунды) | `realistic` (минуты). Исходы детерминированы
  хэшем `(deal_id, step)`: одна и та же сделка ведёт себя одинаково → воспроизводимые тесты и демо.
- Правила: документ `uploaded → under_review → approved` (10% — `rejected`, «Скан нечитаем» /
  «Не совпадает сумма с инвойсом»); `compliance_check` → 20% `delayed` «Ожидаем ответ от ФНС» →
  `done`; `settlement → completed`.
- Каждое действие = транзакция: изменения + уведомление + запись в `outbox`.
- Для тестов: при `EMULATOR_MANUAL=true` фоновый цикл не запускается, а ручка
  `POST /internal/emulator/tick` выполняет один проход немедленно (`Emulator::tickOnce()`,
  см. `services/service-core/src/core/emulator_tick.cc`). Без этого флага ручка не
  регистрируется. Наружу не публикуется (нет в ingress).
  **F4 (план 07, final review):** ручка не принимает `advance_sec` — параметр не парсится,
  «виртуальное время» не сдвигается, тик продвигает только те переходы, у которых
  `next_action_at` уже наступил по настоящим часам. Из-за этого детерминированные тесты
  (например, `Frontend/e2e/recovery.spec.ts`) всё равно платят реальными секундами ожидания
  между `uploaded → under_review → approved/rejected`. Параметризованный сдвиг времени —
  требование плана 08, не реализовано здесь.

### 4.4 События (transactional outbox)

- Паблишер: `runEvery(0.5s)`, берёт неопубликованные строки `ORDER BY id FOR UPDATE SKIP LOCKED`,
  `PUBLISH deal-events:{company_id} <payload>` через `drogon::nosql::RedisClient`, ставит `published_at`.
- Формат: `{"seq": <outbox.id>, "type": "deal.created"|"deal.updated"|"notification.created",
  "deal_id": "...", "notification_id": "...", "at": "<iso>"}`. `at` — `outbox.created_at`
  (ISO-8601 UTC) той строки, то есть момент записи в outbox, а не момент публикации.
- Доставка at-least-once; `seq` (= `outbox.id`) — уникальный id доставки для дедупликации
  повторов, **не гарантия порядка**: `id` назначается при INSERT, но строка видна паблишеру
  только после COMMIT своей транзакции, а две параллельные `DealRepository::apply()` могут
  закоммититься в любом порядке — потребитель способен увидеть больший `seq` раньше меньшего.
  Это не страшно: событие несёт только триггер перезапроса (данные не в payload), клиент
  идемпотентен. BFF дедуплицирует по множеству уже виденных `seq`, а **не** по отбрасыванию
  `seq` ≤ последнего отправленного — такое сравнение потеряло бы легитимное, ещё не виденное
  событие с меньшим `seq`.
- Redis pub/sub не хранит историю: `PUBLISH` доставляет только тем, кто подписан в этот момент.
  BFF, переподключившийся к каналу (рестарт, разрыв соединения), не получит события, пропущенные
  за время простоя — он должен перезапросить список сделок у service-core, а не полагаться на то,
  что канал «дошлёт» пропущенное.

### 4.5 API service-core (внутренний, все ручки под `JwtFilter`, фильтрация по `company_id`)

| Метод | Путь | Тело / ответ |
|---|---|---|
| GET | `/health` | `{ok, service, version, postgres_ok, redis_ok}` (без JWT) |
| GET | `/api/core/deals?limit=` | `{items: DealSummary[], count}` |
| POST | `/api/core/deals` | `CreateDeal` → `201 Deal` |
| GET | `/api/core/deals/{id}` | `Deal` (с documents, timeline) |
| POST | `/api/core/deals/{id}/scenario` | `{scenario, version}` → `Deal` (вызывает commission) |
| POST | `/api/core/deals/{id}/documents/{docId}/submit` | `{version}` → `Deal` |
| GET | `/api/core/notifications?limit=` | `{items, unread}` |
| POST | `/api/core/notifications/{id}/read` | `204` |

Чужая сделка → `404` (не `403`, чтобы не раскрывать существование). Несовпадение `version` →
`409 VERSION_CONFLICT`. Полный контракт — `docs/openapi/core.yml`.

Курсорная пагинация (`?cursor=` → `next_cursor`) сознательно не реализована в плане 03: список
сделок компании на демо-объёмах — единицы-десятки записей, `?limit=` без курсора закрывает эту
потребность. Добавить курсоры, когда UX дашборда (план 06) определит размер страницы —
см. дорожную карту.

## 5. service-commission

- Перенос `ivrpy` → `services/service-commission/app/` (пакет), `pandas` и CSV убираются.
- pg-commission, SQL-миграции тем же `migrate.sh`, что у C++-сервисов (одна механика миграций во
  всей системе, одинаковый helm init-container):
  `corridors(corridor_id PK, from_country, to_country, currency, base_fee, percentage_fee,
  fixed_fee, min_limit, max_limit, min_fee, max_fee)` — seed из текущего CSV + коридоры для RUB/USD/CNY/AED/INR/TRY
  в обе стороны для CN, AE, IN, TR, KZ, BY;
  `scenario_profiles(scenario PK, title, description, fee_multiplier NUMERIC, eta_label,
  limitations TEXT[], min_amount NUMERIC NULL, max_amount NUMERIC NULL, countries CHAR(2)[] NULL)`.
- Деньги — `Decimal`, округление `ROUND_HALF_UP` до 2 знаков (float уходит).
- JWT-проверка — зависимость `require_user` из текущего service-test-python.
- Направление: клиент — российское юрлицо. `import` → `from=RU, to=counterparty`;
  `export` → `from=counterparty, to=RU`; валюта — валюта сделки.

API:

| Метод | Путь | |
|---|---|---|
| GET | `/health` | `{ok, postgres_ok}` |
| POST | `/api/commission/calculate` | совместимый с ivrpy ответ (коды ошибок сохраняются) |
| POST | `/api/commission/quotes` | `{from_country, to_country, currency, amount}` → `{corridor_id, quotes: Quote[4]}` |

`Quote = {scenario, title, description, eta_label, limitations[], available: bool,
unavailable_reason: str|null, commission: {base_fee, percentage_fee, percentage_amount,
fixed_fee, subtotal, clamped, multiplier, total} | null}` (`subtotal` — сумма после
клампа min/max fee, но до умножения на multiplier сценария; `clamped` — `"min"` / `"max"` /
`null`, если сработал клампинг). Нет коридора → все 4 `available=false` с причиной
«Коридор RU→US в USD не поддерживается», а не 404 — экран всё равно рисуется.

## 6. BFF (`Frontend/bff`)

- Моки и `node-cache`-состояние удаляются. Модули: `config.ts` (env через zod), `upstream/`
  (`authClient`, `coreClient`, `commissionClient` на `fetch` с таймаутом 5s, пробросом
  `x-request-id`), `routes/` (по экранам), `ws/` (Redis subscriber → сокеты компании), `errors.ts`.
- Контракт для SPA сохраняется в стиле текущего `/api/*` (минимум переделок фронта):
  `/api/dashboard` (агрегат `deals` + `unread`), `/api/deals`, `/api/deals/:id`,
  `/api/deals/:id/scenarios` (**новое**: котировки из commission под параметры сделки),
  `/api/deals/:id/scenario`, `/api/deals/:id/documents/:docId/submit`, `/api/deals/:id/tracking`,
  `/api/notifications`, `/api/auth/*`.
- Кеш: только `GET /api/deals/:id/scenarios` (ключ — параметры сделки, TTL 60s). Дашборд не
  кешируется в памяти процесса — при 2 репликах он разъезжается; инвалидация через WS-события.
- WS: одна Redis-подписка на процесс `PSUBSCRIBE deal-events:*`, роутинг по `company_id` в
  `Map<companyId, Set<WebSocket>>`, heartbeat 25s (как сейчас), дедуп по `seq`.
- Ошибки upstream → `{error: <русское сообщение>, code: <CODE>}`, статус сохраняется;
  таймаут/недоступность → `503 {code: "UPSTREAM_UNAVAILABLE"}`. Тексты кодов — в одном словаре.
- Fallback-статусы из заявки («Ожидаем ответ от ФНС») берутся из `delay_reason` таймлайна core.

## 7. Frontend SPA

Минимально необходимые изменения, UI Мари сохраняется:

- `LoginPage` / `RegisterPage` (с полями компании и ИНН, валидация 10 цифр), `AuthProvider`
  (`/api/auth/me`), защищённые маршруты, выход в `Header`.
- `api/client.ts` → `credentials: "same-origin"`, обработка 401 → редирект на логин, код ошибки в `ApiError`.
- `ScenarioPage` — карточки из `/api/deals/:id/scenarios`: реальная сумма комиссии, недоступные
  сценарии серые с причиной.
- `DocumentsPage` — кнопка «Отправить документ» вместо произвольной смены статуса; причина отказа.
- `CreateDealWizard` — страны ISO-кодами из общего справочника `constants/countries.ts`,
  черновик в `localStorage` (заявлено в описании, отсутствует в коде).
- `Timeline` — `delay_reason`, участники; `useWebSocket` — переподключение с backoff.
- Типы приводятся к контракту BFF (`types/api.ts`).

## 8. Инфраструктура и деплой

- **compose (Backend):** traefik, pg-auth, pg-core, pg-commission, redis, migrate-*, service-auth,
  service-core, service-commission, bff, frontend. `docker-compose.override.yml.example` —
  сборка bff/frontend из соседнего каталога `../Frontend`.
- **Traefik:** `/api`, `/ws` → bff; `/` → frontend (низкий приоритет).
- **Helm:** `values-service-commission.yaml`, `values-bff.yaml`, `values-frontend.yaml`;
  ingress только у bff и frontend; NetworkPolicy пускает к core/auth/commission только bff
  (и core → commission). Пароли БД → `Secret` (сейчас открытым текстом в values).
  Миграции в values генерирует `infra/gen-migration-values.sh` из `services/*/migrations` —
  убирает ручное дублирование SQL в values. TCP-DNS обход в init-контейнере сохраняется.
- **Readiness/liveness:** `/health` каждого сервиса; bff проверяет Redis.
- **CI Backend (PR):** сборка образов (кэш buildx), Catch2, pytest commission, compose up +
  `tests/api` (pytest + httpx), shellcheck на infra-скрипты.
- **CD Backend (main):** build+push всех backend-образов с тегом `sha` → `helm upgrade` для
  auth/core/commission/bff/frontend. Теги bff/frontend берутся из `infra/helm/frontend-tags.env`,
  который обновляет workflow Frontend через `repository_dispatch` (коммит бота в Backend) —
  деплой всегда из одного места и воспроизводим по git.
- **CI Frontend:** `pnpm typecheck`, `vitest` (bff + frontend), `vite build`, e2e: checkout Backend,
  `docker compose up` с локально собранными bff/frontend, `playwright test`; на `main` — push
  образов + dispatch.

## 9. Тестирование

| Уровень | Где | Что покрывает |
|---|---|---|
| Unit C++ (Catch2) | `service-core/tests` | `DealStateMachine` (все переходы и запреты), прогресс, display_id, детерминизм эмулятора |
| Unit C++ (Catch2) | `libs/common/tests` | `JwtVerifier`: валидный, чужая подпись, истёкший, без `company_id` |
| Unit Python (pytest) | `service-commission/tests` | формула, min/max fee, Decimal-округление, направление import/export, недоступные сценарии |
| API (pytest+httpx) | `Backend/tests/api` | регистрация/логин/me, изоляция компаний (чужая сделка 404), жизненный цикл сделки с `emulator/tick`, 409 по version, outbox → Redis событие, миграции на пустой БД |
| Unit/integration TS (vitest) | `bff` | маппинг ошибок, cookie-сессия, дедуп WS по seq, клиенты upstream (mock `undici`) |
| Component (vitest + Testing Library) | `frontend` | валидация мастера, черновик, карточки сценариев (недоступный) |
| E2E (Playwright) | `Frontend/e2e` | критерий готовности из §1 + логин с неверным паролем |

## 10. ADR (`Backend/docs/adr`)

1. `0001-bff-single-entrypoint` — наружу только bff+frontend; cookie-сессия.
2. `0002-core-calls-commission` — исключение из «no inter-service sync calls»: авторитетный
   пересчёт комиссии на сервере при подтверждении сценария.
3. `0003-transactional-outbox` — гарантированная доставка и порядок событий.
4. `0004-external-systems-emulator` — детерминированный эмулятор вместо интеграций.
5. `0005-sql-migrations-everywhere` — один `migrate.sh` для C++ и Python.

## 11. Порядок работ (этапы для плана)

0. Гигиена: ветка, `git init` Frontend, subtree commission, удаление test-python.
1. service-commission: пакет, БД+миграции, quotes, JWT, pytest.
2. libs/common + service-auth: компании, `company_id`, `/me`.
3. service-core: миграции, `DealStateMachine`+Catch2, репозиторий, ручки, outbox, эмулятор.
4. compose + traefik + `tests/api`.
5. BFF: сессия, клиенты, роуты, WS через Redis, vitest.
6. Frontend: auth, контракт, сценарии, документы, трекинг, vitest.
7. Playwright e2e.
8. Helm/NetworkPolicy/Secrets, CI/CD обоих репо, README + ADR.

Этапы 1–3 независимы по коду и могут идти параллельно; 4 требует 1–3; 5 требует 4; 6 — 5; 7 — 6.

## 12. Риски

- **Сборка C++ в CI долгая** (drogon/libjwt из исходников) → базовый образ `ivr-cpp-base` в ghcr,
  собирается отдельно при изменении Dockerfile.base.
- **DNS в k3s через VPN** — существующий обход сохраняется, новые сервисы используют тот же chart.
- **Расхождение заявки и кода** (заявлены e2e, виртуализация таймлайна, JSON Server+faker,
  8–10k строк) — e2e и черновики реализуются в рамках работ; виртуализация таймлайна не нужна при
  7 шагах и не делается; текст заявки нужно согласовать с фактическим объёмом.
