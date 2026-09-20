"""Регистрация и немедленный вход: ответ обязан уходить только после COMMIT.

Дефект, который воспроизводит этот файл (план 08, задача 5): у Drogon нет
публичного `commit()` (в `drogon/orm/DbClient.h` он закомментирован, наружу
торчат только `rollback()` и `setCommitCallback()`), поэтому `COMMIT` уезжает
в деструкторе `Transaction` — то есть уже после того, как обработчик записал
200 с токеном в сокет. Клиент получает `user_id`/`company_id`/`token`, пока
COMMIT ещё в полёте; следующий запрос берёт из пула ДРУГОЕ соединение и под
READ COMMITTED не видит ни пользователя, ни компанию:

  * `POST /api/auth/login` теми же кредами -> 401 INVALID_CREDENTIALS
    (строки в `users` ещё нет);
  * `GET /me` с выданным токеном -> 404 NOT_FOUND (JWT валиден, JOIN пуст).

Окно измеряется миллисекундами, поэтому тут НЕТ ни пауз, ни ретраев, ни
polling-хелперов — именно такая затычка прятала дефект до сегодняшнего дня.
Если тест начнёт мигать — это неполная починка, а не плохой тест: чинить
надо сервис.

Один прогон ничего не доказывает (окно узкое), поэтому сценарий
параметризован: pytest показывает, сколько попыток из `ATTEMPTS` упало.
"""

from __future__ import annotations

import uuid

import httpx
import pytest

# Столько независимых регистраций делает один прогон файла. Отчёт задачи
# оперирует именно этими числами ("N из ATTEMPTS упало до починки").
ATTEMPTS = 20

PASSWORD = "hunter22"


def _random_inn() -> str:
    # ИНН: ровно 10 цифр (CHECK в миграции 002_company.sql). uuid4 даёт
    # достаточно энтропии, чтобы 20 регистраций подряд не столкнулись.
    return "{:010d}".format(uuid.uuid4().int % 10_000_000_000)


@pytest.mark.parametrize("attempt", range(ATTEMPTS))
def test_register_then_immediate_login_and_me(auth_url: str, attempt: int) -> None:
    login = f"commit-{uuid.uuid4().hex[:12]}"
    payload = {
        "login": login,
        "password": PASSWORD,
        "name": "Тестовый пользователь",
        "company_name": f"ООО Коммит {uuid.uuid4().hex[:8]}",
        "inn": _random_inn(),
    }

    # Один keep-alive клиент на все три запроса: так между ответом на
    # register и уходом login нет даже TCP-рукопожатия — ровно тот
    # «зарегистрировался -> сразу вошёл», что делает SPA через BFF.
    with httpx.Client(base_url=auth_url, timeout=10.0) as client:
        register = client.post("/api/auth/register", json=payload)
        assert register.status_code == 200, register.text
        registered = register.json()

        # Никаких time.sleep() между запросами — в этом весь смысл теста.
        login_resp = client.post(
            "/api/auth/login", json={"login": login, "password": PASSWORD}
        )
        assert login_resp.status_code == 200, (
            "вход сразу после регистрации получил {}: ответ на register ушёл "
            "раньше COMMIT его транзакции, и соединение из пула под READ "
            "COMMITTED ещё не видит пользователя. Тело: {}".format(
                login_resp.status_code, login_resp.text
            )
        )
        logged_in = login_resp.json()
        assert logged_in["user_id"] == registered["user_id"]
        assert logged_in["company_id"] == registered["company_id"]

        # Токен из ответа на register (не из login): проверяем, что данные,
        # на которые он ссылается, уже видны другому соединению.
        me = client.get(
            "/api/auth/me",
            headers={"Authorization": "Bearer {}".format(registered["token"])},
        )
        assert me.status_code == 200, (
            "GET /me с токеном из register получил {}: JWT валиден, но JOIN "
            "users/companies ещё пуст — компания и пользователь не "
            "закоммичены. Тело: {}".format(me.status_code, me.text)
        )
        body = me.json()
        assert body["login"] == login
        assert body["company"]["id"] == registered["company_id"]
        assert body["company"]["inn"] == payload["inn"]
