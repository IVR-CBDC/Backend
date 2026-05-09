#!/bin/sh
set -e

# In k8s, config.json is mounted from ConfigMap — skip envsubst.
# In docker-compose, config.json.tpl is templated at runtime.
if [ ! -f /app/config.json ] && [ -f /app/config.json.tpl ]; then
  envsubst < /app/config.json.tpl > /app/config.json
fi

exec "$@"
