.PHONY: keys cpp-base up down logs test-register test-login test-me test-token test-health smoke-commission test-commission test-commission-db test-cpp test-api test-all smoke-deal lsp openapi \
       e2e-stand-up e2e-stand-down \
       new-cpp new-python k3s-install k3s-import-images k3s-setup \
       k3s-build k3s-deploy k3s-deploy-data up-k3s down-k3s k3s-status \
       k3s-test-health k3s-test-auth k3s-test-core

keys:
	bash infra/gen-keys.sh

# Общий базовый образ для C++/Drogon-сервисов — собирается один раз,
# сервисные сборки после этого не тратят ~15 минут на drogon/libjwt.
cpp-base:
	docker compose --profile build build cpp-base

up: keys
	@docker image inspect ivr-cpp-base:latest >/dev/null 2>&1 || $(MAKE) cpp-base
	docker compose up --build -d
	@echo ""
	@echo "Up:"
	@echo "  http://localhost/                    -> frontend (профиль frontend, см. make e2e-stand-up)"
	@echo "  http://localhost/api, /ws            -> bff (профиль bff)"
	@echo "  service-auth/core/commission         -> только внутри сети (план 08: наружу их нет)"
	@echo ""
	@echo "  bff/frontend под профилями: без них Traefik жив, /api и / отдадут 502."
	@echo "  Дашборд Traefik выключен по умолчанию: docker-compose.dashboard.yml."

down:
	docker compose down -v

logs:
	docker compose logs -f service-auth service-core service-commission

# === Smoke tests ===
#
# План 08 убрал прямые маршруты `/api/auth` и `/api/core` через Traefik:
# наружу торчат только frontend (`/`) и bff (`/api`, `/ws`). Цели ниже
# ходят в BFF, а не в сервисы напрямую. Две особенности BFF меняют их вид:
#
#  * `register`/`login` НЕ возвращают `token` в теле — токен уезжает в
#    HttpOnly-cookie `session` (спека §3.1). Поэтому здесь cookie jar, а не
#    `export TOKEN=...`: цели проверяют то, что реально происходит.
#  * мутирующие запросы проходят CSRF-проверку `Origin` против
#    ALLOWED_ORIGINS, поэтому заголовок `Origin` обязателен.
#
# Все цели ходят через infra/http-json.sh: HTTP-статус попадает в код
# возврата. `curl -s ... | jq` этого не даёт — 401 и 409 проходили бы как
# успех, а цель, которая не краснеет на поломке, хуже отсутствующей.
#
# Требуют поднятого профиля `bff` (обычный `make up` его не поднимает):
#   docker compose --profile bff up -d --wait bff     # или make e2e-stand-up
SMOKE_URL       ?= http://localhost
SMOKE_ORIGIN    ?= http://localhost
SMOKE_PASSWORD  ?= hunter22
COOKIE_JAR      ?= .smoke-cookies.txt
SMOKE_USER_FILE ?= .smoke-user.txt
HTTP_JSON       := bash infra/http-json.sh

# Логин уникален на каждый прогон: с фиксированным `egor` второй запуск на
# том же стенде получает 409 USER_EXISTS, и цель либо врёт зелёным, либо
# краснеет на ровном месте. Уникальный логин делает успешный путь реально
# проверяемым, а сам логин сохраняется для make test-login.
test-register:
	@login="egor-$$(date +%s)-$$$$"; inn=$$(shuf -i 1000000000-9999999999 -n1); \
	$(HTTP_JSON) 200 -c $(COOKIE_JAR) -X POST $(SMOKE_URL)/api/auth/register \
		-H 'Content-Type: application/json' -H 'Origin: $(SMOKE_ORIGIN)' \
		-d "{\"login\":\"$$login\",\"password\":\"$(SMOKE_PASSWORD)\",\"name\":\"Егор\",\"company_name\":\"ООО Ромашка $$inn\",\"inn\":\"$$inn\"}" && \
	printf '%s\n' "$$login" > $(SMOKE_USER_FILE) && \
	echo "логин $$login сохранён в $(SMOKE_USER_FILE) — его возьмёт make test-login"

test-login:
	@login=$$(cat $(SMOKE_USER_FILE) 2>/dev/null || true); \
	test -n "$$login" || { echo "нет $(SMOKE_USER_FILE) — сначала make test-register" >&2; exit 1; }; \
	$(HTTP_JSON) 200 -c $(COOKIE_JAR) -X POST $(SMOKE_URL)/api/auth/login \
		-H 'Content-Type: application/json' -H 'Origin: $(SMOKE_ORIGIN)' \
		-d "{\"login\":\"$$login\",\"password\":\"$(SMOKE_PASSWORD)\"}" && \
	echo "session-cookie сохранена в $(COOKIE_JAR) (см. make test-me, make test-token)"

test-me:
	@test -s $(COOKIE_JAR) || { echo "нет $(COOKIE_JAR) — сначала make test-login" >&2; exit 1; }
	@$(HTTP_JSON) 200 -b $(COOKIE_JAR) $(SMOKE_URL)/api/auth/me

# Достаёт JWT из cookie jar. Нужен тем целям, которые ходят в сервисы мимо
# BFF по внутренней сети (smoke-commission, smoke-deal) — там Bearer-токен
# по-прежнему единственный способ авторизоваться, BFF в этой цепочке нет.
# Netscape-формат cookie jar: поля 6 и 7 — имя и значение.
# Проверяем НЕПУСТОЙ ТОКЕН, а не непустой файл: curl пишет в jar заголовок
# Netscape, поэтому `test -s` проходит и тогда, когда строки `session` там нет
# (логин не прошёл, cookie не поставлена) — цель напечатала бы пустоту и
# отрапортовала успех.
test-token:
	@token=$$(awk '$$6=="session"{print $$7}' $(COOKIE_JAR) 2>/dev/null); \
	test -n "$$token" || { echo "в $(COOKIE_JAR) нет cookie session — сначала make test-login" >&2; exit 1; }; \
	printf '%s\n' "$$token"

# /health сервисов наружу не публикуется и BFF его не проксирует (спека §3:
# наружу только `/api` и `/ws`), поэтому здоровье смотрим на host-портах,
# которые публикует docker-compose.dev.yml. Через Traefik такой проверки
# больше нет и быть не должно.
# ВАЖНО: эти порты публикует ТОЛЬКО docker-compose.dev.yml. После обычного
# `make up` их нет — поднимайте стенд с оверлеем (`make e2e-stand-up`,
# `make test-api`) или добавьте `-f docker-compose.dev.yml` вручную.
test-health:
	@echo "service-auth:" && $(HTTP_JSON) 200 http://127.0.0.1:18080/health
	@echo "service-core:" && $(HTTP_JSON) 200 http://127.0.0.1:18081/health
	@echo "bff (/ready):" && $(HTTP_JSON) 200 http://127.0.0.1:14000/ready

test-commission:
	cd services/service-commission && uv run pytest -q -m "not db"

test-commission-db:
	@docker image inspect ivr-cpp-base:latest >/dev/null 2>&1 || $(MAKE) cpp-base
	docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d pg-commission migrate-commission
	docker compose -f docker-compose.yml -f docker-compose.dev.yml wait migrate-commission
	cd services/service-commission && \
		TEST_PG_DSN=postgresql+asyncpg://commission:commission@127.0.0.1:5434/commission \
		uv run pytest -q -m db

smoke-commission:
	@if [ -z "$$TOKEN" ]; then echo "set TOKEN=\$$(make -s test-token)"; exit 1; fi
	docker run --rm --network ivr_backend-net curlimages/curl:8.10.1 -s \
		-X POST http://service-commission:8000/api/commission/quotes \
		-H "Authorization: Bearer $$TOKEN" -H 'Content-Type: application/json' \
		-d '{"from_country":"RU","to_country":"CN","currency":"CNY","amount":100000}'

# Поднимает стенд в детерминированном режиме эмулятора (EMULATOR_MANUAL=true,
# см. docker-compose.yml) и гоняет tests/api против него через проброшенные
# порты 18080/18081 — доступные только с docker-compose.dev.yml (host-порты
# не публикуются в обычном docker-compose.yml, см. Task 3).
test-api:
	@docker image inspect ivr-cpp-base:latest >/dev/null 2>&1 || $(MAKE) cpp-base
	EMULATOR_MANUAL=true docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d --build \
		pg-auth migrate-auth service-auth \
		pg-core migrate-core service-core \
		pg-commission migrate-commission service-commission redis
	cd tests/api && uv run --with httpx --with pytest pytest -q

# ============================================================
# e2e (план 07, задача 3, Frontend-репозиторий): полный стенд для Playwright
# из ../alfa-cbdc-hub/e2e — auth/core/commission/redis (как test-api) плюс
# bff/frontend под своими профилями, с EMULATOR_MANUAL=true.
#
# Ловушка, найденная при отладке плана 07 (задача 2): единственным способом
# поднять такой стенд руками было набрать
#   EMULATOR_MANUAL=true docker compose ... --profile bff --profile frontend up -d --wait
# — а если потом (по любой причине, например пересобрать/поднять только
# bff/frontend после правки) выполнить тот же `up --profile bff --profile
# frontend ...` ЕЩЁ РАЗ без префикса EMULATOR_MANUAL=true, compose
# пересчитывает желаемое состояние service-core (он не под профилем
# bff/frontend, входит в дефолтный набор, но пересчитывается при каждом up
# того же проекта) — и раз в этот раз EMULATOR_MANUAL не передана,
# пересоздаёт контейнер со значением по умолчанию (false, см.
# docker-compose.yml: `EMULATOR_MANUAL: "${EMULATOR_MANUAL:-false}"`).
# Результат — тихая потеря детерминированности уже идущих e2e, без единого
# предупреждения.
#
# Эта цель — единственный поддерживаемый способ поднять стенд под e2e:
# EMULATOR_MANUAL=true зашита в саму команду (а не в переменную окружения
# вызывающего), поэтому её невозможно забыть при повторном вызове — сколько
# раз `make e2e-stand-up` ни выполни подряд, service-core всегда пересоздаётся
# (если вообще пересоздаётся) с одним и тем же значением.
#
# BFF_IMAGE/FRONTEND_IMAGE — чтобы поднять bff/frontend из уже собранных
# образов (например, из Frontend CI, см. .github/workflows/ci.yml того
# репозитория), не из ghcr-плейсхолдера по умолчанию:
#   BFF_IMAGE=bff-ci:latest FRONTEND_IMAGE=frontend-ci:latest make e2e-stand-up
# Без них компоуз попробует стянуть ghcr.io/ivr-cbdc/frontend/{bff,spa} —
# либо собери их локально через docker-compose.override.yml.example (см.
# README, раздел «BFF»), либо передай свои теги как выше.
# ============================================================
e2e-stand-up: keys
	@docker image inspect ivr-cpp-base:latest >/dev/null 2>&1 || $(MAKE) cpp-base
	EMULATOR_MANUAL=true docker compose -f docker-compose.yml -f docker-compose.dev.yml \
		--profile bff --profile frontend up -d --build --wait
	@echo ""
	@echo "Стенд под e2e поднят (EMULATOR_MANUAL=true):"
	@echo "  SPA: http://127.0.0.1:8090"
	@echo "  BFF: http://127.0.0.1:14000"

# Останавливает только bff/frontend — не трогает остальной стенд (auth/core/
# commission/БД), он может быть нужен для чего-то ещё (make test-api и т.п.).
#
# F8 (план 07, final review): эта цель НЕ возвращает service-core в
# автоматический режим — EMULATOR_MANUAL=true, выставленный e2e-stand-up,
# так и остаётся на контейнере. Разработчик, вернувшийся к ручной работе со
# стендом после e2e, увидит сделки, которые сами никуда не двигаются, без
# единой ошибки (тикать некому — воркфлоу тика нет, фоновый цикл выключен).
# Печатаем это явно вместо тихого пересоздания core с EMULATOR_MANUAL=false
# — пересоздание само по себе может быть нежелательным посреди чужой сессии
# отладки (например, если e2e-stand-down вызван между двумя e2e-прогонами).
e2e-stand-down:
	docker compose -f docker-compose.yml -f docker-compose.dev.yml \
		--profile bff --profile frontend stop bff frontend
	@echo ""
	@echo "bff/frontend остановлены. service-core остаётся в EMULATOR_MANUAL=true"
	@echo "(сделки не будут двигаться сами по времени). Чтобы вернуть живой"
	@echo "автопрогресс: EMULATOR_MANUAL=false docker compose -f docker-compose.yml \\"
	@echo "  -f docker-compose.dev.yml up -d --wait service-core"

# Три набора, что гоняет CI на каждый PR (см. .github/workflows/ci.yml).
# test-commission-db сюда намеренно не входит: ему нужен поднятый
# pg-commission с host-портом 5434, а не только `uv sync` — см. README.
test-all: test-cpp test-commission test-api
	@echo "Все наборы тестов пройдены"

smoke-deal:
	@if [ -z "$$TOKEN" ]; then echo "set TOKEN=..."; exit 1; fi
	bash infra/smoke-deal.sh

lsp:
	cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	ln -sf build/compile_commands.json compile_commands.json

# RelWithDebInfo — потому что при Debug CMakeLists сервисов включают ASan/UBSan,
# и посторонние утечки в libjwt/drogon валят прогон; санитайзеры остаются
# доступны через явный -DCMAKE_BUILD_TYPE=Debug.
test-cpp:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTS=ON
	cmake --build build -j$(shell nproc)
	ctest --test-dir build --output-on-failure

openapi:
	bash infra/gen-openapi.sh

new-cpp:
	@if [ -z "$(name)" ]; then echo "Использование: make new-cpp name=<name>"; exit 1; fi
	bash infra/new-service-cpp.sh $(name)

new-python:
	@if [ -z "$(name)" ]; then echo "Использование: make new-python name=<name>"; exit 1; fi
	bash infra/new-service-python.sh $(name)

# ==============================================================================
# k3s setup
# ==============================================================================

k3s-install:
	bash infra/k3s-install.sh

k3s-import-images:
	@echo "=== Импорт системных образов через Docker (для VPN) ==="
	@for img in rancher/mirrored-coredns-coredns:1.14.2 \
	            rancher/mirrored-pause:3.6 \
	            rancher/mirrored-library-traefik:3.6.13 \
	            bitnami/postgresql:latest \
	            bitnami/os-shell:latest \
	            bitnami/redis:latest \
	            postgres:16; do \
		echo "[pull] $$img..." && \
		docker pull $$img && \
		echo "[save] $$img..." && \
		docker save $$img | sudo k3s ctr images import - && \
		echo "[ok]   $$img" && echo ""; \
	done
	@echo "Все образы импортированы!"

k3s-setup: k3s-install k3s-import-images keys k3s-build k3s-deploy
	@echo ""
	@echo "=== k3s полностью настроен ==="
	@echo "  make k3s-status    — статус подов"
	@echo "  make k3s-test-health — проверка сервисов"

# ==============================================================================
# k3s targets
# ==============================================================================

REGISTRY   := localhost:5000
K3S_SERVICES := service-auth service-core service-commission
HELM_CHART := infra/helm/generic-service

# --- Build & Push to local registry ---
k3s-build-%:
	docker build -t $(REGISTRY)/$*:latest -f services/$*/Dockerfile .
	docker push $(REGISTRY)/$*:latest

k3s-build: $(addprefix k3s-build-,$(K3S_SERVICES))

# --- Deploy data layer (Bitnami charts) ---
k3s-deploy-data:
	helm upgrade --install pg-auth oci://registry-1.docker.io/bitnamicharts/postgresql \
		-n data --create-namespace \
		--set auth.username=auth --set auth.password=auth --set auth.database=auth \
		--set primary.persistence.size=1Gi
	helm upgrade --install pg-core oci://registry-1.docker.io/bitnamicharts/postgresql \
		-n data \
		--set auth.username=core --set auth.password=core --set auth.database=core \
		--set primary.persistence.size=1Gi
	helm upgrade --install pg-commission oci://registry-1.docker.io/bitnamicharts/postgresql \
		-n data \
		--set auth.username=commission --set auth.password=commission \
		--set auth.database=commission --set primary.persistence.size=1Gi
	helm upgrade --install redis oci://registry-1.docker.io/bitnamicharts/redis \
		-n data \
		--set architecture=standalone --set auth.enabled=false \
		--set master.persistence.size=512Mi

# --- Deploy services ---
# Resolves PG ClusterIP at deploy time to bypass broken in-cluster DNS (VPN issue)
k3s-deploy-%:
	bash infra/gen-migration-values.sh $*
	$(eval SVC_NAME := $(shell echo $* | sed 's/service-//'))
	$(eval PG_IP := $(shell kubectl get svc pg-$(SVC_NAME)-postgresql -n data -o jsonpath='{.spec.clusterIP}' 2>/dev/null))
	$(eval PG_HOST := pg-$(SVC_NAME)-postgresql.data.svc.cluster.local)
	@if [ -n "$(PG_IP)" ]; then \
		sed 's/$(PG_HOST)/$(PG_IP)/g' infra/helm/values-$*.yaml > /tmp/values-$*.yaml; \
		helm upgrade --install $* $(HELM_CHART) -f /tmp/values-$*.yaml -f infra/helm/generated/migrations-$*.yaml -n backend; \
		rm -f /tmp/values-$*.yaml; \
	else \
		helm upgrade --install $* $(HELM_CHART) -f infra/helm/values-$*.yaml -f infra/helm/generated/migrations-$*.yaml -n backend; \
	fi

k3s-deploy: k3s-deploy-data $(addprefix k3s-deploy-,$(K3S_SERVICES))

# --- Full cycle ---
up-k3s: k3s-build k3s-deploy

# --- Teardown ---
down-k3s:
	-helm uninstall service-auth service-core service-commission -n backend 2>/dev/null
	-helm uninstall pg-auth pg-core pg-commission redis -n data 2>/dev/null

# --- Logs ---
k3s-logs-%:
	kubectl logs -n backend -l app.kubernetes.io/name=$* -f --tail=100

# --- Status ---
k3s-status:
	@echo "=== backend ==="
	kubectl get pods -n backend
	@echo ""
	@echo "=== data ==="
	kubectl get pods -n data

# --- Smoke tests ---
#
# План 08: в кластере наружу опубликованы только IngressRoute фронта (`/`) и
# BFF (`/api`, `/ws`); у auth/core/commission `ingress.enabled: false`, и
# NetworkPolicy пускает к ним только BFF. Поэтому цели ниже ходят через BFF
# и пользуются cookie-сессией, а не `token` из тела (спека §3.1).
BASE_URL     := http://localhost
K3S_ORIGIN   ?= $(BASE_URL)
K3S_COOKIES  ?= .k3s-smoke-cookies.txt
K3S_USER_FILE ?= .k3s-smoke-user.txt

# /health каждого сервиса в кластере проверяют readiness/liveness-пробы
# (`kubectl get pods -n backend`, цель k3s-status), а не внешний маршрут:
# такого маршрута больше нет и не должно быть. Снаружи проверяем ровно то,
# что опубликовано — фронт отдаёт SPA, BFF отвечает на /api.
k3s-test-health:
	@echo "=== frontend (/) ===" && \
	curl -sS --fail -o /dev/null -w "HTTP %{http_code}\n" $(BASE_URL)/
	@echo "=== bff (/api/auth/me без сессии — ожидаем 401) ===" && \
	$(HTTP_JSON) 401 $(BASE_URL)/api/auth/me
	@echo "Здоровье самих сервисов: make k3s-status (пробы), наружу его нет."

# Логин уникален на прогон — иначе второй запуск ловит 409 USER_EXISTS и
# цель либо врёт зелёным, либо краснеет на ровном месте.
k3s-test-auth:
	@login="k3s-$$(date +%s)-$$$$"; inn=$$(shuf -i 1000000000-9999999999 -n1); \
	echo "=== Register ($$login) ===" && \
	$(HTTP_JSON) 200 -c $(K3S_COOKIES) -X POST $(BASE_URL)/api/auth/register \
		-H 'Content-Type: application/json' -H 'Origin: $(K3S_ORIGIN)' \
		-d "{\"login\":\"$$login\",\"password\":\"$(SMOKE_PASSWORD)\",\"name\":\"Test User\",\"company_name\":\"OOO Test $$inn\",\"inn\":\"$$inn\"}" && \
	printf '%s\n' "$$login" > $(K3S_USER_FILE) && \
	echo "" && \
	echo "=== Login (токен уезжает в HttpOnly-cookie, не в тело) ===" && \
	$(HTTP_JSON) 200 -c $(K3S_COOKIES) -X POST $(BASE_URL)/api/auth/login \
		-H 'Content-Type: application/json' -H 'Origin: $(K3S_ORIGIN)' \
		-d "{\"login\":\"$$login\",\"password\":\"$(SMOKE_PASSWORD)\"}" && \
	echo "" && \
	echo "=== Me (по cookie) ===" && \
	$(HTTP_JSON) 200 -b $(K3S_COOKIES) $(BASE_URL)/api/auth/me

# Тело запроса — camelCase: это контракт BFF (createDealBodySchema в
# bff/src/routes/deals.ts), а не service-core, который принимает snake_case.
# Ответ на создание — 201, не 200.
k3s-test-core:
	@login=$$(cat $(K3S_USER_FILE) 2>/dev/null || true); \
	test -n "$$login" || { echo "нет $(K3S_USER_FILE) — сначала make k3s-test-auth" >&2; exit 1; }; \
	echo "=== Login for session ($$login) ===" && \
	$(HTTP_JSON) 200 -c $(K3S_COOKIES) -X POST $(BASE_URL)/api/auth/login \
		-H 'Content-Type: application/json' -H 'Origin: $(K3S_ORIGIN)' \
		-d "{\"login\":\"$$login\",\"password\":\"$(SMOKE_PASSWORD)\"}" >/dev/null && \
	echo "=== Create deal (через BFF: /api/deals, не /api/core/deals) ===" && \
	$(HTTP_JSON) 201 -b $(K3S_COOKIES) -X POST $(BASE_URL)/api/deals \
		-H 'Content-Type: application/json' -H 'Origin: $(K3S_ORIGIN)' \
		-d '{"counterpartyCountry":"CN","counterpartyName":"Trading Partner Co","operationType":"import","amount":100000,"currency":"CNY"}' && \
	echo "" && \
	echo "=== List deals ===" && \
	$(HTTP_JSON) 200 -b $(K3S_COOKIES) $(BASE_URL)/api/deals
