.PHONY: keys up down logs test-register test-login test-core test-test-python lsp openapi \
       new-cpp new-python k3s-install k3s-import-images k3s-setup \
       k3s-build k3s-deploy k3s-deploy-data up-k3s down-k3s k3s-status \
       k3s-test-health k3s-test-auth k3s-test-core

keys:
	bash infra/gen-keys.sh

up: keys
	docker compose up --build -d
	@echo ""
	@echo "Up:"
	@echo "  http://localhost/api/auth/...        -> service-auth"
	@echo "  http://localhost/api/core/...        -> service-core"
	@echo "  http://localhost/api/test-python/...  -> service-test-python"
	@echo "  http://localhost/health2             -> service-test-python health"
	@echo "  http://localhost:8081                -> traefik dashboard"

down:
	docker compose down -v

logs:
	docker compose logs -f service-auth service-core service-test-python

# === Smoke tests ===

test-register:
	curl -s -X POST http://localhost/api/auth/register \
		-H 'Content-Type: application/json' \
		-d '{"login":"egor","password":"hunter2","name":"Egor"}' | jq

test-login:
	curl -s -X POST http://localhost/api/auth/login \
		-H 'Content-Type: application/json' \
		-d '{"login":"egor","password":"hunter2"}' | jq

test-core:
	@if [ -z "$$TOKEN" ]; then echo "set TOKEN=..."; exit 1; fi
	curl -s -X POST http://localhost/api/core/compute \
		-H "Authorization: Bearer $$TOKEN" \
		-H 'Content-Type: application/json' \
		-d '{"n": 100}' | jq

test-test-python:
	@if [ -z "$$TOKEN" ]; then echo "set TOKEN=..."; exit 1; fi
	curl -s http://localhost/api/test-python/ping \
		-H "Authorization: Bearer $$TOKEN" | jq

test-health2:
	curl -s http://localhost/health2 | jq

lsp:
	cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	ln -sf build/compile_commands.json compile_commands.json

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
K3S_SERVICES := service-auth service-core service-test-python
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
	helm upgrade --install pg-test-python oci://registry-1.docker.io/bitnamicharts/postgresql \
		-n data \
		--set auth.username=test_python --set auth.password=test_python \
		--set auth.database=test_python --set primary.persistence.size=1Gi
	helm upgrade --install redis oci://registry-1.docker.io/bitnamicharts/redis \
		-n data \
		--set architecture=standalone --set auth.enabled=false \
		--set master.persistence.size=512Mi

# --- Deploy services ---
# Resolves PG ClusterIP at deploy time to bypass broken in-cluster DNS (VPN issue)
k3s-deploy-%:
	$(eval SVC_NAME := $(shell echo $* | sed 's/service-//'))
	$(eval PG_IP := $(shell kubectl get svc pg-$(SVC_NAME)-postgresql -n data -o jsonpath='{.spec.clusterIP}' 2>/dev/null))
	$(eval PG_HOST := pg-$(SVC_NAME)-postgresql.data.svc.cluster.local)
	@if [ -n "$(PG_IP)" ]; then \
		sed 's/$(PG_HOST)/$(PG_IP)/g' infra/helm/values-$*.yaml > /tmp/values-$*.yaml; \
		helm upgrade --install $* $(HELM_CHART) -f /tmp/values-$*.yaml -n backend; \
		rm -f /tmp/values-$*.yaml; \
	else \
		helm upgrade --install $* $(HELM_CHART) -f infra/helm/values-$*.yaml -n backend; \
	fi

k3s-deploy: k3s-deploy-data $(addprefix k3s-deploy-,$(K3S_SERVICES))

# --- Full cycle ---
up-k3s: k3s-build k3s-deploy

# --- Teardown ---
down-k3s:
	-helm uninstall service-auth service-core service-test-python -n backend 2>/dev/null
	-helm uninstall pg-auth pg-core pg-test-python redis -n data 2>/dev/null

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
	@curl -sf $(BASE_URL)/health2 | python3 -m json.tool
	@echo "All healthy!"

k3s-test-auth:
	@echo "=== Register ===" && \
	curl -s -X POST $(BASE_URL)/api/auth/register \
		-H 'Content-Type: application/json' \
		-d '{"login":"testuser","password":"testpass123","name":"Test User"}' | python3 -m json.tool && \
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
	echo "=== Compute (n=100) ===" && \
	curl -s -X POST $(BASE_URL)/api/core/compute \
		-H 'Content-Type: application/json' \
		-H "Authorization: Bearer $$TOKEN" \
		-d '{"n":100}' | python3 -m json.tool && \
	echo "" && \
	echo "=== Status ===" && \
	curl -s $(BASE_URL)/api/core/status \
		-H "Authorization: Bearer $$TOKEN" | python3 -m json.tool