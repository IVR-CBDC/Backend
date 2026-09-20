{{/*
Expand the name of the chart.
*/}}
{{- define "generic-service.name" -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Create a default fully qualified app name.
*/}}
{{- define "generic-service.fullname" -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels
*/}}
{{- define "generic-service.labels" -}}
helm.sh/chart: {{ .Chart.Name }}-{{ .Chart.Version | replace "+" "_" }}
app.kubernetes.io/name: {{ include "generic-service.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Экранирование строки для подстановки ВНУТРЬ уже закавыченной JSON-строки.

Нужно для config.json: он сериализуется toPrettyJson, и подстановка попадает
между кавычек, которые поставил сериализатор. Пароль с двойной кавычкой без
экранирования разорвал бы JSON — Drogon получил бы битый конфиг и не
стартовал, а увидели бы это только на кластере.

toJson даёт строку С кавычками и со всеми экранированиями (`"a\"b"`); здесь
снимаются ровно внешние кавычки — обрезкой по длине, а не trimAll, который
съел бы и экранирующий слеш у пароля, кончающегося кавычкой.
*/}}
{{- define "generic-service.jsonEscape" -}}
{{- $j := toJson . -}}
{{- $j | trunc (int (sub (len $j) 1)) | trimPrefix "\"" -}}
{{- end }}

{{/*
Проверка имени ключа Secret'а. Kubernetes требует [-._a-zA-Z0-9]+; всё
остальное API-сервер отвергает при apply — то есть без этой проверки
опечатка вроде `secrets.data.bad key` дожила бы до кластера.
*/}}
{{- define "generic-service.checkSecretKey" -}}
{{- $ctx := . -}}
{{- if not (regexMatch "^[-._a-zA-Z0-9]+$" $ctx.key) }}
{{- fail (printf "%s: ключ Secret'а %q недопустим. Kubernetes разрешает в ключах только [-._a-zA-Z0-9]; API-сервер отверг бы это при apply, а не при рендере." $ctx.where $ctx.key) }}
{{- end }}
{{- end }}

{{/*
Проверка имени переменной окружения: Kubernetes требует C_IDENTIFIER
([A-Za-z_][A-Za-z0-9_]*). Пустое имя и имя с дефисом отвергает API-сервер.
*/}}
{{- define "generic-service.checkEnvName" -}}
{{- $ctx := . -}}
{{- if not (regexMatch "^[A-Za-z_][A-Za-z0-9_]*$" ($ctx.name | toString)) }}
{{- fail (printf "%s: имя переменной окружения %q недопустимо. Kubernetes требует C_IDENTIFIER ([A-Za-z_][A-Za-z0-9_]*); пустое имя или дефис отвергает API-сервер при apply." $ctx.where ($ctx.name | toString)) }}
{{- end }}
{{- end }}

{{/*
Selector labels
*/}}
{{- define "generic-service.selectorLabels" -}}
app.kubernetes.io/name: {{ include "generic-service.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}
