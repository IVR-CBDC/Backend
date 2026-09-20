# backend-platform

Микросервисная платформа Alfa Global CBDC Hub: сервисы за общим ingress, каждый со своей БД, объединённые через JWT.

## Архитектура

```
                            ┌──────────────────────────────┐
[Browser]  ──HTTP──>        │  Traefik (Ingress)           │
                            │  наружу только frontend и bff │
                            └──┬───────────────────┬────────┘
                    /api, /ws ─┘                   └─ /  ──> frontend (nginx, SPA)
                       │
                  ┌────▼─────┐
                  │   bff    │  Node/Express — единственная дверь для SPA
                  └────┬─────┘
                  │                      │                      │
        ┌─────────▼────────┐  ┌──────────▼──────────┐  ┌────────▼──────────┐
        │  service-auth    │  │  service-core       │  │  service-commission │
        │  C++ / Drogon    │  │  C++ / Drogon       │  │  Python/FastAPI   │
        │  пишет: ты       │  │  домен сделок       │  │  комиссии/котировки │
        │  pg-auth         │  │  pg-core            │  │  pg-commission      │
        │  ВЫПУСКАЕТ JWT   │  │  ВАЛИДИРУЕТ JWT     │  │  ВАЛИДИРУЕТ JWT   │
        │  (приват. ключ)  │  │  (публ. ключ)       │  │  (публ. ключ)     │
        └──────────────────┘  └─────────────────────┘  └───────────────────┘
```

### Принципы

1. **Database per Service** — каждый сервис владеет своими данными.
2. **No inter-service sync calls** — сервисы не зовут друг друга по сети ради CRUD.
3. **JWT с RS256** — auth выпускает приватным ключом, остальные валидируют публичным локально (без сети).
4. **Traefik как единая точка входа** — фронт ходит в `localhost`, не знает про внутреннее устройство.
5. **Пользователь = сотрудник юрлица** — `company_id` едет в JWT, поэтому core и
   commission разделяют данные по компании, не обращаясь в auth по сети.

## Quick start

```bash
make keys      # генерит RSA-пару (один раз)
make up        # собирает и запускает всё
```

Регистрация и логин (через BFF — прямых путей `/api/auth`, `/api/core` за
Traefik больше нет, спека §3). Нужен поднятый профиль `bff`:

```bash
docker compose --profile bff up -d --wait bff   # или make e2e-stand-up

make test-register  # логин уникален на каждый прогон, сохраняется в .smoke-user.txt
make test-login     # токен уезжает в HttpOnly-cookie, не в тело ответа
make test-me        # ходит по сохранённой cookie (.smoke-cookies.txt)

# Целям, которые ходят в сервисы мимо BFF по внутренней сети, Bearer всё
# ещё нужен — токен достаётся из той же cookie:
export TOKEN=$(make -s test-token)
make smoke-commission
make smoke-deal       # сквозная сделка: создать → сценарий → документы → эмулятор → completed
make test-api         # pytest против поднятого стенда (EMULATOR_MANUAL=true)
```

`make test-health` (`/health` сервисов и `/ready` BFF) стоит особняком: он
бьёт в host-порты **18080, 18081 и 14000, которые публикует только
`docker-compose.dev.yml`**. После обычного `make up` их нет и цель честно
упадёт с «Failed to connect». Поднимайте стенд с оверлеем:

```bash
make e2e-stand-up   # или docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d --wait
make test-health
```

Все smoke-цели возвращают ненулевой код на неожиданном HTTP-статусе
(`infra/http-json.sh`) — зелёный `make test-me` действительно означает 200,
а не «401 проскочил через `| jq`».

`make up` наружу отдаёт только traefik (порт 80) — так локальная топология
совпадает с прод. С плана 08 дашборд Traefik **выключен по умолчанию**
(`api.insecure: false`, порт 8080 контейнера наружу не публикуется): сам
ingress-контроллер не входит в список того, что торчит наружу (спека §3).
Для демонстрации он включается отдельным оверлеем:

```bash
docker compose -f docker-compose.yml -f docker-compose.dashboard.yml up -d traefik
# http://127.0.0.1:8081/dashboard/  (порт занят? TRAEFIK_DASHBOARD_PORT=18082 ...)
```

Host-порты сервисов и БД (18080,
18081, 5433–5435, для тестов и ручной отладки) публикует только оверлей
`docker-compose.dev.yml`, который `make test-api` и `make test-commission-db`
подключают автоматически:

```bash
docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d --wait
```

**Явный `-f` отменяет автоподхват `docker-compose.override.yml`.** Любая
команда с явным `-f docker-compose.yml ...` (как выше, в Makefile, в
`.github/workflows/ci.yml`) не подхватывает `docker-compose.override.yml`
автоматически — только файлы, перечисленные через `-f`. Мы намеренно
оставили `docker-compose.dev.yml` под своим именем, а не переименовали в
`docker-compose.override.yml`: если бы это имя подхватывалось автоматически,
`make up` (без `-f`) начал бы публиковать host-порты сервисов и БД сам того
не показывая — а это ломает принцип «наружу только traefik» (см. выше).
Explicit лучше implicit здесь: все `-f`-цепочки перечисляют нужные им
оверлеи по имени, и это единственный способ узнать, что реально подключено,
не заглядывая в поведение docker compose по умолчанию.
**Правило для плана 05**: когда появится
`docker-compose.override.yml.example` (сборка bff/frontend из `../Frontend`,
спека §8) — он останется опциональным и не должен предполагаться подключённым
неявно. Любая цель/джоба, которой действительно нужны bff/frontend, обязана
добавить его в свою `-f`-цепочку явно (`-f docker-compose.override.yml`),
иначе получится то же молчаливое расхождение, которого мы здесь избегаем:
`make up` видит сервис, а `make test-api`/CI — нет.

## BFF (план 05)

Стенд умеет поднимать сервис `bff` — единственную дверь для SPA из
Frontend-репозитория (`IVR-CBDC/Frontend`, локально `../alfa-cbdc-hub`).
С плана 08 Traefik маршрутизирует на него `/api` и `/ws` (роутер `bff`,
`priority: 100`), а прямых маршрутов `/api/auth` и `/api/core` к сервисам
больше нет — наружу торчат только `frontend` и `bff` (спека §3).
`bff` по-прежнему под профилем `bff`, поэтому обычный `make up` его не
поднимает; Traefik от этого не падает — недоступный upstream даёт 502 на
запрос, а не ошибку старта.

По умолчанию `docker-compose.yml` ссылается на
`${BFF_IMAGE:-ghcr.io/ivr-cbdc/frontend/bff:latest}` — это правильный
namespace по спеке §9 (образ bff собирает и публикует CI
**Frontend**-репозитория, тег приезжает в `infra/helm/frontend-tags.env`),
но **этот образ пока никто не публиковал** — ни туда, ни в старый
`backend/bff`. Поэтому `bff` объявлен с `profiles: ["bff"]`: обычный
`make up` (без списка сервисов, т.е. `docker compose up --build -d`) его
не трогает и не пытается тянуть несуществующий образ — свежий клон
поднимается (F1, final review плана 05). Чтобы поднять именно `bff`,
называй его явно — явное указание сервиса в команде поднимает его,
несмотря на `profiles` (задокументированное поведение `docker compose`):

```bash
BFF_IMAGE=<готовый образ> \
docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d --wait bff
curl -s http://127.0.0.1:14000/ready    # порт открыт только dev-оверлеем
```

Для локальной сборки образа из соседнего checkout Frontend-репозитория
используй `docker-compose.override.yml.example`:

```bash
cd ../alfa-cbdc-hub && docker build -t bff-local:dev -f bff/Dockerfile bff
cd -
cp docker-compose.override.yml.example docker-compose.override.yml
docker compose -f docker-compose.yml -f docker-compose.dev.yml \
  -f docker-compose.override.yml up -d --wait bff
```

Напоминание из правила выше: `docker-compose.override.yml` не подхватывается
неявно, пока `-f`-цепочка уже перечисляет файлы явно — его нужно дописать в
команду самому, `cp` рядом с именем по умолчанию недостаточно.

`bff` ждёт `service_healthy` от auth/core/commission/redis и сам публикует
healthcheck по `/ready` (`{"ok":true,...}`, F10 final review — `/health` теперь
только живость процесса, без сети; `/ready` — настоящая готовность: Redis-
подписка в моменте плюс достижимость auth/core/commission) — `--wait` не
сочтёт стенд готовым, пока BFF не сможет говорить со всеми апстримами. SPA
(`frontend/`) в стенд пока не входит — это план 06.

В k3s это же разделение разнесено по пробам (`infra/helm/values-bff.yaml`):
`livenessProbe` → `/health`, `readinessProbe` → `/ready`. Порядок важен: с
liveness на `/ready` недоступный auth перезапускал бы совершенно здоровый
BFF по кругу — ровно то, ради чего ручки и разделяли.

CI Backend-репозитория (`.github/workflows/ci.yml`, джоба `api`) сознательно
**не** поднимает `bff`: у него нет ни исходников bff (они в
Frontend-репозитории), ни доступа к опубликованному образу (см. абзац выше)
— добавление `bff` в список `up --wait` сейчас лишь заменило бы одну гарантированную
поломку (`make up`) на другую (CI). Это станет возможным, когда
Frontend-репозиторий будет запушен под `IVR-CBDC/Frontend` и его CI начнёт
публиковать образ — тогда backend CI сможет либо тянуть его, либо
чекаутить Frontend-репозиторий рядом и собирать оттуда, как это уже делает
`docker-compose.override.yml.example` локально.

## Frontend SPA (план 06)

Стенд умеет поднимать сервис `frontend` — nginx со статикой SPA из
Frontend-репозитория (`IVR-CBDC/Frontend`, локально `../alfa-cbdc-hub`),
проксирующий `/api` и `/ws` на `bff`. Тот же паттерн, что у `bff` выше:
образ ещё не опубликован, сервис под своим профилем. С плана 08 Traefik
отдаёт ему `/` (роутер `frontend`, `priority: 1` — явно ниже `bff`, чтобы
`/api` не перехватывался). Образ `nginxinc/nginx-unprivileged` слушает
8080, поэтому upstream в `infra/traefik/dynamic.yml` — `http://frontend:8080`.

По умолчанию `docker-compose.yml` ссылается на
`${FRONTEND_IMAGE:-ghcr.io/ivr-cbdc/frontend/spa:latest}` — этот образ CI
Frontend-репозитория пока не публиковал, поэтому `frontend` объявлен с
`profiles: ["frontend"]`: обычный `make up` его не трогает. Чтобы поднять
именно `frontend`, называй его явно:

```bash
FRONTEND_IMAGE=<готовый образ> \
docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d --wait frontend
```

Для локальной сборки образа из соседнего checkout Frontend-репозитория:

```bash
cd ../alfa-cbdc-hub && docker build -t frontend-local:dev -f frontend/Dockerfile frontend
cd -
docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d --wait bff frontend
```

(либо через `docker-compose.override.yml` по аналогии с `bff` — см. раздел
выше, добавь в него свой блок `frontend`, указав `image: frontend-local:dev`
собранного выше образа).

`frontend` ждёт `service_healthy` от `bff` и сам публикует healthcheck по
`/` (проверяем, что nginx реально отдаёт HTML страницу) — `--wait` не
сочтёт стенд готовым, пока nginx не поднялся. С хоста SPA доступен только
через порт, проброшенный dev-оверлеем:

```bash
docker compose -f docker-compose.yml -f docker-compose.dev.yml \
  --profile bff --profile frontend up -d --wait
# http://127.0.0.1:8090 — порт открыт только docker-compose.dev.yml
```

## Стенд под e2e (план 07, задача 3)

Playwright-сюит Frontend-репозитория (`../alfa-cbdc-hub/e2e`) гоняется против
уже поднятого стенда — SPA (`127.0.0.1:8090`), BFF (`127.0.0.1:14000`) и
`service-core` в ручном режиме эмулятора. Одна команда на подъём:

```bash
make e2e-stand-up
```

и одна на остановку профилей `bff`/`frontend` (остальной стенд остаётся —
он может быть нужен для чего-то ещё):

```bash
make e2e-stand-down
```

**`EMULATOR_MANUAL=true` останавливает фоновое автопродвижение сделок**
(см. «Домен сделки» ниже) — для e2e это то, что нужно (детерминированные,
управляемые тестом переходы через `POST /internal/emulator/tick`), но **не
то**, что нужно для ручной работы со стендом (посмотреть, как сделка сама
проходит комплаенс/расчёт вживую, без ручного тика). Если тебе нужен именно
живой автопрогресс — не используй `make e2e-stand-up`, подними стенд как
обычно (`make up`, при необходимости добавив `--profile bff --profile
frontend` без `EMULATOR_MANUAL`).

`make e2e-stand-up` — единственный поддерживаемый способ поднять стенд под
e2e: `EMULATOR_MANUAL=true` зашита в саму цель Makefile, а не полагается на
переменную окружения того, кто её вызывает. Раньше эта команда набиралась
руками, и повторный `up --profile bff --profile frontend ...` без префикса
`EMULATOR_MANUAL=true` (например, чтобы просто пересобрать/поднять
bff/frontend после правки) пересоздавал `service-core` со значением по
умолчанию (`false`) — тихо и без предупреждения ломая детерминированность
уже идущих тестов. Подробности — в комментарии над целью в `Makefile`.

Образы `bff`/`frontend` по умолчанию берутся из ghcr (см. разделы «BFF» и
«Frontend SPA» выше) — если их там ещё нет (CI Frontend-репозитория их пока
не публикует), передай уже собранные локально теги:

```bash
BFF_IMAGE=bff-ci:latest FRONTEND_IMAGE=frontend-ci:latest make e2e-stand-up
```

**F2 (план 07, final review) — порядок мержа:** цель `e2e-stand-up` живёт
только в ветке `feat/e2e-support` этого репозитория, не в его ветке по
умолчанию. Frontend-репозиторий (`.github/workflows/ci.yml`, джоба `e2e`)
чекаутит Backend без `ref:`, то есть его default branch — эта джоба не
сможет пройти, пока `feat/e2e-support` не будет влита. Мерж Backend-ветки —
обязательное условие ДО того, как e2e-джоба Frontend CI сможет стать
зелёной, не наоборот.

**F8 (план 07, final review):** `make e2e-stand-down` не возвращает
`service-core` в автоматический режим эмулятора — см. предупреждение в
выводе самой цели и комментарий над ней в `Makefile`.

## Структура

```
backend-platform/
├── docker-compose.yml                # 3 сервиса + 3 БД + Redis + Traefik
├── docker-compose.dev.yml            # оверлей: host-порты сервисов и БД (только для тестов/отладки)
├── Makefile
├── infra/
│   ├── gen-keys.sh                   # ./gen-keys.sh — RSA для JWT
│   ├── keys/                         # сгенерированные ключи (gitignore)
│   ├── traefik/traefik.yml
│   ├── helm/generic-service/         # один чарт на все пять релизов
│   ├── helm/values-*.yaml            # по релизу на файл
│   └── services.tsv                  # список сервисов и ПОРЯДОК выката (Makefile + CD)
├── services/
│   ├── service-auth/                 # C++/Drogon, выпускает JWT
│   ├── service-core/                 # C++/Drogon, валидирует JWT
│   └── service-commission/           # Python/FastAPI, комиссии и котировки, валидирует JWT
└── docs/
    └── jwt-contract.md               # формат JWT — единственный общий контракт
```

## Как добавлять новые ручки

### В service-auth (C++)

1. В `include/auth_controller.h`: добавляешь метод и `ADD_METHOD_TO`.
2. В `src/auth_controller.cc`: реализация через `Task<>` и `co_await db->execSqlCoro(...)`.

### В service-core (C++)

То же что в auth, но с `"common::JwtFilter"` в `ADD_METHOD_TO` если нужна авторизация
(так у всех ручек домена сделок, кроме `/health` и внутренней `/internal/emulator/tick`).
`company_id` берёшь из `req->attributes()->get<std::string>("company_id")` — им же
фильтруются все выборки, чтобы чужая сделка выглядела как `404`, а не `403`. Ручки
разложены по одной ответственности на файл под `src/core/` (`deals.cc`, `scenario.cc`,
`documents.cc`, `notifications.cc`, ...), персистентность — через `DealRepository`
(`repository.h`/`repository.cc`), переходы состояний — через чистый `DealStateMachine`
(`state_machine.h`/`.cc`, без Drogon и БД, юнит-тестируется отдельно).

### В service-commission (Python)

Логика — чистые функции в `app/domain.py` (тесты в `tests/test_domain.py`), доступ к БД — `app/repository.py`,
ручки — `app/api.py` внутри `create_router`. Авторизация — зависимость `User` (JWT, возвращает `sub`).
Миграции — `migrations/NNN_*.sql`. Тесты: `make test-commission` и `make test-commission-db`.

## Ломающее изменение: компании (план 02)

Регистрация требует `company_name` и `inn` (10 цифр). Пользователи с одинаковым ИНН
попадают в одну компанию. В JWT добавлен claim `company_id`, и токены **без него
считаются невалидными** — выданные раньше токены нужно перевыпустить (повторный логин).
Ошибки C++-сервисов отдаются в формате `{"code": "...", "error": "..."}`.

**Допущение демо, а не продакшн-изоляция.** Регистрация открытая, а ИНН публичен
(ЕГРЮЛ) — значит, любой, кто его знает, попадёт в чужую компанию и увидит её сделки.
`company_id` при этом единственная граница между арендаторами. В реальной эксплуатации
вход в существующую компанию должен идти через инвайт или подтверждение администратором;
здесь это осознанно не сделано.

**Rolling deploy на k3s**: миграция `002_company.sql` делает `users.company_id`
`NOT NULL`. Пока старые поды service-auth ещё живы во время выката — их
`INSERT INTO users` не заполняет эту колонку, и регистрация на них начнёт
падать сразу после применения миграции. Поэтому миграцию и новый образ нужно
выкатывать вместе, одним шагом; короткий простой регистрации во время
переключения допустим.

Тесты C++ (Catch2, собираются только в workspace-сборке):

    make test-cpp

### Ранбук: `500` на регистрации, а следом `409 USER_EXISTS`

Ответ обеим C++-сервисам отдаётся только после подтверждённого `COMMIT`
(`libs/common/include/common/commit.h`). У Drogon нет публичного `commit()`,
поэтому подтверждение ловится через `setCommitCallback`, а на случай, когда
колбэк не приходит вообще, у ожидания есть свой таймаут —
`DB_COMMIT_TIMEOUT_SEC`, по умолчанию 30 секунд.

**Когда так бывает.** Postgres подтвердил `COMMIT` позже таймаута — то есть
база думала над коммитом дольше 30 секунд. На практике это перегруженный или
подвисший Postgres (долгий fsync, исчерпанный дисковый IO, заблокированный
чекпоинт), а не обычная работа.

**Как выглядит.** Клиент получает `500 INTERNAL_ERROR`, в логах сервиса —
`awaitCommit: commit-колбэк не пришёл за N s`. Но `COMMIT` доходит до базы
следом, уже после ответа, и **данные оказываются записаны вопреки `500`**.
Поэтому повторная регистрация теми же кредами получит `409 USER_EXISTS`, а
логин теми же кредами — успешные `200`. Это не рассогласование базы: строка
в `users` полноценная, просто клиенту про неё не сказали.

**Что делать.** Не заводить пользователя заново и не чистить `users` руками.
Попросить пользователя войти (`POST /api/auth/login`) — если логин проходит,
регистрация на самом деле состоялась и делать больше нечего. Если логин даёт
`401`, пользователя действительно нет: регистрацию можно повторять. Заодно
посмотреть, почему Postgres коммитил дольше 30 секунд — именно это и есть
настоящая проблема; сам таймаут крутить (`DB_COMMIT_TIMEOUT_SEC`) стоит
только осознанно, `<= 0` выключает его совсем и возвращает вечное ожидание.

## Домен сделки (план 03)

`service-core` владеет полным жизненным циклом сделки: создание, выбор сценария расчёта
(комиссия пересчитывается на сервере через `service-commission`, клиентское значение не
доверяется), подача документов, автоматическое продвижение через комплаенс и расчёт до
завершения. Вся логика переходов — чистый `DealStateMachine` (без Drogon/БД, Catch2), его
результат применяет `DealRepository::apply` одной транзакцией (стадия, документы, таймлайн,
уведомления, запись в outbox).

Жизненный цикл (спека §4.2):

```
created ──выбор сценария──> documents ──все обязательные документы approved──> compliance_check
                                                                                     │ эмулятор
                                                                                     ▼
                                                             settlement ──эмулятор──> completed
любая из {documents, compliance_check, settlement} ──отказ──> blocked
blocked ──устранение (переподача документа)──> возврат к стадии, откуда заблокировало
```

**Эмулятор внешних систем** — детерминированная замена банка/ФНС/расчётного рельса: тот же
`DealRepository::apply`, что и у пользовательских ручек, поэтому произведённые им
`deal.updated`/`notification.created` события неотличимы от настоящих. Исходы (одобрение
документа, задержка комплаенса) — не случайны, а хэш от `(deal_id, kind)` (`fnv1a`), поэтому
одна и та же сделка всегда ведёт себя одинаково — воспроизводимо для демо и тестов.
Управляется переменными окружения:

- `EMULATOR_SPEED` — `demo` (секунды, по умолчанию) или `realistic` (минуты).
- `EMULATOR_MANUAL` — `true` отключает фоновый цикл и включает ручку
  `POST /internal/emulator/tick` (без JWT, не публикуется наружу) для детерминированных
  тестовых стендов; без неё маршрут не регистрируется вовсе.

**События** — transactional outbox: `DealRepository::apply` пишет строки в `outbox` в той же
транзакции, что и данные, паблишер (`runEvery(0.5s)`) публикует их в Redis-канал
`deal-events:{company_id}` и отмечает `published_at`. Каждое сообщение несёт `seq`
(= `outbox.id`) — уникальный идентификатор для дедупликации на стороне потребителя;
доставка at-least-once, порядок `seq` **не гарантирован** между параллельными сделками.
Redis pub/sub не хранит историю сообщений — `PUBLISH` доставляет только тем, кто подписан
в этот момент; BFF, переподключившийся к каналу, должен перезапросить список сделок у
service-core, а не ждать, что канал дошлёт пропущенное за время простоя.

## Что НЕ сделано (специально, для следующих итераций)

- Метрики Prometheus (`/metrics`)
- Structured logging (slog-style)
- mTLS между сервисами (на случай когда они таки начнут общаться)
- HTTPS на Traefik с Let's Encrypt
- JWKS endpoint вместо файлового ключа (для ротации без передеплоя)
- Rate limiting на Traefik
- `NetworkPolicy` для `bff` и `frontend` — дописывается при первом выкате по
  фактическим меткам пода Traefik (план 08, ADR 0006)

k3s-манифесты и CORS в этот список больше не входят: чарт
`infra/helm/generic-service` выкатывает все пять релизов, CORS задаётся
списком источников (`CORS_ALLOWED_ORIGINS` у Traefik в compose,
`ALLOWED_ORIGINS` у BFF в k3s), а не звёздочкой.

## Миграция k3s: test-python → commission (один раз перед мержем в main)

Пилотный сервис `service-test-python`/`pg-test-python` заменяется на `service-commission`.
CD не устанавливает `pg-commission` и не удаляет старые релизы сам — перед мержем PR с
service-commission в `main` выполнить руками на k3s-сервере:

```bash
make k3s-secrets      # пароли, если их ещё нет
make k3s-deploy-data  # поднимет pg-auth/pg-core/pg-commission и redis
helm uninstall service-test-python -n backend
helm uninstall pg-test-python -n data
```

**Литеральный `--set auth.password=commission` здесь больше не годится** (и
раньше был источником того самого расхождения): пароль обязан совпадать с
тем, что уезжает в Secret сервиса, а единственный его источник —
`infra/helm/secrets-k3s.env`. Если `pg-commission` уже поднят со старым
литералом, `make k3s-deploy-data` это заметит и остановится с объяснением:
`helm upgrade` пароль в уже инициализированной базе не меняет.

## Тесты и CI

Наборы тестов:

- `make test-cpp` — Catch2, C++ (46 тестов): `libs/common`, `service-auth`, `service-core`.
  Отдельная workspace-сборка с `-DBUILD_TESTS=ON`, не образ сервиса.
- `make test-commission` — pytest без БД (55 тестов): `app/domain.py` и остальная логика
  `service-commission`, не требует Postgres.
- `make test-commission-db` — pytest с маркером `db` (+6 тестов): поднимает `pg-commission`
  из `docker-compose.dev.yml` (host-порт 5434) и гоняет тесты репозитория против реальной БД.
  **Не входит в `make test-all`**, потому что требует уже поднятого стенда с host-портом, а не
  просто `uv sync` — держать это отдельной командой честнее, чем прятать побочный эффект
  внутри общей цели.
- `make test-api` — pytest (`tests/api`, 12 тестов) против полного стенда
  (`docker-compose.yml` + `docker-compose.dev.yml`, `EMULATOR_MANUAL=true`) через сквозной HTTP.

Прогнать всё, что гоняет CI, одной командой:

```bash
make test-all   # test-cpp + test-commission + test-api
```

### Что делает CI (`.github/workflows/ci.yml`)

На каждый `pull_request` и на `push` в любую ветку, кроме `main`, четыре независимые джобы
(кроме `api`, которой нужен собранный `ivr-cpp-base`, — она ждёт джобу `cpp`):

- `shellcheck` — `shellcheck infra/*.sh`.
- `cpp` — собирает `ivr-cpp-base` (кэш GitHub Actions), затем `cmake`/`ctest` внутри него.
- `commission` — `pytest -m "not db"`, затем с `postgres:16-alpine` как service-контейнером
  джобы — миграции через `infra/migrate.sh` и `pytest -m db`.
- `api` — поднимает стенд (`up -d --wait`, `EMULATOR_MANUAL=true`) и гоняет `tests/api`; при
  падении логи `docker compose logs` уходят в артефакт джобы.

### Зачем базовый образ и когда его пересобирать

`ivr-cpp-base` (`infra/docker/cpp-base.Dockerfile`) собирает drogon и libjwt один раз вместо
того, чтобы каждый сервисный Dockerfile (`service-auth`, `service-core`) делал это заново —
сборка drogon с нуля занимает ~15-19 минут. Локально образ собирается через `make cpp-base`
(или неявно при `make up`, если его ещё нет); в CI/CD — через `docker/build-push-action` с
`cache-from`/`cache-to: type=gha` (в `deploy.yml` образ ещё и пушится в `ghcr.io`, сервисные
сборки берут его через `--build-arg BASE_IMAGE=...`).

Пересобирать вручную (`make cpp-base`) нужно только при смене версии drogon/libjwt или набора
apt-пакетов в `cpp-base.Dockerfile` — изменения кода `service-auth`/`service-core` на него не
влияют (образ не зависит от исходников репозитория).

## Переход на k3s

Когда будешь готов:

1. Каждый сервис → `Deployment` + `Service` (ClusterIP)
2. Каждый Postgres → `StatefulSet` + `PVC` + `Service`
3. Redis → `Deployment` + `Service` (или single-node `StatefulSet`)
4. Traefik → уже встроен в k3s, нужен только `Ingress` ресурс с теми же правилами что в compose labels
5. Ключи JWT → `Secret` (private у service-auth, public у остальных)
6. Конфиги → `ConfigMap`

Это **прямой 1:1 перенос** — структура compose специально такая, что мапится в k8s манифесты без переосмысления.

### Периметр в k3s (план 08)

- `IngressRoute` есть только у `bff` и `frontend`; у auth/core/commission
  `ingress.enabled: false`. Внутренняя ручка эмулятора
  `POST /internal/emulator/tick` (спека §4.3) в кластере не публикуется
  вообще — e2e дёргают её только на compose-стенде через host-порт 18081
  из `docker-compose.dev.yml`.
- `NetworkPolicy` чарта пускает к сервису только поды из
  `networkPolicy.allowFrom`: к auth и core — `bff`, к commission — `bff` и
  `service-core`. Источники ищутся **в том же namespace** — `bff` обязан
  жить в `backend` вместе с сервисами.
- **Task 1 нельзя выкатывать без Task 2.** Пока в кластере нет пода `bff`
  (`values-bff.yaml`), политика закрывает auth и core для всех, и это
  выглядит как «сервисы не поднялись». Выкатывать периметр и BFF вместе.
- Если после применения политики поды встают `Unhealthy` — первым делом
  смотреть `networkPolicy.allowFromCIDRs`: на части CNI kubelet ходит в под
  с адреса узла, и такой трафик надо разрешать подсетью узлов, а не
  `namespaceSelector` (правила `kube-system` здесь намеренно нет — оно
  пустило бы к сервисам ещё и Traefik, CoreDNS, metrics-server).
- У `bff` и `frontend` `networkPolicy.enabled: false`, и это не забытый
  флаг: к ним ходит Traefik — под из **другого** namespace (в k3s это
  `kube-system`), чьи метки без живого кластера неизвестны. Неверный
  `podSelector` здесь молча закрыл бы единственный вход в систему. Правило
  дописывается при первом выкате, по фактическим меткам пода Traefik.
  **Следствие, которое надо называть прямо:** пока политики нет, к
  `bff:4000` может ходить любой под кластера — в обход Traefik и его
  middleware целиком. Внутренний периметр (auth/core/commission пускают
  только bff) от этого не страдает, но сам bff внутри кластера сейчас не
  защищён ничем. Это незакрытый пункт, а не «периметр держится на Traefik».
### Порядок выката: `service-auth` → … → `bff` → `frontend` (план 08)

Порядок обязателен и задан в коде, а не в памяти запускающего:
`infra/services.tsv` — единственный список выкатываемых сервисов, и
**порядок строк в нём и есть порядок выката**. Его читают оба механизма:
`make k3s-deploy` (последовательный цикл, не список зависимостей — под `-j`
тот выполнялся бы параллельно) и `.github/workflows/deploy.yml`.

Две зависимости старта, обе воспроизведены:

- **`bff` падает при старте, если недоступен `service-auth`.** Он тянет
  публичный ключ auth до того, как поднимет сервер. Разделение `/health` и
  `/ready` здесь не спасает: до проб дело не доходит.
- **`frontend` падает при старте, если недоступен `bff`.** nginx резолвит
  имя `bff` при разборе конфига (`proxy_pass http://bff:4000`), а не при
  первом запросе, и без пода bff уходит в CrashLoopBackOff с `[emerg] host
  not found in upstream "bff"` — выглядит как сломанный образ SPA, хотя
  сломан порядок.

`service-core` и `service-commission` таких зависимостей **не имеют**:
публичный ключ JWT оба читают из файла (`JWT_PUBLIC_KEY_PATH`), а обращение
core → commission ленивое — клиент создаётся при первом сценарии
(`services/service-core/src/commission_client.cc`), а не при старте. Поэтому
их место в списке между auth и bff произвольно; место auth, bff и
frontend — нет.

`--wait` в обоих механизмах не косметика: без него `helm` возвращается сразу
и последовательность перестаёт что-либо означать.

### Проверки до выката (`infra/k3s-preflight.sh`)

Один скрипт на оба механизма — его вызывают и `make k3s-deploy`, и
`deploy.yml` по ssh. Все проверки идут **по всем строкам до первого
`helm upgrade`**: обрыв посреди цикла оставил бы стенд наполовину
обновлённым (auth новый, остальные старые), а это хуже, чем не начинать.

Что проверяется:

- **пароль задан** — локально в `secrets-k3s.env`, в CD в secrets
  репозитория;
- **пароль совпадает с тем, что в базе** — сверяется с Secret
  `pg-<short>-postgresql`. Это единственный момент, когда обе стороны
  известны одновременно; после `helm upgrade` первая теряется. Без
  кластера проверка пропускается и прямо об этом говорит;
- **тег bff/frontend не является неизменяемым.** `latest` хуже пустого
  тега: `helm upgrade` с неизменившимся `image.tag` — no-op, новая сборка
  фронта молча не выкатывается, а выкат считается успешным. Пока в
  `frontend-tags.env` стоит заглушка `latest`, выкат **обрывается целиком**
  — это отказ работать вхолостую, а не поломка. Для локального стенда:
  `K3S_ALLOW_MUTABLE_TAGS=1`; в CD такого послабления нет.

CD добавляет к этому свою проверку на раннере: `docker manifest inspect`
каждого образа из `services.tsv` до первого `helm upgrade`. Путь образов
bff и frontend (`image.repository` в их values) до первого dispatch из
`alfa-cbdc-hub` ничем не подтверждён, а `ImagePullBackOff` посреди выката
дороже отказа до его начала — тем более что по порядку фронт идёт
последним.

**Шестой сервис — одна строка в `infra/services.tsv`.** Колонки описаны в
шапке файла: собирается ли образ здесь, нужен ли ему `BASE_IMAGE`, есть ли
у него миграции, откуда берётся тег, есть ли у него своя БД.
`make k3s-build-bff` и `make k3s-build-frontend` намеренно падают с
объяснением: этих Dockerfile'ов здесь нет и не будет, образы собирает CI
`alfa-cbdc-hub`.

### Секреты в k3s — и почему в compose пароли открыты (план 08)

Это осознанное разделение, а не недоделка.

- **k3s.** Пароли БД не лежат в `infra/helm/values-*.yaml`. Каждый релиз с
  БД объявляет `secrets.enabled: true`, а значения приезжают снаружи git —
  из CD или из файла, который не коммитится:

  ```bash
  helm upgrade --install service-auth infra/helm/generic-service \
    -f infra/helm/values-service-auth.yaml \
    -f infra/helm/generated/migrations-service-auth.yaml \
    -n backend --set secrets.data.dbPassword='<пароль>'

  # service-commission берёт из Secret не только пароль, но и весь DSN
  # (пароль там — часть строки подключения). Передаётся всё равно ОДИН
  # --set: DSN живёт в secrets.derived и собирается из dbPassword. Два
  # независимых --set с одним паролем внутри разъехались бы при ротации
  # молча.
  ```

  **Откуда берутся значения при локальном выкате.** Руками их набирать не
  надо — и, главное, не надо набирать их дважды:

  ```bash
  make k3s-secrets     # создаёт infra/helm/secrets-k3s.env (режим 600, вне git)
  make k3s-deploy      # k3s-deploy-data + пять сервисов в порядке services.tsv
  ```

  Этот один файл читают **оба** места, которые иначе разъезжаются молча:
  `k3s-deploy-data` (пароль, который получает Postgres при инициализации) и
  `k3s-deploy-%` (пароль, который чарт кладёт в Secret релиза). До плана 08
  первый ставил литералы `auth`/`core`/`commission`, а второй — то, что
  передали снаружи; чарт не имеет способа узнать, что в базе, поэтому
  расхождение давало идеальный рендер и `password authentication failed` в
  поде. Без файла `make k3s-deploy-*` останавливается с объяснением, а не
  выкатывает сервис с пустым паролем.

  **Откуда берутся значения в GitHub Actions.** Из secrets репозитория
  (Settings → Secrets and variables → Actions):
  `DB_PASSWORD_AUTH`, `DB_PASSWORD_CORE`, `DB_PASSWORD_COMMISSION`. Имена
  перечислены в последней колонке `infra/services.tsv` — там же, где их
  читает скрипт выката, так что добавление шестой БД не требует искать их
  по workflow. Пустой секрет обрывает выкат с сообщением. Значения в
  репозиторий не коммитятся никогда.

  **Как генерировать пароль.** `openssl rand -base64` использовать нельзя:
  он выдаёт `/`, а пароль `service-commission` попадает в userinfo DSN, где
  `/` обязан быть процент-кодирован — гвард чарта такой пароль отвергает, и
  первый же честный выкат упирается в нашу собственную проверку. Набор
  заведомо разрешённый:

  ```bash
  LC_ALL=C tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 32; echo
  ```

  Ровно это делает `make k3s-secrets`.

  **Ротация пароля — отдельная процедура, а не повторный выкат.** Чарт
  bitnami/postgresql задаёт пароль только при ПЕРВОЙ инициализации PVC.
  `helm upgrade` с другим `auth.password` оставит в базе старый пароль, а
  Secret сервиса поедет с новым — то же `password authentication failed`, но
  теперь ещё и после «успешного» выката. Менять пароль после первого
  выката = `ALTER USER` в базе (или снос PVC, если данные не жалко), и
  только потом правка `secrets-k3s.env` / secrets репозитория.

  **`secrets.data` — сырые данные, `secrets.derived` — шаблоны.** Разделение
  обязательное, а не косметическое: пароль пользователя нельзя прогонять
  через `tpl`. Пароль с `{{` внутри уронил бы рендер (`cannot parse
  template`), а если бы `{{…}}` оказалось валидным шаблоном — подставился бы
  молча и **по-разному** в Secret и в `config.json` (у `tpl` один проход, а
  `config.json` берёт значение до него), и сервис с миграциями ушли бы в базу
  с разными паролями. Шаблонизируется только `derived`, то есть то, что
  написано в репозитории. Проверено рендером на паролях с `{{`, `"`, `\`,
  `$`, `` ` `` и `$(…)`: во всех местах — ровно заданная строка.

  Имена ключей Secret'а и имён переменных окружения проверяются на
  пригодность для Kubernetes (`[-._a-zA-Z0-9]+` плюс отдельный запрет на
  вырожденные `.` и `..`, и C_IDENTIFIER): иначе опечатку вроде
  `secrets.data.bad key` отбраковал бы API-сервер при apply, а не рендер.

  **Ограничение на состав пароля.** Пароль `service-commission` попадает в
  userinfo строки подключения (`postgresql+asyncpg://commission:ПАРОЛЬ@…`),
  поэтому разрешены только символы, которые RFC 3986 позволяет там без
  процент-кодирования: латиница, цифры и `- . _ ~ ! $ & ' ( ) * + , ; =`.
  Двоеточие тоже запрещено — в userinfo оно отделяет пользователя от пароля.
  Пароль с `@`, `/`, `#`, `?`, `%`, пробелом и прочим **роняет рендер** с
  сообщением, называющим конкретные символы.

  Это проверка, а не кодирование, и намеренно: процент-энкодер в
  Helm-шаблоне — лекарство хуже болезни, а `urlquery` кодирует пробел как
  `+`, который в userinfo раскодируется обратно плюсом, то есть молча меняет
  пароль. Пароли для стенда генерируем мы сами, так что ограничение ничего
  не стоит; тихо разъехавшийся DSN стоил бы отладки на кластере — Secret при
  этом выглядел бы совершенно правильным. У `service-auth` и `service-core`
  такого ограничения нет: их пароль в URL не попадает.

  Ссылки на ключи Secret'а сверяются с тем, что в нём есть: опечатка в
  `migrations.db.passwordSecretKey` или в `envSecret[].key` роняет рендер и
  называет имеющиеся ключи. Без этой сверки ошибка дожила бы до кластера и
  выглядела бы как `CreateContainerConfigError`, то есть «под не стартует»,
  а не «забыли `--set`».

  Под перезапускается при смене секрета: в аннотациях пода
  `checksum/secrets` и `checksum/config`. Для `config.json` это не
  оптимизация, а единственный способ — он монтируется через `subPath`, а
  такие тома kubelet не обновляет вообще, и без чексуммы под остался бы со
  старым паролем навсегда.

  Параметры, которые переопределяет CD (`ALLOWED_ORIGINS`, `COOKIE_SECURE`),
  живут в `envMap` — map, а не список: `--set env[5].value=...` адресует
  переменную по индексу массива, и вставка любой строки выше по списку молча
  переназначила бы другую. Запятые экранируются, их режет сам `--set`:
  `--set-string envMap.ALLOWED_ORIGINS='https://a\,https://b'`.

  Пароль подтягивают и сервис, и init-контейнер миграций
  (`migrations.db.passwordSecretKey`) — из одного и того же Secret'а, иначе
  секрет защищал бы половину пути. У auth и core `config.json` содержит
  `db_clients[].passwd`, поэтому он едет **Secret'ом, а не ConfigMap**
  (`config.secret: true`): выносить пароль в Secret бессмысленно, если он же
  рядом лежит в ConfigMap, который отдаётся любому с `get configmaps`.
  Забыть `--set` нельзя: рендер падает с явным сообщением, а не выкатывает
  сервис с пустым паролем.

- **compose.** Здесь пароли (`auth`/`core`/`commission`) остаются **открытым
  текстом** в `docker-compose.yml`, и так и задумано: это локальный стенд,
  который поднимается одной командой на машине разработчика, ходит по http и
  не хранит ничего ценного. Заводить для него секрет-менеджер значит платить
  сложностью за защиту от несуществующей угрозы. **Compose от этого не стал
  защищённым — он им и не был.** Наружу его публиковать нельзя.

### Непривилегированные поды (план 08)

Все пять подов: `runAsNonRoot: true`, `allowPrivilegeEscalation: false`,
`capabilities: drop: [ALL]`, `seccompProfile: RuntimeDefault`.

- `runAsUser` задаётся **явно** в values каждого сервиса. Без него kubelet
  отказывается стартовать контейнер: образы auth и core не объявляют `USER`
  вовсе, а commission объявляет нечисловой `USER app` — доказать, что
  процесс не root, kubelet не может ни в том, ни в другом случае. Рендер
  чарта падает, если `runAsNonRoot: true` стоит без `runAsUser`.
- `readOnlyRootFilesystem: true` у **всех пяти**, исключений нет. Двум
  сервисам для этого нужен записываемый каталог, и оба — `emptyDir`, без
  состояния:
  - `frontend` → `/tmp`: nginx-unprivileged держит там pid и все
    `*_temp`-пути, без него падает на `mkdir() "/tmp/proxy_temp" failed
    (30: Read-only file system)`. `/var/cache/nginx` монтировать не надо —
    проверено запуском `docker run --read-only --user 101:101 --tmpfs /tmp`
    (HTTP 200).
  - `service-auth`, `service-core` → `/app/uploads`: Drogon при старте
    создаёт `./uploads/tmp/00 … FF` относительно cwd. Без тома сервис
    работает (`/health` 200), но печатает 256 строк `ERROR Error 30 creating
    path` на каждый рестарт — лог с 256 ERROR в норме перестаёт быть логом.
- Образ SPA — `nginxinc/nginx-unprivileged` и слушает **8080**, а не 80:
  привязка к порту ниже 1024 требует root, под `runAsNonRoot` стоковый
  `nginx:alpine` не стартует вообще.
