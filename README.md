# backend-platform

Микросервисная платформа Alfa Global CBDC Hub: сервисы за общим ingress, каждый со своей БД, объединённые через JWT.

## Архитектура

```
                            ┌──────────────────────────────┐
[Frontend] ──HTTP──>        │  Traefik (Ingress)           │
                            │  один URL, разные пути       │
                            └──┬─────────┬─────────┬───────┘
              /api/auth/*  ────┘         │         └──── (внутр.)
                                /api/core/*
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

Регистрация и логин:

```bash
make test-register
make test-login    # копируешь token из ответа

export TOKEN=<твой токен>
make test-me
make smoke-commission
make smoke-deal       # сквозная сделка: создать → сценарий → документы → эмулятор → completed
make test-api         # pytest против поднятого стенда (EMULATOR_MANUAL=true)
```

`make up` наружу отдаёт только traefik (порт 80) и его дашборд (8081) — так
локальная топология совпадает с прод. Host-порты сервисов и БД (18080,
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
Traefik пока про него не знает — внешняя маршрутизация появится в плане 08,
поэтому в `docker-compose.yml` `bff` виден только другим сервисам внутри
`backend-net`.

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
Traefik про `frontend` пока не знает (план 08), профиль и образ ещё не
опубликован.

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
│   └── k3s/                          # манифесты k3s (TODO)
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
- k3s манифесты (Deployment, Service, Ingress, StatefulSet для Postgres)
- HTTPS на Traefik с Let's Encrypt
- JWKS endpoint вместо файлового ключа (для ротации без передеплоя)
- Rate limiting на Traefik
- CORS — нужен будет когда фронт появится

## Миграция k3s: test-python → commission (один раз перед мержем в main)

Пилотный сервис `service-test-python`/`pg-test-python` заменяется на `service-commission`.
CD не устанавливает `pg-commission` и не удаляет старые релизы сам — перед мержем PR с
service-commission в `main` выполнить руками на k3s-сервере:

```bash
helm upgrade --install pg-commission oci://registry-1.docker.io/bitnamicharts/postgresql -n data \
  --set auth.username=commission --set auth.password=commission --set auth.database=commission \
  --set primary.persistence.size=1Gi
helm uninstall service-test-python -n backend
helm uninstall pg-test-python -n data
```

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
