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
        │  пишет: ты       │  │  пишет: ты          │  │  комиссии/котировки │
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
make test-core
make smoke-commission
```

## Структура

```
backend-platform/
├── docker-compose.yml                # 3 сервиса + 3 БД + Redis + Traefik
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

То же что в auth, но с `"core_svc::JwtFilter"` в `ADD_METHOD_TO` если нужна авторизация.
`user_id` берёшь из `req->attributes()->get<std::string>("user_id")`.

### В service-commission (Python)

Логика — чистые функции в `app/domain.py` (тесты в `tests/test_domain.py`), доступ к БД — `app/repository.py`,
ручки — `app/api.py` внутри `create_router`. Авторизация — зависимость `User` (JWT, возвращает `sub`).
Миграции — `migrations/NNN_*.sql`. Тесты: `make test-commission` и `make test-commission-db`.

## Ломающее изменение: компании (план 02)

Регистрация требует `company_name` и `inn` (10 цифр). Пользователи с одинаковым ИНН
попадают в одну компанию. В JWT добавлен claim `company_id`, и токены **без него
считаются невалидными** — выданные раньше токены нужно перевыпустить (повторный логин).
Ошибки C++-сервисов отдаются в формате `{"code": "...", "error": "..."}`.

**Rolling deploy на k3s**: миграция `002_company.sql` делает `users.company_id`
`NOT NULL`. Пока старые поды service-auth ещё живы во время выката — их
`INSERT INTO users` не заполняет эту колонку, и регистрация на них начнёт
падать сразу после применения миграции. Поэтому миграцию и новый образ нужно
выкатывать вместе, одним шагом; короткий простой регистрации во время
переключения допустим.

Тесты C++ (Catch2, собираются только в workspace-сборке):

    make test-cpp

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

## Переход на k3s

Когда будешь готов:

1. Каждый сервис → `Deployment` + `Service` (ClusterIP)
2. Каждый Postgres → `StatefulSet` + `PVC` + `Service`
3. Redis → `Deployment` + `Service` (или single-node `StatefulSet`)
4. Traefik → уже встроен в k3s, нужен только `Ingress` ресурс с теми же правилами что в compose labels
5. Ключи JWT → `Secret` (private у service-auth, public у остальных)
6. Конфиги → `ConfigMap`

Это **прямой 1:1 перенос** — структура compose специально такая, что мапится в k8s манифесты без переосмысления.
