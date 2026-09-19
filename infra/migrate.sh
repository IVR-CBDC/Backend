#!/bin/sh
set -e

# Usage: migrate.sh [migrations_dir]
# Env: DB_HOST, DB_PORT, DB_NAME, DB_USER, DB_PASSWORD
MIGRATIONS_DIR="${1:-/app/migrations}"

export PGPASSWORD="${DB_PASSWORD}"
PSQL="psql -h ${DB_HOST} -p ${DB_PORT} -U ${DB_USER} -d ${DB_NAME} -v ON_ERROR_STOP=1 -X -q"

# Create schema_migrations table. CREATE TABLE IF NOT EXISTS is its own
# statement/transaction, but it's idempotent and harmless to race on.
${PSQL} -c "
CREATE TABLE IF NOT EXISTS schema_migrations (
  version   INTEGER PRIMARY KEY,
  filename  TEXT NOT NULL,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
"

# Apply unapplied migrations in order.
#
# Каждая миграция применяется ОДНИМ вызовом psql: BEGIN, захват
# pg_advisory_xact_lock, проверка "уже применена?", тело миграции и запись
# в schema_migrations — всё в одной транзакции, COMMIT в конце. Так apply
# и record не могут разъехаться (падение между ними в старой версии скрипта
# приводило к повторному применению при перезапуске).
#
# Блокировка обязана быть xact-уровня (pg_advisory_xact_lock), а не session-
# уровня: каждый вызов psql — отдельная сессия, и session-lock, взятый в
# одном вызове psql, исчезает вместе с ним до следующего вызова. Держать её
# внутри транзакции конкретной миграции — единственный честный способ,
# который реально сериализует параллельные запуски (два init-контейнера),
# не давая обоим одновременно решить "не применено" и продублировать работу.
#
# "Уже применена?" проверяется повторным SELECT из schema_migrations ПОСЛЕ
# взятия блокировки (внутри той же транзакции) — через psql \gset в
# переменную should_apply, вокруг тела миграции — psql \if/\endif. Это
# честнее, чем INSERT ... ON CONFLICT DO NOTHING до применения тела: тело
# миграции не идемпотентно само по себе (например ALTER TABLE ADD COLUMN
# без IF NOT EXISTS), поэтому нельзя выполнять его "на всякий случай" и
# полагаться только на конфликт при записи — его нужно вовсе не выполнять,
# если версия уже применена.
# F8 (final review, минор — заведомо не чиним здесь, отдельное решение):
# у psql-вызова ниже нет lock_timeout/statement_timeout. Если миграция
# зависнет (или просто долго идёт), она держит pg_advisory_xact_lock до
# конца своей транзакции — вторая реплика (второй init-контейнер) молча
# ждёт на SELECT pg_advisory_xact_lock(...) без вывода и без таймаута, а не
# падает с понятной ошибкой. Также: CREATE INDEX CONCURRENTLY невозможен
# внутри транзакции — этот скрипт оборачивает каждую миграцию в
# BEGIN/COMMIT, так что миграции, которым нужен CONCURRENTLY, сюда не
# впишутся без отдельного пути выполнения вне транзакции.
applied=0
# find вместо ls *.sql — не путает служебный вывод ls с именами файлов и
# не ломается на несуществующем MIGRATIONS_DIR за счёт 2>/dev/null.
for file in $(find "${MIGRATIONS_DIR}" -maxdepth 1 -name '*.sql' 2>/dev/null | sort); do
  filename=$(basename "$file")
  version=$(echo "$filename" | grep -oE '^[0-9]+' | sed 's/^0*//')

  if [ -z "$version" ]; then
    echo "SKIP: cannot parse version from ${filename}"
    continue
  fi

  output=$(${PSQL} -v version="${version}" -v filename="${filename}" -v migfile="${file}" <<'SQL'
BEGIN;
SELECT pg_advisory_xact_lock(hashtext('ivr_migrations')) AS _lock \gset
SELECT NOT EXISTS(SELECT 1 FROM schema_migrations WHERE version = :version) AS should_apply \gset
\if :should_apply
\echo APPLY: :filename ...
\i :migfile
INSERT INTO schema_migrations (version, filename) VALUES (:version, :'filename');
\else
\echo SKIP: :filename (already applied)
\endif
COMMIT;
SQL
)
  printf '%s\n' "$output"

  if printf '%s\n' "$output" | grep -q '^APPLY:'; then
    applied=$((applied + 1))
  fi
done

echo "Done. Applied ${applied} migration(s)."
