# k3s: изоляция, секреты, публикация и CD — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Довести систему до состояния, в котором её не стыдно выкатить: наружу торчат только фронт и BFF, сервисы доступны лишь тем, кому положено, пароли не лежат открытым текстом, все пять образов публикуются и деплоятся одним механизмом, а e2e знают, где брать внутреннюю ручку эмулятора.

**Architecture:** Два периметра. Внешний — Traefik: наружу только `frontend` (`/`) и `bff` (`/api`, `/ws`), прямые пути `/api/auth` и `/api/core` убираются и в compose, и в k3s. Внутренний — NetworkPolicy: к auth/core/commission ходит только BFF, к commission дополнительно core (единственное разрешённое межсервисное обращение, спека §3). Секреты уезжают в Kubernetes Secret, `deploy.yml` переписывается на матрицу вместо пяти скопированных блоков. Всё, что выкатывается, должно быть воспроизводимо: образ каждого сервиса тегируется sha, фронтовые теги приезжают из `frontend-tags.env`.

**Tech Stack:** Helm, Traefik (IngressRoute, Middleware), Kubernetes NetworkPolicy и Secret, GitHub Actions (matrix), Docker Compose.

**Spec:** `docs/superpowers/specs/2026-09-15-cbdc-hub-integration-design.md` — §3 (наружу только bff и frontend), §8 (Helm, NetworkPolicy, Secret, CD обоих репозиториев), §4.3 (внутренняя ручка эмулятора не публикуется).
**Вход из плана 07:** раздел «Перенесено из плана 07» в `docs/superpowers/plans/2026-09-15-00-roadmap.md` — прочитать целиком до начала работы, там требования, а не пожелания.

**Ограничение реальности.** Кластера k3s здесь нет, remote у репозиториев нет, образы не публикуются. Значит: всё проверяется `helm template`/`helm lint`, `docker compose config`, разбором YAML и локальным стендом. **Заявлять «выкатили» или «CI зелёный» запрещено** — только «отрендерено», «воспроизведено локально». Это не формальность: половина находок последних трёх планов была там, где кто-то принял непроверенное за проверенное.

## Global Constraints

- Backend-репозиторий `/home/legors/Documents/IVR`, ветка `feat/k3s-hardening` (создаётся в Task 1) от `main`. Frontend `/home/legors/Documents/alfa-cbdc-hub`, ветка `main` — только там, где задача прямо это разрешает; коммиты не смешивать.
- Наружу (Traefik, оба окружения) публикуются **только** `frontend` и `bff`. У auth, core, commission `ingress.enabled: false`.
- CORS `*` убирается: список источников задаётся переменной и по умолчанию содержит адреса стенда, а не звёздочку.
- Пароли БД и прочие секреты не хранятся в values открытым текстом. В k3s — `Secret`; в compose допустимо оставить как есть (локальный стенд), но это должно быть явно сказано, а не подразумеваться.
- Каждый сервис в compose обязан иметь healthcheck (правило плана 04). `--wait` должен что-то значить.
- `COOKIE_SECURE` и `ALLOWED_ORIGINS` — параметры, а не литералы, в обоих окружениях.
- Тесты, которые обязаны остаться зелёными: `make test-cpp` (46), `make test-api` (12), `make test-commission` (55), фронт `pnpm --dir frontend test` (69), e2e `pnpm --dir e2e test` (5 спек, локально с `--trace=off`).
- Никаких `git push`, `kubectl`, `helm install/upgrade` против кластера. `docker compose down` запрещён; контейнеры `registry`, `legors-db`, `tealdeer-db` не трогать.
- Сообщение коммита заканчивается строкой `Co-Authored-By: Claude <модель> <noreply@anthropic.com>` — модель та, что реально пишет коммит.

## File Structure

```
IVR/
├── infra/traefik/dynamic.yml            MOD  наружу только frontend и bff; CORS по списку
├── infra/helm/generic-service/
│   ├── templates/networkpolicy.yaml     MOD  ingress по podSelector, а не по всему namespace
│   ├── templates/secret.yaml            NEW  пароли БД и прочее
│   ├── templates/deployment.yaml        MOD  env из Secret, securityContext
│   └── values.yaml                      MOD  ingress.enabled=false по умолчанию, cors без "*"
├── infra/helm/values-bff.yaml           NEW
├── infra/helm/values-frontend.yaml      NEW
├── infra/helm/values-service-*.yaml     MOD  ingress выключен, пароли через Secret
├── infra/helm/frontend-tags.env         NEW  теги образов фронта (план 05/06)
├── docker-compose.yml                   MOD  traefik-роутинг, параметры
├── docker-compose.e2e.yml               NEW  EMULATOR_MANUAL=true как свойство набора файлов
├── Makefile                             MOD  k3s-цели на пять сервисов, e2e-стенд через новый оверлей
├── .github/workflows/deploy.yml         MOD  матрица вместо пяти блоков; публикация всех образов
└── README.md, docs/adr/                 MOD  ADR по решениям плана

alfa-cbdc-hub/ (Task 4)
├── .github/workflows/ci.yml             MOD  e2e через новый оверлей; тянуть backend-образы, не собирать
└── e2e/…                                MOD  один адрес вместо трёх (через nginx фронта)
```

---

### Task 1: Периметр — Traefik и NetworkPolicy

**Files:**
- Modify: `infra/traefik/dynamic.yml`, `infra/helm/generic-service/templates/networkpolicy.yaml`, `infra/helm/generic-service/values.yaml`, `infra/helm/values-service-auth.yaml`, `infra/helm/values-service-core.yaml`, `infra/helm/values-service-commission.yaml`, `docker-compose.yml`

**Interfaces:**
- Consumes: существующий чарт `generic-service`, compose-стенд.
- Produces: наружу доступны только `frontend` и `bff`; NetworkPolicy пускает к auth/core/commission только под BFF (и core → commission); `helm template` рендерит это для всех пяти сервисов.

- [ ] **Step 1: Ветка**

`git checkout main && git status --short && git checkout -b feat/k3s-hardening`

- [ ] **Step 2: Traefik в compose**

В `infra/traefik/dynamic.yml` убрать роутеры `auth` и `core` (сервисы в `services:` тоже, если больше не нужны) и добавить:
- роутер `bff` — `PathPrefix(/api) || PathPrefix(/ws)` → `http://bff:4000`;
- роутер `frontend` — `PathPrefix(/)` с низким приоритетом → `http://frontend:8080`.
Порядок важен: у Traefik при равной длине правил приоритет по длине, поэтому `/` должен иметь явно меньший `priority`, иначе он перехватит `/api`. Проверить это явно, а не понадеяться.
CORS: заменить `accessControlAllowOriginList: "*"` на список из переменной окружения Traefik или на статический список адресов стенда. Звёздочка вместе с cookie-сессией — плохая пара, даже если браузер её и так не примет с `credentials`.

- [ ] **Step 3: NetworkPolicy**

Сейчас шаблон пускает весь namespace `backend`. Переписать на `podSelector`: к сервису ходит только тот, кому положено. Список разрешённых источников вынести в values (`networkPolicy.allowFrom: [ {app: bff}, ... ]`), чтобы каждый сервис объявлял своё:
- auth ← bff;
- core ← bff;
- commission ← bff, core (единственное межсервисное обращение по спеке §3).
Не забыть `kube-system` для проб и DNS, иначе поды станут недоступны для kubelet — это ровно та ошибка, которая выглядит как «сервис не поднялся».

- [ ] **Step 4: Выключить ingress у внутренних сервисов**

В `values.yaml` чарта `ingress.enabled: false` по умолчанию; в values auth/core/commission выключить явно и убрать `match`. В values bff/frontend (Task 2) — включить.

- [ ] **Step 5: Проверка**

```bash
OUT=/tmp/claude-1000/-home-legors-Documents/e9e4492d-fa15-4d6f-b2aa-e1d1470f347c/scratchpad
for s in service-auth service-core service-commission; do
  bash infra/gen-migration-values.sh "$s" >/dev/null
  helm template "$s" infra/helm/generic-service \
    -f "infra/helm/values-$s.yaml" \
    -f "infra/helm/generated/migrations-$s.yaml" > "$OUT/r-$s.yaml" && echo "$s ok"
done
grep -c "IngressRoute" "$OUT/r-service-auth.yaml"   # ожидаем 0
grep -A5 "NetworkPolicy" -h "$OUT/r-service-core.yaml" | head -20
docker compose config >/dev/null && echo "compose ok"
```
(точное имя скрипта генерации values для миграций проверить в `Makefile` — использовать то, что там, а не это по памяти)
Expected: три `ok`, ни одного IngressRoute у внутренних сервисов, в NetworkPolicy виден `podSelector` источника, compose валиден.
Затем поднять стенд и проверить периметр вживую: `/api/auth/...` через Traefik больше не отвечает напрямую, а через BFF (`/api/auth/login`) — отвечает.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "feat(infra): expose only frontend and bff, narrow NetworkPolicy to real callers"
```

---

### Task 2: Секреты, values для bff и frontend, securityContext

**Files:**
- Create: `infra/helm/generic-service/templates/secret.yaml`, `infra/helm/values-bff.yaml`, `infra/helm/values-frontend.yaml`, `infra/helm/frontend-tags.env`
- Modify: `infra/helm/generic-service/templates/deployment.yaml`, `infra/helm/generic-service/values.yaml`, `infra/helm/values-service-*.yaml`

**Interfaces:**
- Consumes: чарт из Task 1.
- Produces: пароли БД через `Secret` (`envFrom`/`secretKeyRef`), values для BFF и фронта, `frontend-tags.env` с тегами их образов; `securityContext` с `runAsNonRoot`.

- [ ] **Step 1: Secret**

Шаблон `secret.yaml` рендерится при `secrets.enabled` и кладёт пары из `secrets.data` (значения приезжают через `--set` из CD или из внешнего файла — в репозиторий значения не коммитятся). В `deployment.yaml` пароль БД и прочее подтягивать через `secretKeyRef`, а не литералом в `env`.
Миграции тоже используют пароль — `migrations.db.password` должен браться оттуда же, иначе секрет наполовину бесполезен.
В values сервисов убрать открытые пароли, оставив ссылку на ключ секрета. В README честно написать: **в compose пароли остаются открытыми, это локальный стенд**; секрет — про k3s.

- [ ] **Step 2: values для bff и frontend**

`values-bff.yaml`: образ `ghcr.io/ivr-cbdc/frontend/bff`, порт 4000, `ingress.enabled: true` с `PathPrefix(/api) || PathPrefix(/ws)`, env (`AUTH_URL`, `CORE_URL`, `COMMISSION_URL`, `REDIS_URL`, `ALLOWED_ORIGINS`, `COOKIE_SECURE`), `readinessProbe` → `/ready`, `livenessProbe` → `/health` (разделение сделано в плане 05 — использовать его, а не проверять живость походом в апстримы).
`values-frontend.yaml`: образ `ghcr.io/ivr-cbdc/frontend/spa`, **порт 8080** (образ непривилегированный, план 06), `ingress.enabled: true` с `PathPrefix(/)` и низким приоритетом, пробы на `/`.
`frontend-tags.env` — файл с тегами обоих образов, который CD фронта будет обновлять (спека §8). Сейчас — с заглушкой `latest` и комментарием, что его пишет CI фронт-репозитория.

- [ ] **Step 3: securityContext**

В `deployment.yaml` добавить `securityContext` пода и контейнера: `runAsNonRoot: true`, `allowPrivilegeEscalation: false`, `readOnlyRootFilesystem` где возможно, `capabilities: drop: [ALL]`. Проверить, что C++-сервисы и nginx-образ это переживают (у фронта порт 8080 как раз поэтому). Если какой-то сервис не может — не выключать глобально, а отметить исключение с причиной.

- [ ] **Step 4: Проверка**

`helm template` для всех пяти сервисов; убедиться, что: у внутренних нет IngressRoute, у bff/frontend есть; пароль в манифесте приходит из `secretKeyRef`, а не строкой; `runAsNonRoot` присутствует; порт фронта 8080.
`helm lint` для всех пяти.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(helm): secrets for db credentials, values for bff and frontend, non-root pods"
```

---

### Task 3: Публикация образов и CD матрицей

**Files:**
- Modify: `.github/workflows/deploy.yml`, `Makefile`, `README.md`
- Create: `docs/adr/0006-two-perimeters.md` (или дополнить существующие ADR)

**Interfaces:**
- Consumes: values из Task 2.
- Produces: `deploy.yml` на матрице (пять сервисов вместо пяти скопированных блоков), публикация backend-образов на `main`, обновление k3s-целей `Makefile` под пять сервисов.

- [ ] **Step 1: Матрица в deploy.yml**

Заменить пять почти одинаковых блоков `helm upgrade` матрицей по имени сервиса, чтобы добавление шестого было строкой в списке. Теги backend-образов — sha (как сделано в плане 04); теги bff и frontend читаются из `infra/helm/frontend-tags.env`, который обновляет CD фронт-репозитория (спека §8).
Публикация backend-образов на `main` в ghcr — чтобы CI фронта мог их тянуть, а не пересобирать C++ на каждом PR (долг из плана 07).

- [ ] **Step 2: Makefile**

`K3S_SERVICES` расширить до пяти. Цели `k3s-build-%`/`k3s-deploy-%` должны работать и для bff/frontend, образы которых собираются в другом репозитории, — либо явно исключить их из локальной сборки с понятным сообщением. Не делать вид, что `make k3s-build` соберёт то, чего здесь нет.

- [ ] **Step 3: ADR**

Записать решения, которые иначе придётся восстанавливать по коммитам: два периметра (Traefik снаружи, NetworkPolicy внутри) и почему core разрешено ходить в commission; почему секреты только для k3s, а в compose пароли открыты; почему фронт слушает 8080.

- [ ] **Step 4: Проверка**

Разбор YAML обоих workflow; `make -n k3s-deploy` разворачивается осмысленно; `helm template` всех пяти по-прежнему рендерится. **В отчёте явно сказать, что ни один шаг деплоя не выполнялся** — кластера нет.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "ci(deploy): matrix over five services, publish backend images, ADRs"
```

---

### Task 4: Стенд e2e и фронтовый CI под новый периметр (меняет оба репозитория)

**Files (Backend):** `docker-compose.e2e.yml` (новый), `docker-compose.yml`, `docker-compose.dev.yml`, `Makefile`, `README.md`
**Files (Frontend, ветка `main`):** `.github/workflows/ci.yml`, `e2e/playwright.config.ts`, `e2e/fixtures/stand.ts`, `README.md`

**Interfaces:**
- Consumes: периметр из Task 1, образы из Task 3.
- Produces: `EMULATOR_MANUAL=true` как свойство файла, а не соглашения; e2e ходят через один адрес (nginx фронта проксирует `/api` и `/ws`); решение по ручке `tick` записано.

- [ ] **Step 1: Режим эмулятора структурно**

Завести `docker-compose.e2e.yml`, содержащий только `service-core.environment.EMULATOR_MANUAL: "true"`. Тогда любой `docker compose -f ... -f ...e2e.yml up` идемпотентен: и Makefile, и CI, и человек руками сходятся к одному. `make e2e-stand-up` перевести на него.
Дополнительно: отдавать `emulator_manual` в `/health` (или `/ready`) service-core и проверять это в `global-setup.ts` — тогда неверный режим виден одной строкой в начале прогона, а не 404 посреди теста. За план 07 на эту ловушку наступили дважды.

- [ ] **Step 2: Один адрес для e2e**

nginx фронта уже проксирует `/api` и `/ws` на BFF. Направить `E2E_BFF_URL` на `E2E_BASE_URL`, чтобы адрес был один и `Origin` совпадал по построению. Остаётся особый случай — `E2E_CORE_URL` для ручки `tick`.

- [ ] **Step 3: Решение по ручке tick — записать, а не подразумевать**

Ручка не публикуется наружу (спека §4.3), а NetworkPolicy из Task 1 закроет её и внутри. Выбрать и **записать в спеку** один из вариантов: (a) e2e навсегда живут на compose-стенде, в k3s не гоняются; (b) в k3s для них предусмотрен отдельный путь (port-forward в джобе). Вариант (a) честнее и дешевле — если выбираешь его, скажи в спеке прямо, что e2e не являются частью проверки кластерного деплоя, и что тогда проверяет деплой.

- [ ] **Step 4: CI фронта**

Тянуть backend-образы из ghcr вместо сборки C++ (долг из плана 07); поднимать стенд через новый оверлей. Помнить, что джоба по-прежнему не может быть выполнена здесь.

- [ ] **Step 5: Проверка**

Полный прогон: `make test-cpp`, `make test-api`, `make test-commission`, `pnpm --dir frontend test`, `pnpm --dir e2e test` (с `--trace=off`). Стенд поднять через новую цель и убедиться, что периметр из Task 1 не сломал e2e: они ходят через фронт, а не напрямую в BFF.
Отдельно проверить, что после `make e2e-stand-down` и обычного `make up` эмулятор снова в автоматическом режиме — та самая ловушка.

- [ ] **Step 6: Два коммита**

По одному в каждом репозитории; в отчёте оба хеша и явное указание, что ветка Backend остаётся невмёрженной.
