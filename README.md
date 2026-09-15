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

## Что НЕ сделано (специально, для следующих итераций)

- `service-auth/me` парсит JWT — пока заглушка, нужно использовать тот же verifier что в service-core
- Метрики Prometheus (`/metrics`)
- Structured logging (slog-style)
- Тесты Catch2 для C++
- mTLS между сервисами (на случай когда они таки начнут общаться)
- k3s манифесты (Deployment, Service, Ingress, StatefulSet для Postgres)
- HTTPS на Traefik с Let's Encrypt
- JWKS endpoint вместо файлового ключа (для ротации без передеплоя)
- Rate limiting на Traefik
- CORS — нужен будет когда фронт появится

## Переход на k3s

Когда будешь готов:

1. Каждый сервис → `Deployment` + `Service` (ClusterIP)
2. Каждый Postgres → `StatefulSet` + `PVC` + `Service`
3. Redis → `Deployment` + `Service` (или single-node `StatefulSet`)
4. Traefik → уже встроен в k3s, нужен только `Ingress` ресурс с теми же правилами что в compose labels
5. Ключи JWT → `Secret` (private у service-auth, public у остальных)
6. Конфиги → `ConfigMap`

Это **прямой 1:1 перенос** — структура compose специально такая, что мапится в k8s манифесты без переосмысления.
