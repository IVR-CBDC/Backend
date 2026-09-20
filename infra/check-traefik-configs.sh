#!/usr/bin/env bash
# Guard против молчаливого расхождения двух статических конфигов Traefik.
#
# infra/traefik/traefik.dashboard.yml — копия traefik.yml, которая обязана
# отличаться ТОЛЬКО блоком `api:` (оверлей docker-compose.dashboard.yml
# подменяет им основной конфиг, чтобы включить дашборд). Связать файлы
# средствами Traefik нельзя: при `--configFile` он игнорирует и CLI-флаги, и
# переменные окружения, а include'ов у статического конфига нет. Значит новый
# entryPoint, другой уровень логов или другой путь к dynamic.yml, добавленные
# в один файл, разъедутся с другим молча — и всплывут на защите.
#
# Проверка: вырезаем комментарии, пустые строки и блок `api:` — остаток должен
# совпадать побайтово.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
base="$root/infra/traefik/traefik.yml"
dash="$root/infra/traefik/traefik.dashboard.yml"

# Без этой проверки awk молча отдал бы пустой вывод на несуществующем файле,
# а diff двух пустот — «всё совпало». Проверка, которая зеленеет от собственной
# поломки, бесполезна.
for f in "$base" "$dash"; do
  if [ ! -s "$f" ]; then
    echo "нет файла или он пуст: $f" >&2
    exit 1
  fi
done

strip() {
  awk '
    /^api:/            { skip = 1; next }   # начало блока api
    /^[^[:space:]]/    { skip = 0 }         # любой следующий ключ верхнего уровня
    skip               { next }
    /^[[:space:]]*#/   { next }             # комментарии
    /^[[:space:]]*$/   { next }             # пустые строки
    { print }
  ' "$1"
}

if diff -u <(strip "$base") <(strip "$dash") >/dev/null; then
  echo "ok: traefik.yml и traefik.dashboard.yml различаются только блоком api:"
  exit 0
fi

echo "РАСХОЖДЕНИЕ между traefik.yml и traefik.dashboard.yml (вне блока api:):" >&2
diff -u <(strip "$base") <(strip "$dash") >&2 || true
echo "" >&2
echo "Оверлей дашборда подменяет весь статический конфиг, поэтому всё, кроме" >&2
echo "блока api:, обязано совпадать. Перенесите изменение во второй файл." >&2
exit 1
