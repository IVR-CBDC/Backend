.PHONY: keys cpp-base up down logs test-register test-login test-me smoke-commission test-commission test-commission-db test-cpp test-api test-all smoke-deal lsp openapi \
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
	@echo "  http://localhost/api/auth/...        -> service-auth"
	@echo "  http://localhost/api/core/...        -> service-core"
	@echo "  service-commission                   -> только внутри сети (make smoke-commission)"
	@echo "  http://localhost:8081                -> traefik dashboard"

down:
	docker compose down -v

logs:
	docker compose logs -f service-auth service-core service-commission

# === Smoke tests ===

test-register:
	curl -s -X POST http://localhost/api/auth/register \
		-H 'Content-Type: application/json' \
		-d '{"login":"egor","password":"hunter22","name":"Егор","company_name":"ООО Ромашка","inn":"7707083893"}' | jq

test-login:
	curl -s -X POST http://localhost/api/auth/login \
		-H 'Content-Type: application/json' \
		-d '{"login":"egor","password":"hunter22"}' | jq

test-me:
	@if [ -z "$$TOKEN" ]; then echo "set TOKEN=..."; exit 1; fi
	curl -s http://localhost/api/auth/me -H "Authorization: Bearer $$TOKEN" | jq

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
	@if [ -z "$$TOKEN" ]; then echo "set TOKEN=..."; exit 1; fi
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
BASE_URL := http://localhost

k3s-test-health:
	@echo "=== Health checks ==="
	@curl -sf $(BASE_URL)/api/auth/health | python3 -m json.tool
	@curl -sf $(BASE_URL)/api/core/health | python3 -m json.tool
	@echo "All healthy!"

k3s-test-auth:
	@echo "=== Register ===" && \
	curl -s -X POST $(BASE_URL)/api/auth/register \
		-H 'Content-Type: application/json' \
		-d '{"login":"testuser","password":"testpass123","name":"Test User","company_name":"ООО Тест","inn":"7710137066"}' | python3 -m json.tool && \
	echo "" && \
	echo "=== Login ===" && \
	curl -s -X POST $(BASE_URL)/api/auth/login \
		-H 'Content-Type: application/json' \
		-d '{"login":"testuser","password":"testpass123"}' | python3 -m json.tool

k3s-test-core:
	@echo "=== Login for token ===" && \
	TOKEN=$$(curl -s -X POST $(BASE_URL)/api/auth/login \
		-H 'Content-Type: application/json' \
		-d '{"login":"testuser","password":"testpass123"}' | python3 -c "import sys,json;print(json.load(sys.stdin)['token'])") && \
	echo "Token: $$TOKEN" && \
	echo "" && \
	echo "=== Create deal ===" && \
	curl -s -X POST $(BASE_URL)/api/core/deals \
		-H 'Content-Type: application/json' \
		-H "Authorization: Bearer $$TOKEN" \
		-d '{"counterparty_country":"CN","counterparty_name":"Trading Partner Co","operation_type":"import","amount":100000,"currency":"CNY"}' \
		| python3 -m json.tool && \
	echo "" && \
	echo "=== List deals ===" && \
	curl -s $(BASE_URL)/api/core/deals \
		-H "Authorization: Bearer $$TOKEN" | python3 -m json.tool