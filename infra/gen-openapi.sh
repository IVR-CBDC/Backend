#!/usr/bin/env bash
# Генерирует docs/openapi.yml:
#   1) Парсит аннотации из C++ хедеров (// @GET, // @body, // @200 ...)
#   2) Достаёт OpenAPI из FastAPI (python -c "app.openapi()")
#   3) Мержит всё в один файл
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="docs/openapi.yml"
mkdir -p docs

HEADERS=(
  services/service-auth/include/auth_controller.h
  services/service-core/include/core_controller.h
)

FASTAPI_APP="services/service-test-python/app"


to_oapi_type() {
  case "$1" in
    string)  echo "string" ;;
    integer) echo "integer" ;;
    boolean) echo "boolean" ;;
    number)  echo "number" ;;
    *)       echo "string" ;;
  esac
}

emit_schema() {
  local json_str="$1"
  local indent="$2"

  json_str="${json_str#\{}"
  json_str="${json_str%\}}"

  if [[ -z "${json_str// /}" ]]; then
    echo "${indent}type: object"
    return
  fi

  echo "${indent}type: object"
  echo "${indent}properties:"

  local required_fields=()

  IFS=',' read -ra pairs <<< "$json_str"
  for pair in "${pairs[@]}"; do
    pair="$(echo "$pair" | sed 's/[" ]//g')"
    local key="${pair%%:*}"
    local val="${pair#*:}"

    local optional=false
    if [[ "$key" == *'?' ]]; then
      key="${key%?}"
      optional=true
    fi

    local oapi_type
    oapi_type="$(to_oapi_type "$val")"

    echo "${indent}  ${key}:"
    echo "${indent}    type: ${oapi_type}"

    if [[ "$optional" == false ]]; then
      required_fields+=("$key")
    fi
  done

  if [[ ${#required_fields[@]} -gt 0 ]]; then
    echo "${indent}required:"
    for rf in "${required_fields[@]}"; do
      echo "${indent}  - ${rf}"
    done
  fi
}

declare -a ENDPOINTS=()

parse_headers() {
  local method="" path="" summary="" has_auth=false body="" responses=""

  for header_file in "${HEADERS[@]}"; do
    method="" path="" summary="" has_auth=false body="" responses=""

    while IFS= read -r line; do
      line="$(echo "$line" | sed 's/^[[:space:]]*//')"

      if [[ "$line" =~ ^//[[:space:]]*@(GET|POST|PUT|DELETE|PATCH)[[:space:]]+(.*) ]]; then
        method="${BASH_REMATCH[1]}"
        path="${BASH_REMATCH[2]}"
        summary="" has_auth=false body="" responses=""
        continue
      fi

      if [[ "$line" =~ ^//[[:space:]]*@summary[[:space:]]+(.*) ]]; then
        summary="${BASH_REMATCH[1]}"; continue
      fi

      if [[ "$line" =~ ^//[[:space:]]*@header[[:space:]]+Authorization ]]; then
        has_auth=true; continue
      fi

      if [[ "$line" =~ ^//[[:space:]]*@body[[:space:]]+(.*) ]]; then
        body="${BASH_REMATCH[1]}"; continue
      fi

      if [[ "$line" =~ ^//[[:space:]]*@([0-9]+)[[:space:]]+(.*) ]]; then
        local code="${BASH_REMATCH[1]}"
        local schema="${BASH_REMATCH[2]}"
        responses="${responses}${code}=${schema};"
        continue
      fi

      if [[ "$line" =~ ^ADD_METHOD_TO && -n "$method" ]]; then
        local dup=false
        for existing in "${ENDPOINTS[@]+"${ENDPOINTS[@]}"}"; do
          local ex_m="${existing%%|*}"
          local ex_r="${existing#*|}"
          local ex_p="${ex_r%%|*}"
          if [[ "$ex_m $ex_p" == "$method $path" ]]; then
            dup=true; break
          fi
        done
        [[ "$dup" == true ]] ||
        ENDPOINTS+=("${method}|${path}|${summary}|${has_auth}|${body}|${responses}")
        method="" path="" summary="" has_auth=false body="" responses=""
      fi
    done < "$header_file"
  done
}


generate_cpp_paths() {
  local prev_path=""

  for ep in "${ENDPOINTS[@]}"; do
    IFS='|' read -r method path summary has_auth body responses <<< "$ep"
    local method_lower
    method_lower="$(echo "$method" | tr '[:upper:]' '[:lower:]')"

    if [[ "$path" != "$prev_path" ]]; then
      echo "  ${path}:"
      prev_path="$path"
    fi

    echo "    ${method_lower}:"
    if [[ -n "$summary" ]]; then
      echo "      summary: \"${summary}\""
    fi

    if [[ "$has_auth" == true ]]; then
      echo "      security:"
      echo "        - bearerAuth: []"
    fi

    if [[ -n "$body" ]]; then
      echo "      requestBody:"
      echo "        required: true"
      echo "        content:"
      echo "          application/json:"
      echo "            schema:"
      emit_schema "$body" "              "
    fi

    echo "      responses:"
    if [[ -n "$responses" ]]; then
      IFS=';' read -ra resp_arr <<< "$responses"
      for resp in "${resp_arr[@]}"; do
        [[ -z "$resp" ]] && continue
        local code="${resp%%=*}"
        local schema="${resp#*=}"

        local desc="OK"
        case "$code" in
          400) desc="Bad Request" ;;
          401) desc="Unauthorized" ;;
          403) desc="Forbidden" ;;
          404) desc="Not Found" ;;
          409) desc="Conflict" ;;
          500) desc="Internal Server Error" ;;
        esac

        echo "        \"${code}\":"
        echo "          description: \"${desc}\""
        echo "          content:"
        echo "            application/json:"
        echo "              schema:"
        emit_schema "$schema" "                "
      done
    fi

    echo ""
  done
}


generate_fastapi_paths() {
  local json_spec=""

  # 1) Пробуем из запущенного контейнера
  json_spec="$(docker compose exec -T service-test-python \
    python3 -c 'from main import app; import json; print(json.dumps(app.openapi()))' \
    2>/dev/null)" || true

  # 2) Fallback: локальный Python с мок-зависимостями (для CI)
  if [[ -z "$json_spec" ]]; then
    json_spec="$(python3 -c "
import sys, types, os, json

for mod in ('asyncpg', 'asyncpg.pool'):
    sys.modules[mod] = types.ModuleType(mod)

import sqlalchemy.ext.asyncio as _aio
_orig_create = _aio.create_async_engine
def _fake_engine(*a, **kw):
    class FakeEngine:
        pass
    return FakeEngine()
_aio.create_async_engine = _fake_engine

_aio.async_sessionmaker = lambda *a, **kw: None

os.environ['JWT_PUBLIC_KEY_PATH'] = '/dev/null'
import builtins, io
_orig_open = builtins.open
def _patched_open(path, *a, **kw):
    if 'jwt' in str(path).lower() or str(path) == '/dev/null':
        return io.BytesIO(b'fake-key-for-openapi-gen')
    return _orig_open(path, *a, **kw)
builtins.open = _patched_open

sys.path.insert(0, '${FASTAPI_APP}')
from main import app
builtins.open = _orig_open

print(json.dumps(app.openapi()))
" 2>/dev/null)" || true
  fi

  if [[ -z "$json_spec" ]]; then
    echo "  # service-test-python: не удалось извлечь OpenAPI" >&2
    echo "  # (нужен запущенный контейнер или python3 + fastapi + pydantic)" >&2
    return
  fi

  echo "$json_spec" | python3 -c "
import sys, json

spec = json.load(sys.stdin)

def yaml_type(schema, indent):
    lines = []
    t = schema.get('type', 'object')
    lines.append(f'{indent}type: {t}')
    if 'properties' in schema:
        lines.append(f'{indent}properties:')
        for pname, pval in schema['properties'].items():
            lines.append(f'{indent}  {pname}:')
            pt = pval.get('type', 'string')
            lines.append(f'{indent}    type: {pt}')
        req = schema.get('required', [])
        if req:
            lines.append(f'{indent}required:')
            for r in req:
                lines.append(f'{indent}  - {r}')
    return '\n'.join(lines)

def resolve_ref(ref):
    parts = ref.lstrip('#/').split('/')
    obj = spec
    for p in parts:
        obj = obj[p]
    return obj

for path, methods in spec.get('paths', {}).items():
    print(f'  {path}:')
    for method, detail in methods.items():
        print(f'    {method}:')
        if 'summary' in detail:
            print(f'      summary: \"{detail[\"summary\"]}\"')
        if 'description' in detail:
            print(f'      description: \"{detail[\"description\"]}\"')
        print('      responses:')
        for code, resp in detail.get('responses', {}).items():
            desc = resp.get('description', 'OK')
            print(f'        \"{code}\":')
            print(f'          description: \"{desc}\"')
            content = resp.get('content', {})
            if 'application/json' in content:
                schema = content['application/json'].get('schema', {})
                if '\$ref' in schema:
                    schema = resolve_ref(schema['\$ref'])
                print('          content:')
                print('            application/json:')
                print('              schema:')
                print(yaml_type(schema, '                '))
        print()
"
}


parse_headers

{
  cat <<'HEADER'
openapi: "3.0.3"
info:
  title: IVR Backend API
  version: "0.1.0"
  description: "-"

servers:
  - url: http://localhost
    description: Local

components:
  securitySchemes:
    bearerAuth:
      type: http
      scheme: bearer
      bearerFormat: JWT

paths:
HEADER

  generate_cpp_paths

  fastapi_out="$(generate_fastapi_paths 2>&1)" || true
  if [[ -n "$fastapi_out" ]]; then
    echo "$fastapi_out"
  else
    echo "  # service-test-python: не удалось извлечь OpenAPI (нужен python3 + fastapi)"
  fi

} > "$OUT"

cpp_count="${#ENDPOINTS[@]}"
py_count="$(grep -c '^  /api/test-python\|^  /health2' "$OUT" 2>/dev/null || true)"
py_count="${py_count:-0}"
total=$(( cpp_count + py_count ))

echo "Generated ${OUT}"
echo "  C++ endpoints: ${cpp_count}"
echo "  FastAPI endpoints: ${py_count}"
echo "  Total: ${total}"
