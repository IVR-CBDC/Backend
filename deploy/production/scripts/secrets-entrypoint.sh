#!/bin/sh
set -eu
DB_PASSWORD=$(cat /run/secrets/db_password)
[ -n "$DB_PASSWORD" ] || { echo 'Empty database secret' >&2; exit 1; }
export DB_PASSWORD
if command -v python >/dev/null 2>&1; then
    # Existing database passwords need not use our old hex-only format.
    PG_DSN=$(python -c 'import os; from urllib.parse import quote
v=os.environ
host=v["DB_HOST"]
if ":" in host and not host.startswith("["): host="["+host+"]"
print("postgresql+asyncpg://"+quote(v["DB_USER"],safe="")+":"+quote(v["DB_PASSWORD"],safe="")+"@"+host+":"+v["DB_PORT"]+"/"+quote(v["DB_NAME"],safe=""))')
    export PG_DSN
else
    # C++ envsubst consumes JSON strings; escape existing passwords before rendering.
    for name in DB_PASSWORD DB_HOST DB_NAME DB_USER; do
        value=''
        eval 'value=${'"$name"'}'
        if printf '%s' "$value" | LC_ALL=C grep -q '[[:cntrl:]]'; then
            echo 'Control characters in database config are unsupported' >&2; exit 1
        fi
        escaped=$(printf '%s' "$value" | sed 's/\\/\\\\/g; s/"/\\"/g')
        export "$name=$escaped"
    done
fi
exec "$@"
