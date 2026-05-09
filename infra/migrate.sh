#!/bin/sh
set -e

# Usage: migrate.sh [migrations_dir]
# Env: DB_HOST, DB_PORT, DB_NAME, DB_USER, DB_PASSWORD
MIGRATIONS_DIR="${1:-/app/migrations}"

export PGPASSWORD="${DB_PASSWORD}"
PSQL="psql -h ${DB_HOST} -p ${DB_PORT} -U ${DB_USER} -d ${DB_NAME} -v ON_ERROR_STOP=1"

# Create schema_migrations table
${PSQL} -c "
CREATE TABLE IF NOT EXISTS schema_migrations (
  version   INTEGER PRIMARY KEY,
  filename  TEXT NOT NULL,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
"

# Apply unapplied migrations in order
applied=0
for file in $(ls "${MIGRATIONS_DIR}"/*.sql 2>/dev/null | sort); do
  filename=$(basename "$file")
  version=$(echo "$filename" | grep -oE '^[0-9]+' | sed 's/^0*//' )

  if [ -z "$version" ]; then
    echo "SKIP: cannot parse version from ${filename}"
    continue
  fi

  exists=$(${PSQL} -tAc "SELECT 1 FROM schema_migrations WHERE version = ${version}" 2>/dev/null || true)
  if [ "$exists" = "1" ]; then
    echo "SKIP: ${filename} (already applied)"
    continue
  fi

  echo "APPLY: ${filename} ..."
  ${PSQL} -1 -f "$file"
  ${PSQL} -c "INSERT INTO schema_migrations (version, filename) VALUES (${version}, '${filename}')"
  applied=$((applied + 1))
done

echo "Done. Applied ${applied} migration(s)."
