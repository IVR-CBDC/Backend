#!/bin/sh
# Проверки, которые обязаны пройти ДО первого `helm upgrade`.
#
# Зачем отдельным скриптом, а не строчками в Makefile и в workflow: проверки
# нужны обоим механизмам выката (make k3s-deploy и .github/workflows/deploy.yml)
# и обязаны быть одинаковыми. Два независимых набора проверок разъезжаются так
# же молча, как разъезжались два списка сервисов до infra/services.tsv.
#
# Зачем ДО первого upgrade, а не по ходу цикла: выкат последовательный
# (порядок auth → … → bff → frontend обязателен), и обрыв на третьем сервисе
# оставляет стенд в смешанном состоянии — два релиза новые, три старые. Все
# проверки прогоняются по всем строкам, потом выкатывается.
#
# Использование:
#   sh infra/k3s-preflight.sh <путь к services.tsv> [сервис ...]
#
# Пароли БД читаются ИЗ ОКРУЖЕНИЯ по именам из последней колонки tsv:
# локально их экспортирует Makefile из infra/helm/secrets-k3s.env, в CD они
# приезжают из secrets репозитория. Скрипт не знает и не должен знать, откуда
# они взялись.
#
# kubectl используется только для чтения и только если он есть и отвечает:
# без кластера проверка пароля против базы пропускается (и говорит об этом),
# но остальные проверки работают всегда.
set -eu

TSV="${1:?первым аргументом — путь к services.tsv}"
shift

INFRA_DIR=$(dirname "$TSV")
HELM_DIR="$INFRA_DIR/helm"
TAGS_FILE="$HELM_DIR/frontend-tags.env"

# Теги, которые не фиксируют содержимое. Для bff и frontend такой тег ХУЖЕ
# пустого: пустой обрывает выкат, а неизменяемый делает `helm upgrade` no-op —
# новая сборка фронта молча не выкатывается, а выкат считается успешным.
MUTABLE_TAGS="latest main master edge dev stable"

if [ ! -s "$TSV" ]; then
    echo "preflight: $TSV пуст или нечитаем — список сервисов не прочитан." >&2
    echo "Это не 'нечего выкатывать', это сломанный выкат: продолжать нельзя." >&2
    exit 1
fi

if [ -f "$TAGS_FILE" ]; then
    # shellcheck disable=SC1090
    . "$TAGS_FILE"
fi

ROWS=$(mktemp)
trap 'rm -f "$ROWS"' EXIT
if [ "$#" -gt 0 ]; then
    for want in "$@"; do
        awk -v s="$want" '!/^[[:space:]]*#/ && NF && $1==s' "$TSV" >> "$ROWS"
        if ! awk -v s="$want" '!/^[[:space:]]*#/ && NF && $1==s {found=1} END {exit !found}' "$TSV"; then
            echo "preflight: неизвестный сервис '$want' — строки нет в $TSV" >&2
            exit 1
        fi
    done
else
    awk '!/^[[:space:]]*#/ && NF' "$TSV" > "$ROWS"
fi

fail=0
note() { echo "preflight: $*" >&2; }

while read -r name build cppbase mig tagsrc pwvar; do
    # Колонки build/cpp-base/migrations читаются, чтобы позиционно добраться
    # до tagsrc и pwvar; самим проверкам они не нужны.
    : "$build" "$cppbase" "$mig"

    if [ ! -f "$HELM_DIR/values-$name.yaml" ]; then
        note "$name: нет $HELM_DIR/values-$name.yaml"
        fail=1
    fi

    # --- тег образа ---
    if [ "$tagsrc" != sha ]; then
        eval "tag=\${$tagsrc-}"
        if [ -z "$tag" ]; then
            note "$name: $tagsrc пуст или отсутствует в $TAGS_FILE"
            fail=1
        else
            for m in $MUTABLE_TAGS; do
                if [ "$tag" = "$m" ]; then
                    note "$name: тег '$tag' неизменяемым не является."
                    note "  helm upgrade с тем же image.tag — no-op: kubelet не пойдёт за"
                    note "  новым образом, и новая сборка фронта молча не выкатится, хотя"
                    note "  выкат будет считаться успешным. Это хуже пустого тега."
                    note "  $TAGS_FILE обновляет CD репозитория alfa-cbdc-hub через"
                    note "  repository_dispatch (спека §8); пока dispatch не приходил, там"
                    note "  стоит заглушка. Для стенда, где это осознанно допустимо:"
                    note "  K3S_ALLOW_MUTABLE_TAGS=1 (и тогда pullPolicy решает всё за вас)."
                    if [ "${K3S_ALLOW_MUTABLE_TAGS-}" = 1 ]; then
                        note "  K3S_ALLOW_MUTABLE_TAGS=1 — пропускаю, но выкат невоспроизводим."
                    else
                        fail=1
                    fi
                fi
            done
        fi
    fi

    # --- пароль БД ---
    if [ "$pwvar" != "-" ]; then
        eval "pw=\${$pwvar-}"
        if [ -z "$pw" ]; then
            note "$name: пароль \$$pwvar не задан в окружении."
            note "  Локально: make k3s-secrets (создаёт infra/helm/secrets-k3s.env)."
            note "  В CD: secrets.$pwvar в настройках репозитория."
            note "  Чарт падает на рендере без него намеренно (fail-closed): пустой"
            note "  пароль молча выкатил бы сервис, который не подключится к базе."
            fail=1
        else
            short=${name#service-}
            cur=""
            if command -v kubectl >/dev/null 2>&1; then
                cur=$(kubectl get secret "pg-$short-postgresql" -n data \
                    -o jsonpath='{.data.password}' 2>/dev/null | base64 -d 2>/dev/null || true)
            fi
            if [ -z "$cur" ]; then
                note "$name: пароль против базы не сверялся — нет kubectl, нет кластера"
                note "  или релиз pg-$short ещё не выкачен. Остальные проверки прошли."
            elif [ "$cur" != "$pw" ]; then
                note "$name: пароль в кластере НЕ совпадает с \$$pwvar."
                note "  В Secret pg-$short-postgresql лежит другой пароль, то есть в базе"
                note "  тоже другой: чарт bitnami задаёт пароль только при первой"
                note "  инициализации PVC. Если выкатить так, рендер будет идеален, выкат"
                note "  'успешен', а под получит password authentication failed."
                note "  Чинить: привести \$$pwvar к тому, что в базе; или ALTER USER $short"
                note "  WITH PASSWORD в самой базе; или снести релиз pg-$short вместе с PVC."
                fail=1
            fi
        fi
    fi
done < "$ROWS"

if [ "$fail" -ne 0 ]; then
    echo "preflight: проверки не пройдены — выкат не начинался." >&2
    exit 1
fi

echo "preflight: ok ($(wc -l < "$ROWS" | tr -d ' ') сервис(ов))"
