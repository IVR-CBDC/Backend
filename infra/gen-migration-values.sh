#!/usr/bin/env bash
# Генерирует helm values с SQL-миграциями сервиса из services/<svc>/migrations/*.sql.
# Единственный источник правды — .sql файлы; вручную миграции в values не пишутся.
set -euo pipefail

svc="${1:?usage: gen-migration-values.sh <service-name>}"
root="$(cd "$(dirname "$0")/.." && pwd)"
src="${root}/services/${svc}/migrations"
script="${root}/infra/migrate.sh"
out_dir="${root}/infra/helm/generated"
out="${out_dir}/migrations-${svc}.yaml"

mkdir -p "${out_dir}"
shopt -s nullglob
files=("${src}"/*.sql)

{
  echo "# СГЕНЕРИРОВАНО infra/gen-migration-values.sh — не редактировать"
  echo "migrations:"
  # Единственный источник правды и для кода скрипта: чарт не хранит свою
  # копию infra/migrate.sh, а рендерит её из этого значения.
  echo "  script: |"
  sed 's/^/    /' "${script}"
  if [ "${#files[@]}" -eq 0 ]; then
    echo "  files: {}"
  else
    echo "  files:"
    for f in "${files[@]}"; do
      echo "    $(basename "${f}"): |"
      sed 's/^/      /' "${f}"
      echo ""
    done
  fi
} > "${out}"

echo "${out}"
