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
{{- /* Регулярка выше пропускает вырожденные "." и "..", а Kubernetes
       отвергает их отдельным правилом: ключ становится именем файла при
       монтировании Secret'а, а такие имена заняты самой файловой системой. */ -}}
{{- if or (eq $ctx.key ".") (eq $ctx.key "..") }}
{{- fail (printf "%s: ключ Secret'а %q недопустим. Ключ становится именем файла при монтировании Secret'а, а \".\" и \"..\" — это сам каталог и родительский; API-сервер отвергает их отдельным правилом." $ctx.where $ctx.key) }}
{{- end }}
{{- end }}

{{/*
Проверка, что значение можно без кодирования положить в userinfo URL
(пароль в строке подключения вида postgresql://user:ПАРОЛЬ@host/db).

Возвращает значение как есть — задумано для пайпа внутри шаблона derived.

Почему проверка, а не кодирование: процент-энкодер в Helm-шаблоне —
лекарство хуже болезни, а `urlquery` кодирует пробел как `+`, который в
userinfo раскодируется обратно плюсом, то есть тихо меняет пароль. Пароли
для этого стенда генерируем мы сами, так что ограничение на набор символов
ничего не стоит; молча разъехавшийся DSN стоил бы отладки на кластере.

Разрешено то, что RFC 3986 позволяет в userinfo без кодирования:
unreserved (ALPHA DIGIT - . _ ~) и sub-delims (! $ & ' ( ) * + , ; =).
Двоеточие исключено намеренно: в userinfo оно отделяет пользователя от
пароля, и пароль с ":" сместил бы границу.
*/}}
{{- define "generic-service.requireUrlUserinfoSafe" -}}
{{- $v := . | toString -}}
{{- $bad := regexFindAll "[^A-Za-z0-9._~!$&'()*+,;=-]" $v -1 -}}
{{- if $bad }}
{{- fail (printf "пароль содержит символы, которые в userinfo URL обязаны быть процент-кодированы: %s. Такой пароль попал бы в DSN как есть и развалил бы разбор строки подключения на стороне клиента — причём Secret при этом выглядел бы правильным. Разрешены латиница, цифры и - . _ ~ ! $ & ' ( ) * + , ; = (RFC 3986 userinfo без двоеточия: в userinfo оно отделяет пользователя от пароля). Сгенерируйте пароль из этого набора." (join " " (uniq $bad))) }}
{{- end }}
{{- $v -}}
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
