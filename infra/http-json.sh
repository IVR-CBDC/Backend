#!/usr/bin/env bash
# Обёртка над curl для smoke-целей Makefile.
#
# Зачем: `curl -s ... | jq` зеленеет на чём угодно — 401, 409, 502 проходят
# как успех, потому что код возврата берётся от jq. Цель, которая не краснеет
# на настоящей поломке, хуже отсутствующей: она создаёт ложную уверенность.
# Здесь тело печатается всегда, но код возврата определяется HTTP-статусом.
#
# Usage: http-json.sh <ожидаемый-код> <аргументы curl...>
#   bash infra/http-json.sh 200 -b cookies.txt http://localhost/api/auth/me
set -euo pipefail

expected="$1"
shift

body=$(mktemp)
# shellcheck disable=SC2064  # путь нужен раскрытый сейчас, а не при выходе
trap "rm -f '$body'" EXIT

# Ошибку самого curl (нет соединения, DNS) set -e поймает здесь же.
code=$(curl -sS -o "$body" -w '%{http_code}' "$@")

if command -v jq >/dev/null 2>&1; then
  jq . <"$body" 2>/dev/null || cat "$body"
else
  cat "$body"
fi

if [ "$code" != "$expected" ]; then
  echo "" >&2
  echo "ОЖИДАЛСЯ HTTP $expected, получен $code" >&2
  exit 1
fi
