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
// Написано против Drogon 1.9.11 (`orm_lib/src/TransactionImpl.cc`). Если
// версия поменяется — особенно если `commit()` раскомментируют — обёртку
// надо пересмотреть целиком.
//
// Разобранные подводные камни:
//
//  * **Колбэк приходит на чужом треде.** `TransactionImpl::~TransactionImpl`
//    ставит COMMIT в цикл соединения БД; колбэк выполнится там. Возобновлять
//    корутину обработчика на этом треде нельзя — остаток обработчика (включая
//    запись ответа) уехал бы с тредовой принадлежностью Drogon'а. Поэтому
//    петля обработчика запоминается при приостановке, а возобновление
//    отправляется в неё через `queueInLoop`.
//  * **Нельзя удерживать транзакцию из самого колбэка.** Если лямбда
//    захватит `TransactionPtr`, ссылка не умрёт до вызова колбэка, а колбэк
//    не вызовется до деструктора — ожидание зависнет навсегда. Здесь лямбды
//    захватывают только `shared_ptr<CommitState>` (см. ниже) и ничего больше.
//  * **Нельзя захватывать сырой `this` awaiter'а.** Awaiter — временный
//    объект в кадре корутины. Если кадр умрёт приостановленным (например,
//    `drogon::async_run` в `outbox_publisher.cc`/`emulator.cc` не доработает
//    при остановке приложения), пришедший позже колбэк обратился бы к
//    освобождённой памяти. Всё, что переживает приостановку, вынесено в
//    `CommitState` под `shared_ptr`: колбэк и таймер держат его сами, и
//    смерть кадра их уже не касается.
//  * **`queueInLoop`, а не `runInLoop`.** `runInLoop` на своём же треде
//    выполнил бы лямбду прямо внутри колбэка, и если колбэк почему-либо
//    придёт синхронно из `await_suspend`, корутина возобновилась бы (и,
//    возможно, уничтожила свой кадр) до возврата из `await_suspend` — UB.
//    `queueInLoop` ставит в очередь всегда.
//  * **Колбэк может не прийти никогда.** В деструкторе `TransactionImpl`
//    `commitCb(false)` вызывается ТОЛЬКО внутри
//    `catch (const DrogonDbException &)`; исключение любого другого типа
//    улетает из лямбды, и колбэка не будет. `Transaction::setTimeout()` тут
//    не помогает — деструктор зовёт `conn->execSql` напрямую, минуя путь с
//    таймаутом. Поэтому таймаут здесь свой (`DB_COMMIT_TIMEOUT_SEC`, по
//    умолчанию 30 с). Гонка «колбэк против таймера» закрыта атомарным «кто
//    первый» в `CommitState::finish`: корутина возобновляется ровно один раз,
//    опоздавший молча уходит. Наивный `runAfter` без этого дал бы двойное
//    возобновление.
//  * **После `rollback()` ждать нечего.** `TransactionImpl` в этом случае
//    выставляет `isCommitedOrRolledback_` и в деструкторе НЕ вызывает
//    commit-колбэк вообще. Чтобы это нельзя было перепутать, ветки отката
//    ходят через `rollbackAndDiscard(std::move(tx))`: она забирает
//    транзакцию себе, и живого `tx` для `awaitCommit` после неё просто не
//    остаётся.

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <coroutine>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
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

// Сколько ждать commit-колбэк, прежде чем считать COMMIT неудавшимся.
// <= 0 выключает таймер (тогда поведение — бесконечное ожидание, как было).
inline double commitTimeoutSec() {
  static const double value = []() -> double {
    const char *env = std::getenv("DB_COMMIT_TIMEOUT_SEC");
    if (env == nullptr || *env == '\0') return 30.0;
    try {
      return std::stod(env);
    } catch (...) {
      return 30.0;
    }
  }();
  return value;
}

// Всё, что переживает приостановку корутины. Держится по `shared_ptr` и
// колбэком, и таймером, поэтому смерть кадра корутины не делает их висячими.
// `handle_` и `loop_` заполняются в конструкторе и дальше не меняются, так
// что читать их из чужого треда безопасно: передача `shared_ptr` в другой
// тред идёт через очередь цикла событий (мьютекс), а значит порядок
// установлен.
class CommitState {
 public:
  CommitState(std::coroutine_handle<> handle, trantor::EventLoop *loop)
      : handle_(handle), loop_(loop) {}

  // Возобновляет корутину РОВНО ОДИН РАЗ. Кто первым выставил `resumed_` —
  // тот и возобновляет; опоздавший (пришедший после таймера колбэк или
  // отработавший вхолостую таймер) молча уходит.
  void finish(bool success) {
    if (resumed_.exchange(true, std::memory_order_acq_rel)) return;
    ok_.store(success, std::memory_order_release);
    auto handle = handle_;
    loop_->queueInLoop([handle]() { handle.resume(); });
  }

  bool alreadyResumed() const { return resumed_.load(std::memory_order_acquire); }

  bool ok() const { return ok_.load(std::memory_order_acquire); }

 private:
  const std::coroutine_handle<> handle_;
  trantor::EventLoop *const loop_;
  std::atomic<bool> resumed_{false};
  std::atomic<bool> ok_{false};
};

struct [[nodiscard]] CommitAwaiter {
  explicit CommitAwaiter(std::shared_ptr<drogon::orm::Transaction> trans)
      : trans_(std::move(trans)) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) {
    // Петля, на которой корутину нужно возобновить: та, на которой она
    // сейчас приостанавливается (тред обработчика). Вне цикла событий
    // (теоретически — например, из теста) падаем на главную петлю приложения.
    auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    if (loop == nullptr) loop = drogon::app().getLoop();

    state_ = std::make_shared<CommitState>(handle, loop);

    // Локальная копия — единственная оставшаяся ссылка на транзакцию.
    auto trans = std::move(trans_);

    // Транзакция здесь НЕ захватывается — см. второй подводный камень выше.
    auto state = state_;
    trans->setCommitCallback([state](bool ok) { state->finish(ok); });

    const double timeout = commitTimeoutSec();
    if (timeout > 0.0) {
      // Таймер не отменяется, когда колбэк приходит вовремя: отменять его из
      // чужого треда небезопасно, а сработать вхолостую он умеет (`finish`
      // проглотит опоздавшего). Цена — `CommitState` живёт до срабатывания
      // таймера, десятки байт на транзакцию.
      loop->runAfter(timeout, [state, timeout]() {
        if (state->alreadyResumed()) return;
        LOG_ERROR << "awaitCommit: commit-колбэк не пришёл за " << timeout
                  << " s — отвечаем как на неудавшийся COMMIT. Если COMMIT всё же "
                     "дойдёт позже, данные окажутся записаны вопреки ответу 500";
        state->finish(false);
      });
    }

    // Последняя ссылка уходит -> ~TransactionImpl -> COMMIT.
    trans.reset();
  }

  bool await_resume() const noexcept { return state_->ok(); }

 private:
  std::shared_ptr<drogon::orm::Transaction> trans_;
  std::shared_ptr<CommitState> state_;
};

}  // namespace detail

// Отпускает `trans` и ждёт результат её COMMIT. Транзакцию сюда нужно
// именно ПЕРЕДАВАТЬ (`std::move`), а не копировать: любая уцелевшая у
// вызывающего ссылка отложит деструктор, а значит и COMMIT, и ожидание
// зависнет.
//
// `true` — COMMIT подтверждён сервером, данные видны другим соединениям.
// `false` — COMMIT не прошёл (Drogon уже залогировал причину) либо не
// подтвердился за `DB_COMMIT_TIMEOUT_SEC`; вызывающий обязан ответить
// `500 INTERNAL_ERROR`.
//
// Звать на откаченной транзакции нельзя — для отката есть
// `rollbackAndDiscard()`, которая транзакцию забирает.
[[nodiscard]] inline detail::CommitAwaiter awaitCommit(
    std::shared_ptr<drogon::orm::Transaction> trans) {
  return detail::CommitAwaiter(std::move(trans));
}

// Откат с изъятием транзакции у вызывающего. Структурная замена
// комментарию «не зови здесь awaitCommit»: после этого вызова живого `tx`
// у вызывающего не остаётся, и передать его в `awaitCommit` (а значит
// повиснуть навсегда в ожидании колбэка, которого после отката не будет)
// становится невыразимо.
//
// Отпускать ссылку сразу же безопасно: `TransactionImpl::rollback()` кладёт
// `shared_from_this()` в очередь цикла и в колбэки самого `rollback`, так
// что транзакция доживает до конца отката и без нашей ссылки, а деструктор
// не успеет увидеть `isCommitedOrRolledback_ == false` и послать COMMIT.
inline void rollbackAndDiscard(std::shared_ptr<drogon::orm::Transaction> trans) {
  if (!trans) return;
  trans->rollback();
}

}  // namespace common
