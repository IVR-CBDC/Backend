#!/usr/bin/env bash
# Сквозной сценарий сделки одной командой: создать → выбрать сценарий →
# подать документы → дождаться эмулятора → напечатать финальную стадию и
# уведомления. Требует поднятый `make up` и TOKEN (см. `make test-login`).
#
# Идемпотентен: каждый запуск создаёт свою собственную сделку и ничего не
# удаляет — повторный запуск не требует очистки и не падает из-за состояния,
# оставшегося от предыдущего запуска.
#
# Обходит Traefik и бьёт напрямую в service-core на ivr_backend-net — порт
# 8081 traefik-дашборда на этой машине занят посторонним процессом (см.
# implementer-rules.md), а маршрут к сети всё равно нужен docker run'у.
set -euo pipefail

if [ -z "${TOKEN:-}" ]; then
  echo "set TOKEN=... (см. make test-login)" >&2
  exit 1
fi

CURL="docker run --rm --network ivr_backend-net curlimages/curl:8.10.1 -s"
CORE="http://service-core:8080"
AUTH_HEADER="Authorization: Bearer $TOKEN"

pretty() { python3 -m json.tool; }
# Reads JSON on stdin, walks the given dotted keys, prints the result.
# `$@` must actually reach python (a past bug here forgot it and silently
# printed the whole document instead of the field).
field() {
  python3 -c '
import sys, json
d = json.load(sys.stdin)
for k in sys.argv[1:]:
    d = d[k]
print(d)
' "$@"
}

# Mirrors services/service-core/src/emulator.cc's fnv1a/documentApproved:
# the emulator's verdict for a document is a deterministic function of
# (deal_id, kind) that never changes for that deal — a rejected document
# stays rejected forever, it cannot be usefully resubmitted. Since deal_id
# is a server-generated UUID, predict the verdict client-side and keep
# drawing new deals until we get one the emulator will approve in full —
# otherwise this script would non-deterministically end at `blocked`
# instead of `completed` on any given run.
approved_deal_id() {
  python3 -c '
import sys

FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for byte in data:
        h ^= byte
        h = (h * FNV_PRIME) & MASK64
    return h


def approved(deal_id: str, kind: str) -> bool:
    return fnv1a(f"{deal_id}:{kind}".encode()) % 10 != 0


deal_id = sys.argv[1]
sys.exit(0 if approved(deal_id, "contract") and approved(deal_id, "invoice") else 1)
' "$1"
}

create_deal() {
  $CURL -X POST "$CORE/api/core/deals" \
    -H "$AUTH_HEADER" -H 'Content-Type: application/json' \
    -d '{"counterparty_country":"CN","counterparty_name":"Trading Partner Co","operation_type":"import","amount":100000,"currency":"CNY"}'
}

echo "=== Создание сделки (пока не выпадет id, который эмулятор одобрит целиком) ==="
deal_json=""
deal_id=""
for attempt in $(seq 1 50); do
  deal_json="$(create_deal)"
  deal_id="$(echo "$deal_json" | field deal id)"
  if approved_deal_id "$deal_id"; then
    break
  fi
  deal_id=""
done
if [ -z "$deal_id" ]; then
  echo "Не удалось подобрать сделку за 50 попыток — что-то не так с эмулятором." >&2
  exit 1
fi
echo "$deal_json" | pretty
version="$(echo "$deal_json" | field deal version)"

echo ""
echo "=== Выбор сценария (cbdc) ==="
scenario_json="$($CURL -X POST "$CORE/api/core/deals/$deal_id/scenario" \
  -H "$AUTH_HEADER" -H 'Content-Type: application/json' \
  -d "{\"scenario\":\"cbdc\",\"version\":$version}")"
echo "$scenario_json" | pretty

echo ""
echo "=== Подача документов ==="
doc_ids="$(echo "$scenario_json" | python3 -c "
import sys, json
deal = json.load(sys.stdin)['deal']
for doc in deal['documents']:
    print(doc['id'])
")"
version="$(echo "$scenario_json" | field deal version)"
for doc_id in $doc_ids; do
  submit_json="$($CURL -X POST "$CORE/api/core/deals/$deal_id/documents/$doc_id/submit" \
    -H "$AUTH_HEADER" -H 'Content-Type: application/json' \
    -d "{\"version\":$version}")"
  echo "$submit_json" | pretty
  version="$(echo "$submit_json" | field deal version)"
done

echo ""
echo "=== Ожидание эмулятора (EMULATOR_SPEED=demo, до 90с) ==="
stage=""
for i in $(seq 1 30); do
  deal_json="$($CURL "$CORE/api/core/deals/$deal_id" -H "$AUTH_HEADER")"
  stage="$(echo "$deal_json" | field deal stage)"
  echo "  [$i] stage=$stage"
  [ "$stage" = "completed" ] && break
  [ "$stage" = "blocked" ] && break
  sleep 3
done

echo ""
echo "=== Финальная стадия сделки: $stage ==="
echo "$deal_json" | pretty

echo ""
echo "=== Уведомления ==="
$CURL "$CORE/api/core/notifications" -H "$AUTH_HEADER" | pretty

if [ "$stage" != "completed" ]; then
  echo "" >&2
  echo "ВНИМАНИЕ: сделка не дошла до completed за отведённое время (stage=$stage)." >&2
  exit 1
fi
