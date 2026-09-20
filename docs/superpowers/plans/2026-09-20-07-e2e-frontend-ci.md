# Playwright e2e и CI фронта — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Закрыть критерий готовности из спеки живым сквозным тестом: регистрация → сделка → сценарий с реальной комиссией → документы → `completed` в трекинге без перезагрузки страницы. Плюс CI фронт-репозитория, который гоняет юнит-тесты, сборку и e2e против поднятого стенда.

**Architecture:** E2E гоняются против настоящего стенда, а не против моков: только так проверяется то, ради чего всё делалось — что событие из Redis доезжает до экрана. Детерминизм даёт ручной режим эмулятора (`EMULATOR_MANUAL=true`): тест сам двигает время через `POST /internal/emulator/tick`, вместо того чтобы спать и надеяться. Каждый тест регистрирует свою компанию — это и изоляция данных, и проверка того, что арендаторы не видят друг друга.

**Tech Stack:** Playwright (chromium), TypeScript, GitHub Actions, Docker Compose.

**Spec:** `docs/superpowers/specs/2026-09-15-cbdc-hub-integration-design.md` — §1 «Критерий готовности», §9 строка «E2E (Playwright)», §8 «CI Frontend».
**Зацепки для тестов** уже расставлены планом 06: `data-testid="document-row"`+`data-doc-kind`, `data-testid="scenario-card"`+`data-scenario`, `data-app-status` на обёртке, `aria-current="step"`, `aria-label` карточек только с названием.

**Два репозитория.** Основная работа — `/home/legors/Documents/alfa-cbdc-hub` (ветка `main`). Task 3 меняет `/home/legors/Documents/IVR` в ветке `feat/e2e-support` — отдельный коммит.

## Global Constraints

- E2E гоняются против стенда, поднятого с `EMULATOR_MANUAL=true`: продвижение сделки инициирует **тест** вызовом `POST /internal/emulator/tick`, а не ожидание. `sleep`/`waitForTimeout` как способ дождаться прогресса запрещены — только `expect(...).toHaveText/toBeVisible` с автоожиданием или явное ожидание ответа.
- Каждый тест создаёт свою компанию (уникальные логин и ИНН). Общих фикстур с данными между тестами нет.
- Никаких `data-testid`, добавленных «по ходу»: если зацепки не хватает, это находка для отчёта, а не тихая правка SPA. Исключение — если план прямо разрешает в конкретном шаге.
- Тесты должны падать осмысленно: при падении сохраняются трассировка, скриншот и логи контейнеров.
- CI фронт-репозитория **нельзя запустить** (у репозиториев ещё нет remote, образы не публикуются). Workflow пишется правильно, проверяется локальным воспроизведением шагов и разбором YAML; в отчёте это заявляется прямо, без утверждений о «зелёном CI».
- Никаких `git push` и операций с кластером. Контейнеры `registry`, `legors-db`, `tealdeer-db` не трогать. Стенд не гасить (`docker compose down` запрещён) — только поднимать и перезапускать отдельные сервисы.
- Сообщение коммита заканчивается строкой `Co-Authored-By: Claude <модель> <noreply@anthropic.com>` — модель та, что реально пишет коммит.

## File Structure

```
alfa-cbdc-hub/
├── e2e/
│   ├── package.json            NEW  @playwright/test, отдельный пакет
│   ├── playwright.config.ts    NEW  chromium, baseURL, trace/screenshot on failure
│   ├── fixtures/stand.ts       NEW  регистрация компании, tick эмулятора, очистка localStorage
│   ├── happy-path.spec.ts      NEW  критерий готовности из спеки §1
│   ├── recovery.spec.ts        NEW  отказ по документу → переподача → completed
│   ├── isolation.spec.ts       NEW  вторая компания не видит чужую сделку
│   └── session.spec.ts         NEW  истёкшая сессия ведёт на вход (дефект из плана 06)
├── .github/workflows/ci.yml    NEW  typecheck+test+build (bff и frontend), затем e2e
└── README.md                   MOD  как гонять e2e локально

IVR/ (Backend, Task 3)
├── docker-compose.dev.yml      MOD  профиль/переменные для e2e-прогона
└── README.md                   MOD  как поднять стенд под e2e
```

---

### Task 1: Каркас Playwright и сквозной сценарий

**Files:**
- Create: `e2e/package.json`, `e2e/playwright.config.ts`, `e2e/fixtures/stand.ts`, `e2e/happy-path.spec.ts`
- Modify: `README.md`

**Interfaces:**
- Consumes: SPA на `http://127.0.0.1:8090`, BFF на `http://127.0.0.1:14000`, ручка `POST http://127.0.0.1:18081/internal/emulator/tick` (service-core, только при `EMULATOR_MANUAL=true`).
- Produces:
  - `fixtures/stand.ts`: фикстура `company` (регистрирует уникального пользователя через UI или API и возвращает данные), `tick(times?)` — двигает эмулятор, `clearDraft(page)` — чистит `localStorage["draft:new-deal"]`.
  - `pnpm --dir e2e test` гоняет сценарии против уже поднятого стенда; `pnpm --dir e2e test:ui` для отладки.

- [ ] **Step 1: Завести пакет**

`e2e/package.json` с `@playwright/test`, скриптами `test`, `test:ui`, `install-browsers`. Ставить только chromium (`pnpm exec playwright install --with-deps chromium`) — остальные движки для этого проекта не нужны и стоят минут в CI.
`playwright.config.ts`: `baseURL` из env `E2E_BASE_URL` (по умолчанию `http://127.0.0.1:8090`), `trace: "retain-on-failure"`, `screenshot: "only-on-failure"`, `video: "retain-on-failure"`, один воркер (`workers: 1`) — стенд общий, параллельные прогоны будут драться за эмулятор; `reporter: [["list"], ["html", {open: "never"}]]`.
**Не** поднимать стенд через `webServer`: стенд живёт в Backend-репозитории и управляется снаружи. Вместо этого в конфиг добавить `globalSetup`, который проверяет доступность SPA и BFF и падает с понятным сообщением («подними стенд: …»), если их нет.

- [ ] **Step 2: Фикстуры**

`company` — регистрирует компанию **через API BFF** (`POST /api/auth/register`), а не через UI: регистрация проверяется отдельным тестом, а здесь это подготовка, и она должна быть быстрой и надёжной. Возвращает `{login, password, inn, companyName}` и оставляет cookie в контексте браузера, чтобы тест начинался уже авторизованным. Уникальность — через `crypto.randomUUID()` в логине и случайные 10 цифр в ИНН.
`tick(times = 1)` — `POST /internal/emulator/tick` на service-core; если ручка отвечает 404, значит стенд поднят без `EMULATOR_MANUAL=true` — падать с понятным сообщением, а не таймаутом.
Очистка `localStorage["draft:new-deal"]` — в `beforeEach`, иначе черновик протечёт между тестами (предупреждение из плана 06).

- [ ] **Step 3: Написать `happy-path.spec.ts` — критерий готовности из спеки §1**

Один тест, проходящий весь путь, с проверками на каждом шаге (не «кликнули и ладно»):
1. Начинаем авторизованными (фикстура `company`), дашборд пуст — «Всего сделок: 0».
2. Создаём сделку через мастер: страна «Китай», импорт, сумма, валюта CNY, контрагент. Проверяем, что после создания на дашборде одна сделка и показана **«Китай»**, а не «CN».
3. Экран сценариев: карточки показывают **числовую сумму комиссии с валютой**; выбираем «Расчёт через ЦВЦБ»; проверяем, что после подтверждения сделка перешла к документам.
4. Экран документов: два документа в статусе «Отсутствует»; подаём оба; **двигаем эмулятор** и ждём, пока оба станут «Подтверждён» — **без перезагрузки страницы** (это и есть проверка живых обновлений).
5. Трекинг: двигаем эмулятор до `completed`; проверяем, что стадия и таймлайн обновились сами, а прогресс равен 100%.
Ни одного `waitForTimeout`: после каждого `tick` — `await expect(locator).toHaveText(...)` с автоожиданием.

- [ ] **Step 4: Запустить**

```bash
cd /home/legors/Documents/IVR
EMULATOR_MANUAL=true docker compose -f docker-compose.yml -f docker-compose.dev.yml --profile bff --profile frontend up -d --wait
cd /home/legors/Documents/alfa-cbdc-hub/e2e && pnpm install && pnpm exec playwright install --with-deps chromium && pnpm test
```
Expected: тест зелёный. Если стенд уже поднят с `EMULATOR_MANUAL=false`, пересоздать только service-core с новым значением (не гасить весь стенд).
**Важно:** прогнать тест дважды подряд. Второй прогон на том же стенде обязан быть зелёным — если нет, значит тест зависит от состояния, оставленного предыдущим.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "test(e2e): playwright harness and the spec's end-to-end acceptance path"
```

---

### Task 2: Сценарии восстановления, изоляции и сессии

**Files:**
- Create: `e2e/recovery.spec.ts`, `e2e/isolation.spec.ts`, `e2e/session.spec.ts`

**Interfaces:**
- Consumes: фикстуры из Task 1.
- Produces: три сценария, каждый закрывает конкретный дефект, найденный ранее в этом проекте.

- [ ] **Step 1: `recovery.spec.ts` — путь восстановления сделки**

План 03 обнаружил, что этот путь был недостижим: вердикт эмулятора зависел только от сделки и вида документа, поэтому переподача отклонялась снова, всегда. Тест должен закрепить, что это работает.
Эмулятор детерминирован по `(deal_id, kind, attempt)`, и **вторая попытка всегда успешна**, но какой документ отклонят на первой — предсказать из теста нельзя, не повторяя хеш-функцию в тестовом коде (план 03 такое повторение уже породил, и ревью справедливо назвало это дублированием продакшн-логики).
Поэтому: создаём сделки в цикле (не больше разумного числа, например 10), пока не попадётся та, где после первого прогона эмулятора хотя бы один документ отклонён. Если за 10 попыток не попалась — тест падает с внятным сообщением (это сигнал, что вероятность отказа изменилась). Дальше: видим причину отказа на экране, переподаём, двигаем эмулятор, документ одобрен, сделка доходит до `completed`.
Комментарием объяснить, почему цикл, а не воспроизведение хеша в тесте.

- [ ] **Step 2: `isolation.spec.ts` — арендаторы не видят друг друга**

Компания A создаёт сделку. В новом контексте браузера регистрируется компания B. Проверяем: на дашборде B сделки нет; прямой переход по URL сделки A показывает ошибку «не найдено», а не данные; переход на трекинг сделки A — то же самое.
Это единственная проверка изоляции, которую видит пользователь: pytest-тесты плана 03 проверяют API, а здесь — что SPA не покажет чужое даже при прямом вводе адреса.

- [ ] **Step 3: `session.spec.ts` — истёкшая сессия ведёт на вход**

Дефект из плана 06: при истечении cookie приложение оставалось в состоянии «авторизован», все экраны показывали ошибку, а `/login` отбрасывал обратно — выйти было нельзя без перезагрузки.
Тест: авторизуемся, удаляем cookie сессии через `context.clearCookies()` (это ровно то, что делает браузер, когда истекает `maxAge`), инициируем любое действие, требующее запроса, и проверяем, что оказались на экране входа и можем войти заново. Тест обязан падать на коде до исправления — если есть сомнения, проверить это, временно откатив правку в рабочей копии (и вернув обратно).

- [ ] **Step 4: Запустить всё**

`pnpm --dir e2e test` — все четыре файла зелёные, дважды подряд.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "test(e2e): recovery, tenant isolation and session expiry paths"
```

---

### Task 3: CI фронт-репозитория (меняет оба репозитория)

**Files (Frontend):** `.github/workflows/ci.yml`, `README.md`
**Files (Backend, ветка `feat/e2e-support`):** `docker-compose.dev.yml`, `README.md`

**Interfaces:**
- Consumes: `pnpm --dir bff test`, `pnpm --dir frontend test`, `pnpm --dir e2e test`, стенд из Backend-репозитория.
- Produces: workflow `CI` на `pull_request` и `push` (кроме `main`), джобы `bff`, `frontend`, `e2e`.

- [ ] **Step 1: Workflow**

Джобы:
- `bff` — `pnpm --dir bff install --frozen-lockfile`, `typecheck`, `test`, `build`.
- `frontend` — то же для `frontend`.
- `e2e` — нуждается в Backend-репозитории: `actions/checkout` второй раз с `repository: IVR-CBDC/Backend`, `path: backend`. Далее: сгенерировать ключи, собрать образы bff и frontend из этого репозитория, поднять стенд с `EMULATOR_MANUAL=true`, дождаться `--wait`, прогнать Playwright, при падении приложить трассировки и `docker compose logs` артефактами.
Учесть уроки плана 04: `BUILDX_BUILDER: default` на шаге, где compose собирает образы (иначе container-драйвер не увидит локально загруженные образы); `concurrency` с `cancel-in-progress`; кэш pnpm.
**Честно отметить в комментарии workflow и в отчёте:** джоба `e2e` не может выполниться, пока у репозиториев нет remote — чекаут Backend по имени провалится. Это не повод писать её неправильно.

- [ ] **Step 2: Backend — поддержка e2e**

В `docker-compose.dev.yml` (или отдельным блоком) обеспечить, чтобы прогон под e2e был одной командой: значения `EMULATOR_MANUAL=true`, профили `bff` и `frontend`. Если удобнее — добавить цель в `Makefile` (`make e2e-stand`), но не ломая существующие цели.
В README Backend — раздел «Стенд под e2e»: одна команда на подъём, одна на остановку профилей, и предупреждение, что `EMULATOR_MANUAL=true` останавливает автоматическое продвижение сделок (для ручной работы со стендом это не то, что нужно).

- [ ] **Step 3: Проверка**

Локально воспроизвести шаги каждой джобы: установка, typecheck, тесты, сборка, подъём стенда, прогон e2e. Разобрать оба YAML (`python3 -c "import yaml; ..."`), прогнать `bash -n` по встроенным скриптам.
В отчёте прямо написать, что именно не проверено (выполнение workflow на раннере) и почему.

- [ ] **Step 4: Два коммита — по одному в каждом репозитории**

В отчёте указать оба хеша и явно сказать, что ветка Backend остаётся невмёрженной.
