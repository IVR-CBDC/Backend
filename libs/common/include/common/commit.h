#pragma once

// Ожидание реального COMMIT транзакции Drogon.
//
// Зачем это вообще нужно. У `drogon::orm::Transaction` НЕТ публичного
// `commit()`: в `drogon/orm/DbClient.h` он закомментирован
// (`// virtual void commit() = 0;`), наружу торчат только `rollback()` и
// `setCommitCallback()`. `COMMIT` уезжает в деструкторе `TransactionImpl`,
// причём через `loop->queueInLoop(...)` — то есть асинхронно и, вообще
// говоря, на треде соединения БД, а не на треде обработчика. Пока код
// делает `cb(response)` раньше, чем отпустит последнюю ссылку на
// транзакцию, ответ уходит клиенту, а COMMIT ещё в полёте: следующий
// запрос берёт из пула ДРУГОЕ соединение и под READ COMMITTED не видит
// только что «созданных» строк.
//
// `awaitCommit(std::move(tx))` закрывает этот разрыв: забирает последнюю
// ссылку себе, вешает колбэк, отпускает ссылку (это и запускает COMMIT) и
// приостанавливает корутину до результата. Возвращает признак успеха —
// неуспех обязан становиться `500 INTERNAL_ERROR` у вызывающего, а не
// молча считаться коммитом.
//
// Разобранные подводные камни:
//
//  * **Колбэк приходит на чужом треде.** `TransactionImpl::~TransactionImpl`
//    ставит COMMIT в цикл соединения БД; колбэк выполнится там. Возобновлять
//    корутину обработчика на этом треде нельзя — остаток обработчика (включая
//    запись ответа) уехал бы с тредовой принадлежностью Drogon'а. Поэтому
//    петля обработчика запоминается в `await_suspend`, а возобновление
//    отправляется в неё через `queueInLoop`.
//  * **Нельзя удерживать транзакцию из самого колбэка.** Если лямбда
//    захватит `TransactionPtr`, ссылка не умрёт до вызова колбэка, а колбэк
//    не вызовется до деструктора — ожидание зависнет навсегда. Здесь лямбда
//    захватывает только `this`, `handle` и петлю.
//  * **`queueInLoop`, а не `runInLoop`.** `runInLoop` на своём же треде
//    выполнил бы лямбду прямо внутри колбэка, и если колбэк почему-либо
//    придёт синхронно из `await_suspend`, корутина возобновилась бы (и,
//    возможно, уничтожила свой кадр) до возврата из `await_suspend` — UB.
//    `queueInLoop` ставит в очередь всегда.
//  * **После `rollback()` ждать нечего.** `TransactionImpl` в этом случае
//    выставляет `isCommitedOrRolledback_` и в деструкторе НЕ вызывает
//    commit-колбэк вообще. `awaitCommit` на откаченной транзакции повиснет,
//    поэтому на ветках с `rollback()` её звать нельзя — там ответ 4xx
//    отдаётся как и раньше, без ожидания.

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoop.h>

#include <coroutine>
#include <memory>
#include <stdexcept>
#include <utility>

namespace common {

// Бросается там, где у вызывающего нет отдельного канала для статуса
// (например, `DealRepository::create()` возвращает сам объект). Все
// HTTP-обработчики уже ловят `std::exception` и отвечают
// `500 INTERNAL_ERROR`, так что бросок попадает ровно туда, куда нужно.
class CommitFailed : public std::runtime_error {
 public:
  CommitFailed() : std::runtime_error("transaction commit failed") {}
};

namespace detail {

struct [[nodiscard]] CommitAwaiter : public drogon::CallbackAwaiter<bool> {
  explicit CommitAwaiter(std::shared_ptr<drogon::orm::Transaction> trans)
      : trans_(std::move(trans)) {}

  void await_suspend(std::coroutine_handle<> handle) {
    // Петля, на которой корутину нужно возобновить: та, на которой она
    // сейчас приостанавливается (тред обработчика). Вне цикла событий
    // (теоретически — например, из теста) падаем на главную петлю приложения.
    auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    if (loop == nullptr) loop = drogon::app().getLoop();

    // Локальная копия — единственная оставшаяся ссылка на транзакцию.
    auto trans = std::move(trans_);

    // `this` живёт в кадре приостановленной корутины (временный объект
    // awaiter'а существует до конца полного выражения `co_await`, то есть
    // переживает приостановку), поэтому захват по указателю безопасен.
    // Транзакция здесь НЕ захватывается — см. второй подводный камень выше.
    trans->setCommitCallback([this, handle, loop](bool ok) {
      setValue(ok);
      loop->queueInLoop([handle]() { handle.resume(); });
    });

    // Последняя ссылка уходит -> ~TransactionImpl -> COMMIT.
    trans.reset();
  }

 private:
  std::shared_ptr<drogon::orm::Transaction> trans_;
};

}  // namespace detail

// Отпускает `trans` и ждёт результат её COMMIT. Транзакцию сюда нужно
// именно ПЕРЕДАВАТЬ (`std::move`), а не копировать: любая уцелевшая у
// вызывающего ссылка отложит деструктор, а значит и COMMIT, и ожидание
// зависнет.
//
// `true` — COMMIT подтверждён сервером, данные видны другим соединениям.
// `false` — COMMIT не прошёл (Drogon уже залогировал причину); вызывающий
// обязан ответить `500 INTERNAL_ERROR`.
[[nodiscard]] inline detail::CommitAwaiter awaitCommit(
    std::shared_ptr<drogon::orm::Transaction> trans) {
  return detail::CommitAwaiter(std::move(trans));
}

}  // namespace common
