# Репозиторий фронта и настоящий BFF — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Завести фронтенд под git и превратить BFF из мок-сервера в настоящий: сессия в HttpOnly-cookie, клиенты к auth/core/commission, агрегация данных для экранов и живые события из Redis в WebSocket.

**Architecture:** BFF — единственная дверь для SPA (спека §3): он держит сессию, ходит в сервисы с JWT пользователя и раздаёт события. Токен живёт в cookie и никогда не попадает в JavaScript, поэтому XSS не уводит сессию. Подпись BFF не проверяет на каждом запросе — это делают сервисы; исключение одно: при upgrade WebSocket нужно знать `company_id`, чтобы подписаться на нужный канал Redis. События — триггеры «перезапроси сделку», данных не несут, поэтому дедупликация идёт по множеству увиденных `seq`, а не по «отбрасывать всё меньше последнего» (спека §4.4: порядок не гарантирован).

**Tech Stack:** Node 22, pnpm 11, TypeScript 5.5, Express 4, `ws`, `ioredis`, `jose`, `zod`, vitest + supertest, Docker.

**Spec:** `docs/superpowers/specs/2026-09-15-cbdc-hub-integration-design.md` (§2.2 репозиторий Frontend, §3.1 аутентификация, §6 BFF, §9 строка «Unit/integration TS»).

**Два репозитория.** Основная работа — в `/home/legors/Documents/alfa-cbdc-hub` (станет `IVR-CBDC/Frontend`). Task 5 дополнительно меняет `/home/legors/Documents/IVR` (Backend) — там свой коммит в своей ветке. SPA не трогаем: она в плане 06.

## Global Constraints

- Frontend-репозиторий: `/home/legors/Documents/alfa-cbdc-hub`, ветка `main` (создаётся в Task 1 вместе с `git init`). Backend-репозиторий: `/home/legors/Documents/IVR`, ветка `feat/bff-service` (создаётся в Task 5 от `main`).
- Конверт ошибок BFF: `{"code": "<UPPER_SNAKE>", "error": "<сообщение по-русски>"}`. Коды сервисов (`VALIDATION_ERROR`, `NOT_FOUND`, `VERSION_CONFLICT`, `INVALID_TRANSITION`, `SCENARIO_UNAVAILABLE`, `INVALID_CREDENTIALS`, `USER_EXISTS`, `TOKEN_EXPIRED`, `INVALID_TOKEN`, …) пробрасываются как есть вместе со статусом; недоступность или таймаут upstream → `503 UPSTREAM_UNAVAILABLE`; нет cookie → `401 UNAUTHORIZED`.
- Cookie сессии: имя `session`, `HttpOnly`, `SameSite=Strict`, `Path=/`, `Secure` при `COOKIE_SECURE=true`, `Max-Age` из `exp` токена. Токен не отдаётся в теле ответа SPA.
- CSRF: на мутирующих запросах (POST/PATCH/DELETE) проверяется заголовок `Origin` против списка `ALLOWED_ORIGINS`; несовпадение → `403 FORBIDDEN_ORIGIN`.
- WebSocket: `seq` — идентификатор доставки, **не** порядок. Дедупликация — множеством увиденных значений ограниченного размера; событие с меньшим `seq` отбрасывать нельзя.
- Таймаут запроса к любому upstream — 5 секунд, повторов нет.
- Деньги приходят из сервисов числами; BFF их не пересчитывает и не округляет.
- Тесты BFF гоняются без поднятого стенда: upstream'ы подменяются, Redis — `ioredis-mock` или собственный фейк.
- Никаких `git push` и операций с кластером. Контейнеры `registry`, `legors-db`, `tealdeer-db` не трогать.
- Сообщение коммита заканчивается строкой `Co-Authored-By: Claude <модель> <noreply@anthropic.com>` — модель та, что реально пишет коммит.

## File Structure

```
alfa-cbdc-hub/                          (станет IVR-CBDC/Frontend)
├── .gitignore                MOD  + .env.local, coverage
├── README.md                 MOD  два пакета, как запускать, связь с Backend
├── bff/                      был backend/
│   ├── package.json          MOD  ioredis, jose, cookie-parser, vitest, supertest
│   ├── src/
│   │   ├── index.ts          MOD  сборка приложения, запуск, graceful shutdown
│   │   ├── app.ts            NEW  createApp(deps) — чистая сборка express без listen
│   │   ├── config.ts         NEW  env через zod, одно место правды
│   │   ├── errors.ts         NEW  ApiError, мапперы, обработчик
│   │   ├── session.ts        NEW  cookie сессии, requireSession, проверка Origin
│   │   ├── upstream/
│   │   │   ├── http.ts       NEW  fetch с таймаутом, проброс кодов, x-request-id
│   │   │   ├── auth.ts       NEW  register/login/me
│   │   │   ├── core.ts       NEW  сделки, документы, уведомления
│   │   │   └── commission.ts NEW  quotes
│   │   ├── routes/
│   │   │   ├── auth.ts       NEW
│   │   │   ├── deals.ts      MOD  переписан на core (моки удалены)
│   │   │   └── notifications.ts NEW
│   │   ├── ws/hub.ts         MOD  Redis subscribe, подписка по company_id, дедуп
│   │   ├── types.ts          MOD  типы контракта BFF↔SPA
│   │   ├── mock/data.ts      DEL
│   │   └── services/cache.ts MOD  кеш только для котировок
│   └── tests/                NEW  vitest: session, errors, routes, ws
└── frontend/                 не трогаем (план 06)

IVR/ (Backend, Task 5)
├── docker-compose.yml               MOD  сервис bff (образ из ghcr)
├── docker-compose.dev.yml           MOD  порт bff наружу
├── docker-compose.override.yml.example NEW  сборка bff из ../alfa-cbdc-hub
└── README.md                        MOD  как поднять стенд вместе с BFF
```

---

### Task 1: Репозиторий, переезд backend → bff, инструменты

**Files:**
- Create: `.git` (git init), `bff/vitest.config.ts`, `bff/tests/smoke.test.ts`
- Rename: `backend/` → `bff/`
- Modify: `bff/package.json`, `.gitignore`, `README.md`
- Delete: `bff/src/mock/data.ts`, `bff/pnpm-workspace.yaml`, `frontend/pnpm-workspace.yaml`

**Interfaces:**
- Consumes: существующий код BFF (express-приложение с моками), Node 22 + pnpm 11.
- Produces: git-репозиторий с первым коммитом; каталог `bff/`; `pnpm --dir bff test` (vitest), `pnpm --dir bff typecheck`, `pnpm --dir bff build` работают.

- [ ] **Step 1: Инициализировать репозиторий**

```bash
cd /home/legors/Documents/alfa-cbdc-hub
git init -b main
```
В `.gitignore` добавить `.env.local`, `coverage/`, `*.tsbuildinfo`.
Сделать первый коммит **как есть** (до переделок), чтобы история начиналась с состояния, которое написала Мари:
```bash
git add -A && git commit -m "chore: import Alfa Global CBDC Hub frontend and BFF as authored"
```
Expected: коммит содержит `frontend/` и `backend/`, `node_modules` не попал.

- [ ] **Step 2: Переименовать и подчистить**

```bash
git mv backend bff
git rm -q bff/src/mock/data.ts bff/pnpm-workspace.yaml frontend/pnpm-workspace.yaml
```
`pnpm-workspace.yaml` в обоих пакетах содержит только `allowBuilds: esbuild: false` и никакой workspace-конфигурации — это артефакт, а не структура; каждый пакет ставится отдельно (`pnpm --dir bff install`). Если после удаления `pnpm install` начнёт ругаться на esbuild — вернуть файл в тот пакет, где он нужен, и отметить это в отчёте.
В `bff/package.json`: поле `name` → `alfa-cbdc-hub-bff` (уже так), `description` оставить.

- [ ] **Step 3: Зависимости и скрипты**

В `bff/package.json` добавить в `dependencies`: `ioredis`, `jose`, `cookie-parser`; в `devDependencies`: `vitest`, `supertest`, `@types/supertest`, `@types/cookie-parser`. Версии — актуальные на момент установки, зафиксированные lock-файлом.
Скрипты:
```json
"scripts": {
  "dev": "tsx watch src/index.ts",
  "build": "tsc -p tsconfig.json",
  "start": "node dist/index.js",
  "typecheck": "tsc -p tsconfig.json --noEmit",
  "test": "vitest run",
  "test:watch": "vitest"
}
```
`bff/vitest.config.ts`: окружение `node`, `globals: false` (импортировать `describe/it/expect` явно), `include: ["tests/**/*.test.ts"]`.

- [ ] **Step 4: Убрать моки из кода, чтобы дерево собиралось**

`bff/src/routes/deals.ts` импортирует удалённый `mock/data.ts`. На этом шаге задача — **не** переписывать маршруты (это Task 3), а оставить дерево собираемым: временно оставить только `GET /health`-маршрутизацию, а содержимое `routes/deals.ts` заменить заглушкой, которая на каждый путь отвечает `501 {"code":"NOT_IMPLEMENTED","error":"Маршрут переписывается на реальные сервисы"}`. В файл добавить комментарий со ссылкой на Task 3.
Так каждый коммит остаётся зелёным, а подмена видна и не выглядит рабочей.

- [ ] **Step 5: Тест-дымовуха `bff/tests/smoke.test.ts`**

```ts
import { describe, expect, it } from "vitest";
import request from "supertest";
import { createApp } from "../src/app.js";

describe("BFF", () => {
  it("отвечает на /health", async () => {
    const response = await request(createApp()).get("/health");

    expect(response.status).toBe(200);
    expect(response.body.status).toBe("ok");
  });
});
```
Для него завести `bff/src/app.ts` с `export function createApp(): express.Express`, куда переезжает сборка приложения из `index.ts` (без `listen`), а `index.ts` остаётся точкой входа: создаёт сервер, вешает WS, слушает порт. Разделение нужно, чтобы тесты поднимали приложение без сокета и портов.

- [ ] **Step 6: Проверка**

```bash
cd /home/legors/Documents/alfa-cbdc-hub/bff
pnpm install
pnpm typecheck && pnpm test && pnpm build
```
Expected: установка проходит, typecheck чистый, один тест зелёный, сборка создаёт `dist/`.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "chore(bff): move backend to bff, add vitest, stub out mocked routes"
```

---

### Task 2: Сессия в cookie и конверт ошибок

**Files:**
- Create: `bff/src/config.ts`, `bff/src/errors.ts`, `bff/src/session.ts`, `bff/src/upstream/http.ts`, `bff/src/upstream/auth.ts`, `bff/src/routes/auth.ts`, `bff/tests/session.test.ts`, `bff/tests/errors.test.ts`
- Modify: `bff/src/app.ts`, `bff/src/index.ts`

**Interfaces:**
- Consumes: `POST /api/auth/register`, `POST /api/auth/login`, `GET /api/auth/me` сервиса service-auth (ответы `{user_id, company_id, token}` и `{user_id, login, name, company:{id,name,inn}}`), конверт ошибок `{code, error}`.
- Produces:
  - `config.ts`: `loadConfig()` → `{port, authUrl, coreUrl, commissionUrl, redisUrl, cookieSecure, allowedOrigins, upstreamTimeoutMs}`; падает при старте с понятным сообщением, если обязательная переменная не задана.
  - `errors.ts`: `class ApiError extends Error { status: number; code: string; }`, `installErrorHandler(app)`, `upstreamUnavailable()`.
  - `upstream/http.ts`: `callUpstream<T>(url, init, timeoutMs): Promise<T>` — бросает `ApiError` с кодом и статусом upstream; таймаут/сеть → `ApiError(503, "UPSTREAM_UNAVAILABLE")`.
  - `session.ts`: `setSession(res, token)`, `clearSession(res)`, `readToken(req)`, middleware `requireSession` (нет cookie → `401 UNAUTHORIZED`), middleware `checkOrigin` для мутирующих методов.
  - `routes/auth.ts`: `POST /api/auth/register`, `POST /api/auth/login` (ставят cookie, отдают `{user_id, company_id}` без токена), `POST /api/auth/logout` (204, чистит cookie), `GET /api/auth/me`.

- [ ] **Step 1: Написать падающие тесты**

`bff/tests/errors.test.ts` — проверить, что:
- `ApiError` c кодом и статусом превращается обработчиком в тело `{code, error}` с этим статусом;
- неизвестный маршрут → `404 {"code":"NOT_FOUND"}`;
- непойманное исключение → `500 {"code":"INTERNAL_ERROR"}`, и текст исключения наружу не попадает;
- `callUpstream` на ответ `502` от upstream отдаёт `503 UPSTREAM_UNAVAILABLE`, а на `409 {"code":"VERSION_CONFLICT","error":"..."}` — `ApiError` со статусом 409 и тем же кодом (подмени `fetch` в тесте).

`bff/tests/session.test.ts` — проверить, что:
- успешный логин ставит cookie `session` с флагами `HttpOnly`, `SameSite=Strict`, `Path=/`, и **не** возвращает токен в теле;
- при `COOKIE_SECURE=true` в cookie появляется `Secure`;
- `GET /api/auth/me` без cookie → `401 UNAUTHORIZED`;
- `GET /api/auth/me` с cookie проксирует токен в заголовке `Authorization: Bearer …` (проверить на подменённом upstream);
- `POST /api/auth/logout` → `204` и cookie гасится (`Max-Age=0` или истёкшая дата);
- мутирующий запрос с чужим `Origin` → `403 FORBIDDEN_ORIGIN`, с разрешённым — проходит; GET с чужим `Origin` не блокируется.

- [ ] **Step 2: Запустить — должно упасть**

Run: `pnpm --dir bff test`
Expected: тесты падают на отсутствующих модулях (`config.ts`, `errors.ts`, `session.ts`, `upstream/http.ts`).

- [ ] **Step 3: Реализовать**

Требования, которые легко упустить:
- `Max-Age` cookie берётся из `exp` JWT: разобрать payload **без проверки подписи** (`jose.decodeJwt`) — подпись здесь не нужна, токен только что пришёл от auth по внутренней сети, а проверять его будут сервисы. Прокомментировать это, иначе следующий читатель решит, что забыли проверку.
- Если `exp` нет или он в прошлом — считать ответ auth некорректным и вернуть `502 UPSTREAM_UNAVAILABLE` (такой токен всё равно не даст работать).
- `checkOrigin` пропускает запросы без заголовка `Origin` только для не-мутирующих методов; для мутирующих отсутствие `Origin` — отказ (так ведут себя не-браузерные клиенты, а SPA всегда шлёт `Origin`).
- В `app.ts` порядок middleware: `cookieParser` → `express.json` → `checkOrigin` → маршруты → обработчик ошибок. CORS оставить, но сузить до `allowedOrigins` с `credentials: true` — иначе cookie не поедет.

- [ ] **Step 4: Прогнать тесты и проверить на живом auth**

```bash
pnpm --dir bff test
```
Затем вручную против стенда Backend (он должен быть поднят: `cd /home/legors/Documents/IVR && make test-api` оставляет его запущенным; auth доступен на `127.0.0.1:18080`):
```bash
cd /home/legors/Documents/alfa-cbdc-hub/bff
AUTH_URL=http://127.0.0.1:18080 CORE_URL=http://127.0.0.1:18081 \
  COMMISSION_URL=http://127.0.0.1:18082 REDIS_URL=redis://127.0.0.1:6379 \
  ALLOWED_ORIGINS=http://localhost:5173 pnpm dev &
curl -s -i -X POST http://localhost:4000/api/auth/register -H 'Content-Type: application/json' \
  -H 'Origin: http://localhost:5173' \
  -d '{"login":"bff-smoke","password":"hunter22","name":"BFF","company_name":"ООО BFF","inn":"7736050003"}' | head -20
```
Expected: `200`, в заголовках `Set-Cookie: session=…; HttpOnly; SameSite=Strict`, в теле нет поля `token`.
Если `COMMISSION_URL`/`REDIS_URL` наружу не проброшены — на этом шаге они не нужны, но `loadConfig` не должен падать без них; проверь, что обязательными сделаны только те, что реально нужны для старта, и отметь решение в отчёте.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(bff): cookie session, origin check and uniform error envelope"
```

---

### Task 3: Клиенты к сервисам и маршруты экранов

**Files:**
- Create: `bff/src/upstream/core.ts`, `bff/src/upstream/commission.ts`, `bff/src/routes/notifications.ts`, `bff/tests/deals.test.ts`, `bff/tests/notifications.test.ts`
- Modify: `bff/src/routes/deals.ts` (заглушка → реальные маршруты), `bff/src/services/cache.ts`, `bff/src/types.ts`, `bff/src/app.ts`

**Interfaces:**
- Consumes: service-core (`GET /api/core/deals?limit=`, `GET /api/core/deals/{id}`, `POST /api/core/deals`, `POST /api/core/deals/{id}/scenario`, `POST /api/core/deals/{dealId}/documents/{docId}/submit`, `GET /api/core/notifications?limit=`, `POST /api/core/notifications/{id}/read`), service-commission (`POST /api/commission/quotes`).
- Produces (контракт BFF↔SPA):
  - `GET /api/dashboard` → `{deals: DashboardCard[], unreadNotifications: number}`
  - `GET /api/deals`, `GET /api/deals/:id` → `{deals}` / `{deal}`
  - `POST /api/deals` → `201 {deal}`
  - `GET /api/deals/:id/scenarios` → `{corridorId, scenarios: ScenarioCard[]}` (котировки под параметры сделки; кеш 60 секунд)
  - `POST /api/deals/:id/scenario` → `{deal}`
  - `POST /api/deals/:dealId/documents/:documentId/submit` → `{deal}`
  - `GET /api/deals/:id/tracking` → `{displayId, stage, hasBlockers, blockerReason, timeline}`
  - `GET /api/notifications`, `POST /api/notifications/:id/read`

- [ ] **Step 1: Написать падающие тесты**

`bff/tests/deals.test.ts` (upstream подменён; проверяем именно работу BFF, а не сервисов):
- `/api/dashboard` собирает ответ из списка сделок core и счётчика непрочитанных уведомлений — одним ответом, и не падает целиком, если уведомления недоступны (тогда `unreadNotifications: 0` и предупреждение в лог — обосновать это решение комментарием);
- `/api/deals/:id/scenarios` зовёт commission с параметрами **сделки** (направление коридора из `operation_type`: `import` → `from=RU`, `export` → `to=RU`), а не с параметрами из запроса клиента;
- повторный вызов `/api/deals/:id/scenarios` в пределах TTL не ходит в commission второй раз (счётчик вызовов), а после изменения сделки (другая `version`) — ходит;
- `409 VERSION_CONFLICT` от core доходит до клиента с тем же статусом и кодом;
- `503` от commission на экране сценариев не роняет весь ответ, если сделка уже загружена, — решить и закрепить тестом: либо весь ответ `503`, либо карточки с `available:false`. Выбери первое (честнее: пользователь видит ошибку, а не «сценарии недоступны навсегда») и закрепи.
- `/api/deals/:id/tracking` отдаёт таймлайн и причину блокировки из сделки, не делая лишнего запроса.

`bff/tests/notifications.test.ts`: список проксируется с `unread`, отметка прочитанным возвращает `204`, `404` от core доходит как `404`.

- [ ] **Step 2: Запустить — должно упасть**

Run: `pnpm --dir bff test`

- [ ] **Step 3: Реализовать**

- `upstream/core.ts` и `upstream/commission.ts` — тонкие обёртки над `callUpstream`, принимающие токен сессии и прокидывающие `Authorization`.
- Кеш (`services/cache.ts`) оставить только для котировок; ключ — `deal.id + ":" + deal.version` (при изменении сделки ключ меняется сам, инвалидация не нужна). TTL 60 секунд. Дашборд **не** кешировать: при двух репликах BFF кеш разъедется, а данные и так приходят из core за один запрос.
- Маппинг в контракт SPA держать в одном месте (`types.ts` + функции-мапперы рядом с маршрутами), чтобы план 06 читал один файл.

- [ ] **Step 4: Проверка**

```bash
pnpm --dir bff test && pnpm --dir bff typecheck
```
И живая проверка против поднятого стенда: зарегистрироваться, создать сделку через BFF, получить сценарии, подтвердить, подать документ — все ответы должны быть теми же, что отдаёт core, но без `Authorization` со стороны клиента (только cookie).

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(bff): upstream clients and screen routes backed by core and commission"
```

---

### Task 4: События Redis → WebSocket

**Files:**
- Modify: `bff/src/ws/hub.ts`, `bff/src/index.ts`
- Create: `bff/tests/ws.test.ts`

**Interfaces:**
- Consumes: Redis-канал `deal-events:{company_id}` с payload `{seq, type, deal_id?, notification_id?, at}` (спека §4.4), PEM публичного ключа с `GET /api/auth/.well-known/jwks.json` сервиса auth (несмотря на имя, отдаётся PEM).
- Produces: `initWebSocketHub(server, deps)` — на upgrade проверяет JWT из cookie, достаёт `company_id`, подписывает сокет на события своей компании; `closeWebSocketHub()` для graceful shutdown.

- [ ] **Step 1: Написать падающие тесты `bff/tests/ws.test.ts`**

Проверить (Redis подменён фейком, публикующим события вручную):
- сокет без валидной cookie закрывается с кодом `4401` и не получает событий;
- событие, опубликованное в канал компании A, приходит подписчику A и **не** приходит подписчику компании B;
- повторная доставка того же `seq` (at-least-once) отдаётся клиенту один раз;
- событие с `seq` **меньше** уже виденного всё равно доставляется (это главный тест: спека §4.4 запрещает отбрасывать по высокой планке);
- множество увиденных `seq` ограничено по размеру (подать больше лимита и убедиться, что память не растёт неограниченно — проверить через экспортируемый размер множества).

- [ ] **Step 2: Запустить — должно упасть**

- [ ] **Step 3: Реализовать**

- Публичный ключ auth забирается один раз при старте (`fetch` PEM), с понятной ошибкой, если auth недоступен: BFF не должен молча стартовать без возможности проверить токен на upgrade.
- Одна подписка на процесс: `psubscribe deal-events:*`, роутинг по `company_id` из имени канала в `Map<companyId, Set<WebSocket>>`. На `close` сокета убирать его из набора и удалять пустые наборы (иначе утечка).
- Heartbeat 25 секунд оставить (он уже есть).
- Дедупликация: `Set<number>` увиденных `seq` **на сокет** с ограничением (например, 500 последних, вычищать по FIFO). Прокомментировать, почему не «больше последнего».
- `closeWebSocketHub()` закрывает Redis-подписчика и все сокеты; вызывать из обработчика `SIGTERM` в `index.ts`.

- [ ] **Step 4: Проверка**

`pnpm --dir bff test`, затем живая проверка: поднять BFF против стенда, подключиться `websocat` или коротким Node-скриптом с cookie от логина, создать сделку через BFF и увидеть событие. Если `websocat` нет — написать скрипт в scratchpad.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(bff): live deal events from redis over websocket"
```

---

### Task 5: BFF в стенде (меняет оба репозитория)

**Files (Frontend):**
- Create: `bff/Dockerfile`, `bff/.dockerignore`
- Modify: `README.md`

**Files (Backend, `/home/legors/Documents/IVR`, ветка `feat/bff-service`):**
- Modify: `docker-compose.yml`, `docker-compose.dev.yml`, `README.md`
- Create: `docker-compose.override.yml.example`

**Interfaces:**
- Consumes: собранный BFF (Task 1–4), стенд Backend (auth, core, commission, redis).
- Produces: compose-сервис `bff` с healthcheck'ом; порт наружу только в dev-оверлее; пример override для сборки из `../alfa-cbdc-hub`.

- [ ] **Step 1: Dockerfile для BFF**

Многостадийный: `node:22-alpine` → `pnpm install --frozen-lockfile` → `pnpm build` → рантайм только с `dist/`, прод-зависимостями и непривилегированным пользователем. `HEALTHCHECK` не задавать в образе — он будет в compose (так принято в этом репозитории).
`.dockerignore`: `node_modules`, `dist`, `tests`, `coverage`, `.git`.

- [ ] **Step 2: Сервис в compose (Backend-репозиторий, новая ветка)**

```bash
cd /home/legors/Documents/IVR && git checkout main && git checkout -b feat/bff-service
```
В `docker-compose.yml` добавить:
```yaml
  bff:
    image: ${BFF_IMAGE:-ghcr.io/ivr-cbdc/backend/bff:latest}
    environment:
      PORT: "4000"
      AUTH_URL: http://service-auth:8080
      CORE_URL: http://service-core:8080
      COMMISSION_URL: http://service-commission:8000
      REDIS_URL: redis://redis:6379
      ALLOWED_ORIGINS: http://localhost:5173,http://localhost
      COOKIE_SECURE: "false"
    depends_on:
      service-auth: { condition: service_healthy }
      service-core: { condition: service_healthy }
      service-commission: { condition: service_healthy }
      redis: { condition: service_healthy }
    healthcheck:
      test: ["CMD-SHELL", "wget -qO- http://127.0.0.1:4000/health | grep -q '\"status\":\"ok\"' || exit 1"]
      interval: 5s
      timeout: 3s
      retries: 20
      start_period: 5s
    networks: [backend-net]
```
(Проверь, что `wget` есть в `node:22-alpine` — он есть в busybox; если нет, положи `curl` в рантайм-стадию.)
В `docker-compose.dev.yml` добавить `bff: ports: ["127.0.0.1:14000:4000"]`.
Создать `docker-compose.override.yml.example` со сборкой bff из `../alfa-cbdc-hub` и комментарием, что файл нужно скопировать в `docker-compose.override.yml` **и перечислять явно** в `-f`-цепочках (правило из плана 04).
В Traefik пока не публиковать: маршрутизация наружу — план 08.

- [ ] **Step 3: Проверка**

```bash
cd /home/legors/Documents/alfa-cbdc-hub && docker build -t bff-local:test -f bff/Dockerfile bff
cd /home/legors/Documents/IVR
BFF_IMAGE=bff-local:test docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d --wait bff
docker compose ps bff
curl -s http://127.0.0.1:14000/health
```
Expected: образ собирается, `--wait` дожидается `healthy`, `/health` отвечает `{"status":"ok"}`.
Затем сквозная проверка через BFF: регистрация → создание сделки → сценарии → документ, и параллельно открытый WebSocket получает события.

- [ ] **Step 4: Документация**

- `README.md` Frontend-репозитория: что это за репозиторий (станет `IVR-CBDC/Frontend`), два пакета (`bff/`, `frontend/`), как запускать BFF локально против стенда Backend, какие переменные нужны, как гонять тесты.
- `README.md` Backend-репозитория: как поднять стенд вместе с BFF (через `BFF_IMAGE` или override), и что SPA появится в плане 06.

- [ ] **Step 5: Два коммита — по одному в каждом репозитории**

```bash
cd /home/legors/Documents/alfa-cbdc-hub && git add -A && git commit -m "build(bff): production image and README"
cd /home/legors/Documents/IVR && git add -A && git commit -m "feat(compose): run the BFF in the stack"
```
В отчёте указать оба хеша и явно сказать, что ветка Backend (`feat/bff-service`) остаётся невмёрженной — её смержит контроллер.
