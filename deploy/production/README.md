# Автоматический production-деплой IVR-CBDC

Обычный push в `main` обновляет приложение на https://alfacbcd.ru после однократной настройки подключений. PR, Run workflow и ручные approvals не нужны. VPS: Ubuntu 26.04 amd64, пользователь deploy, Docker Compose, 1 vCPU / 2 ГБ RAM / 10 ГБ SSD.

## Что обновляется

Compose управляет только шестью контейнерами: auth, core, commission из Backend, BFF, SPA и reverse proxy. PostgreSQL и Redis уже работают отдельно: **их нет в этом Compose; pipeline не создаёт, не переносит и не обновляет базы, не выполняет SQL-миграции или бэкапы**. Существующая схема должна соответствовать приложению. Изменения схемы требуют отдельного согласованного процесса.

- Backend `main`: fingerprint закоммиченных входов сборки → сборка только изменившихся/отсутствующих образов → GHCR → SSH → Compose. Общая C++-библиотека/базовый образ пересобирает auth и core. Изменение commission не пересобирает C++. Для обновления базовых зависимостей увеличьте `image-revision` обычным коммитом.
- Frontend `main`: сборка BFF и SPA → GHCR → `repository_dispatch` в Backend → тот же pipeline. Оба образа публикуются до уведомления.
- Общая очередь Backend `ivrcbdc-production`, `queue: max`, без отмены активного деплоя; дополнительно VPS использует `flock` для deploy/TLS. Устаревший снимок пропускается, следующий запуск доставляет актуальные main.
- Образы зафиксированы по digest; SHA обоих репозиториев записаны в `release.json`. Теги `:main` оставлены для прежнего CI, production их не запускает.
- K3s deploy заменён; workflow создания Helm PR удалён. Прикладной код и бизнес-логика не меняются.
- Отдельный `IVR-CBDC/commission-service` — CSV-калькулятор `/calculate`. Он не используется приложением: BFF/core обращаются к `Backend/services/service-commission` с JWT и PostgreSQL. Поэтому push в отдельный CSV-репозиторий сайт не обновляет.

## Состав релиза

На VPS передаётся только production Compose, proxy-конфигурации, фиксированный список серверных скриптов, пример серверной конфигурации, документация и манифест образов. SQL, дампы, данные, volumes, исходники и сборочные инструменты в архив не входят. Образы скачиваются из GHCR; сборок на VPS нет.

Локальные `.env`, ключи, локальный Compose, benchmark, данные и локальные изменения Dockerfile в коммиты деплоя не включаются. `package.py` собирает архив по явному разрешённому списку. Тест подкладывает фиктивные секреты/SQL/локальные файлы и проверяет их отсутствие. Fingerprint использует только Git HEAD.

## Однократная настройка GitHub

| Где | Настройка |
| --- | --- |
| Backend → Environment production | Существующие `PROD_HOST`, `PROD_SSH_KEY`, `PROD_SSH_KNOWN_HOSTS` |
| Backend → repository variable | `PROD_SSH_PORT`, если действующий SSH-порт отличается от 22 |
| Frontend → repository secret | `BACKEND_DISPATCH_TOKEN`: fine-grained PAT только для Backend, `Contents: write` |
| GHCR frontend/bff и frontend/spa | Manage Actions access → Backend → Read; либо публичные packages |
| VPS, пользователь deploy | Для приватных образов `docker login ghcr.io -u YOUR_GITHUB_LOGIN`, отдельный PAT `read:packages` |

GitHub сам выдаёт `GITHUB_TOKEN`; workflows задают `packages: write` для публикации. Личный токен разработчика не копируется в CI. Environment production должен оставаться без required reviewers, wait timer и custom deployment rules. Доступ к deployment ограничьте main; PR не требуется.

## Подключение существующих сервисов на VPS

Pipeline выполняет read-only SSH preflight. Пока настройки отсутствуют, образ собирается, но production не запускается. Причины указаны в журнале preflight. Маркер `shared/auto-deploy-ready` обозначает завершённую однократную настройку, а не подтверждение каждого обновления.

Под пользователем deploy создайте только каталоги приложения:

```bash
umask 077
mkdir -p /home/deploy/ivr-cbdc/{releases,incoming,shared/secrets,shared/proxy,shared/acme,shared/letsencrypt}
```

1. По `server.env.example` подготовьте `/home/deploy/ivr-cbdc/shared/server.env` с действующими адресами/именами/пользователями БД и Redis. `APP_NETWORK` — уже существующая сеть, из которой они доступны. UID/GID берутся через `id -u deploy` / `id -g deploy`. Если сервисы на самом хосте, адрес должен быть доступен из Docker; `127.0.0.1` внутри приложения означает его контейнер.
2. Подключите **существующие production-секреты** в `shared/secrets`: `db_auth`, `db_core`, `db_commission`, `jwt_private.jwk`, `jwt_public.jwk`, `jwt_public.pem`. Можно использовать ссылки на существующие серверные файлы. Не копируйте локальные секреты и не генерируйте новые пароли/ключи. Файлы должны быть доступны deploy; private key и пароли — с правами 600. JWT public JWK/PEM должны относиться к тому же ключу.
3. Для приватного GHCR выполните вход Docker под deploy; защитите `~/.docker/config.json` правами 600. Проверьте DNS домена и отсутствие конфликта портов 80/443 с уже работающим proxy. SSH и firewall этот проект не меняет.
4. Только после проверки существующих подключений и готовности к первому запуску создайте `touch /home/deploy/ivr-cbdc/shared/auto-deploy-ready`. Следующий обычный push автоматически запустит приложение.

Если Redis требует пароль, BFF поддерживает его через `REDIS_URL`. Существующая C++-конфигурация core имеет только host/port; аутентификацию Redis/TLS без изменения приложения она не поддерживает. Не отключайте защиту Redis ради деплоя: это отдельный вопрос совместимости существующей инфраструктуры.

## HTTPS

До настройки TLS ingress возвращает HTTP 503; Secure cookies уже включены. Сертификаты автоматически не запрашиваются. После первого запуска и отдельного разрешения на выпуск:

```bash
bash /home/deploy/ivr-cbdc/current/scripts/tls.sh --issue YOUR_EMAIL --accept-terms
```

Команда использует certbot только в профиле tools. Затем HTTPS/redirect сохраняются между релизами. Если уже есть действующий сертификат, подключите его в `shared/letsencrypt/live/alfacbcd.ru/{fullchain.pem,privkey.pem}` и создайте `shared/tls-enabled` до первого запуска; повторный выпуск не нужен. После включения TLS добавьте продление в crontab deploy:

```cron
17 3,15 * * * /bin/bash /home/deploy/ivr-cbdc/current/scripts/tls.sh --renew 2>&1 | /usr/bin/logger -t ivrcbdc-tls
```

## Обновление и откат

После pull скрипт останавливает только приложение, запускает новый набор и проверяет healthchecks и локальный ingress. Это обновление с коротким перерывом обслуживания. При ошибке возвращаются предыдущие образы и proxy-конфигурация; данные БД, Redis, JWT и production-секреты остаются на месте. Указатель current не переключается при неудаче; workflow сообщает ошибку даже после успешного отката. При первом неуспешном запуске предыдущего релиза нет: останавливаются только новые контейнеры приложения.

Автооткат не изменяет схему или содержимое БД. Совместимость новых и предыдущих версий приложения с уже существующей схемой обязательна. При ошибке самого восстановления выводится `ROLLBACK_FAILED` для ручного разбора.

## Ограничения VPS

Сумма лимитов шести контейнеров приложения — **864 МиБ**. Остальная память нужна существующим PostgreSQL/Redis, Docker и ОС. Это настройка, не измерение суммарного потребления VPS и не гарантия достаточности 2 ГБ. Логи приложения ограничены 5 МиБ × 2 × 6 ≈ 60 МиБ. Certbot при ручном запуске ограничен 192 МиБ.

До pull требуется 3 ГиБ свободно, после pull — 1 ГиБ; при недостатке места обновление прекращается до остановки приложения. Сохраняются current и previous images. Очистка удаляет только старые digest-ссылки приложения из манифестов, без force. Чужие образы, контейнеры, volumes и данные не очищаются. Нет system prune, volume prune или down -v. На VPS не скачиваются cpp-base и build cache. На диске 10 ГБ место под ОС, существующие данные и две версии приложения нужно проверить фактически; скрипт откажет при нехватке резерва.

## Проверки

```bash
python3 deploy/production/tests/check.py
python3 deploy/production/tests/plan-check.py
```

Проверяются изоляция архива, digest, Compose, лимиты, выборочная сборка, нехватка диска, ошибки pull/healthcheck, успешное переключение и откат без операций с БД. Сценарные тесты используют подмену Docker/curl, не запускают production. `runtime-check.py` проверяет nginx и чтение существующих секретов в одноразовых контейнерах без сети, с временным самоподписанным тестовым сертификатом.

`queue: max` — [официальный синтаксис GitHub concurrency](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#concurrency); actionlint 1.7.12 пока не распознаёт только это поле. [Доступ другого репозитория к GHCR](https://docs.github.com/en/packages/learn-github-packages/configuring-a-packages-access-control-and-visibility).
