# 0008. SPA слушает 8080, а не 80

- Статус: принято (план 08, Task 2)
- Дата: 2026-09-21

## Контекст

Все пять подов переведены на `runAsNonRoot: true`,
`allowPrivilegeEscalation: false`, `capabilities: drop: [ALL]`,
`seccompProfile: RuntimeDefault`. Стоковый образ `nginx:alpine` стартует
мастер-процессом от root и биндится на порт 80.

## Решение

Образ SPA — `nginxinc/nginx-unprivileged`, слушает **8080**. Адрес
`http://frontend:8080` прописан и в `infra/traefik/dynamic.yml`
(compose), и в `infra/helm/values-frontend.yaml` (k3s).

Причина простая и не обсуждаемая: привязка к порту ниже 1024 требует
`CAP_NET_BIND_SERVICE`, а эта capability сброшена вместе со всеми
остальными. Под `runAsNonRoot` с пустым набором capabilities стоковый
nginx не стартует вообще.

## Последствия

- `runAsUser: 101` задан явно (пользователь `nginx` в
  nginx-unprivileged). Явный `runAsUser` нужен всем трём собираемым здесь
  образам: kubelet отказывается стартовать контейнер под `runAsNonRoot`,
  если не может доказать, что процесс не root — образы auth и core не
  объявляют `USER` вовсе, commission объявляет нечисловой `USER app`.
  Рендер чарта падает, если `runAsNonRoot: true` стоит без `runAsUser`.
- `readOnlyRootFilesystem: true` требует записываемого `/tmp`
  (`emptyDir`): nginx-unprivileged держит там pid-файл и все
  `*_temp`-пути, без него под падает на `mkdir() "/tmp/proxy_temp" failed
  (30: Read-only file system)`. `/var/cache/nginx` монтировать не надо —
  проверено запуском `docker run --read-only --user 101:101 --tmpfs /tmp`
  (HTTP 200).
- Порт 8080 — часть внешнего контракта стенда: любой новый маршрут к
  фронту (Traefik, IngressRoute, `port-forward`) обязан указывать его, а
  не 80.
- **Фронт зависит от bff на старте.** nginx резолвит имя `bff` при разборе
  конфига (`proxy_pass http://bff:4000`), а не при первом запросе: без
  пода bff контейнер падает с `[emerg] host not found in upstream "bff"`.
  Это не про порт, но живёт в том же файле и ломается одинаково часто —
  порядок выката зафиксирован в `infra/services.tsv`.

## Отвергнутые варианты

- **Оставить nginx:alpine и вернуть `CAP_NET_BIND_SERVICE`.** Делать
  исключение в наборе capabilities ради номера порта, который снаружи всё
  равно не виден (наружу смотрит Traefik) — плохая цена.
- **Запускать стоковый nginx от root и сбрасывать привилегии воркерами.**
  Ровно то, что запрещает `runAsNonRoot`, и ровно то, от чего эта
  настройка защищает.
