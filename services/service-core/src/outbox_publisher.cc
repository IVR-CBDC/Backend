#include "outbox_publisher.h"

#include "common/commit.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace drogon;

namespace core_svc {

namespace {

double envDouble(const char *name, double fallback) {
  const char *env = std::getenv(name);
  if (!env || !*env) return fallback;
  try {
    return std::stod(env);
  } catch (...) {
    return fallback;
  }
}

Json::Value parseJson(const std::string &text) {
  Json::Value out;
  Json::CharReaderBuilder builder;
  std::string errs;
  std::istringstream iss(text);
  if (!Json::parseFromStream(builder, iss, &out, &errs)) return Json::Value(Json::objectValue);
  return out;
}

std::string writeJsonCompact(const Json::Value &value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}

}  // namespace

void OutboxPublisher::start() {
  const double tickSec = envDouble("OUTBOX_TICK_SEC", 0.5);
  app().getLoop()->runEvery(tickSec, []() {
    drogon::async_run([]() -> Task<> {
      try {
        co_await OutboxPublisher::publishOnce();
      } catch (const std::exception &e) {
        LOG_ERROR << "outbox publish failed: " << e.what();
      } catch (...) {
        LOG_ERROR << "outbox publish failed with an unknown exception";
      }
    });
  });
}

Task<int> OutboxPublisher::publishOnce() {
  auto db = app().getDbClient();
  auto trans = co_await db->newTransactionCoro();

  // FOR UPDATE SKIP LOCKED claims rows for this run only; the same
  // transaction's UPDATE below (published_at = now()) is what makes
  // publish-then-commit at-least-once: if the process dies after PUBLISH
  // but before COMMIT, the rows stay unpublished and are republished next
  // run. The BFF/UI dedupe on `seq` (the outbox row id), so a repeat is
  // harmless.
  //
  // `seq` is NOT a delivery-order guarantee, only a unique delivery id.
  // `outbox.id` (a bigserial) is assigned at INSERT time, but a row only
  // becomes visible to this SELECT once its own transaction COMMITs — and
  // two overlapping DealRepository::apply() transactions can commit in
  // either order regardless of which one inserted its outbox row (and got
  // its id) first. So a consumer can observe a higher `seq` before a lower
  // one. This is fine because every payload here is just a refetch trigger
  // (`deal.updated`/`notification.created` carry no state of their own —
  // the consumer always re-reads the deal from service-core), so ordering
  // doesn't matter; consumers must de-duplicate retried deliveries by
  // remembering the set of `seq` values already seen, never by comparing
  // against a highest-seq-so-far watermark (that would drop a legitimately
  // out-of-order, not-yet-seen event).
  auto rows = co_await trans->execSqlCoro(
      "SELECT id, company_id::text AS company_id, payload FROM outbox "
      "WHERE published_at IS NULL ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 100");
  if (rows.size() == 0) co_return 0;

  // getRedisClient() returns a null RedisClientPtr (not a throw) when
  // config.json has no "default" redis client — dereferencing it below would
  // segfault, not throw, so this must be checked before the loop rather than
  // caught around it. Logged once per tick (not once per row) and rows are
  // left unpublished (the SELECT above didn't commit its own transaction —
  // returning here without touching published_at rolls the claim back) so a
  // misconfigured Redis degrades into "events stop flowing" rather than a
  // crash loop.
  auto redis = app().getRedisClient();
  if (!redis) {
    LOG_ERROR << "outbox publisher: no redis client configured (redis_clients missing from config.json?)";
    co_return 0;
  }
  std::vector<long long> ids;
  ids.reserve(rows.size());
  for (const auto &row : rows) {
    const long long id = row["id"].as<long long>();
    const std::string company_id = row["company_id"].as<std::string>();

    Json::Value payload = parseJson(row["payload"].as<std::string>());
    payload["seq"] = static_cast<Json::Int64>(id);
    const std::string message = writeJsonCompact(payload);
    const std::string channel = "deal-events:" + company_id;

    co_await redis->execCommandCoro("PUBLISH %s %s", channel.c_str(), message.c_str());
    ids.push_back(id);
  }

  // Bound to a std::vector<long long> would need Drogon's SqlBinder to know
  // how to serialize it as a Postgres array, which it doesn't for numeric
  // vectors; a literal "{1,2,3}" cast to bigint[] gets the same
  // `id = ANY(...)` semantics the brief specifies.
  std::ostringstream idArray;
  idArray << "{";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) idArray << ",";
    idArray << ids[i];
  }
  idArray << "}";

  co_await trans->execSqlCoro("UPDATE outbox SET published_at = now() WHERE id = ANY($1::bigint[])",
                              idArray.str());

  // Порядок PUBLISH -> UPDATE -> COMMIT здесь оставлен НАМЕРЕННО, в отличие
  // от register.cc и repository.cc (см. common/commit.h и отчёт задачи 5).
  //
  // Тревога «событие о сделке, которой в базе может не оказаться» на этот
  // код не распространяется: строка outbox становится видна SELECT'у выше
  // только после COMMIT ТОЙ транзакции, которая её вставила вместе с самой
  // сделкой (transactional outbox, спека §4.4). То есть к моменту PUBLISH
  // сделка уже durable. Здешняя транзакция пишет только `published_at`.
  //
  // Переставить PUBLISH после COMMIT нельзя: падение между COMMIT и PUBLISH
  // потеряло бы событие навсегда (at-most-once), а спека §4.4 требует
  // at-least-once с дедупликацией по `seq`. Нынешний порядок в худшем
  // случае даёт повтор доставки, который потребитель и так обязан гасить.
  //
  // Ждать COMMIT всё же нужно — ради честности: без ожидания неудачный
  // COMMIT проходил молча, и тик рапортовал «опубликовано N», хотя
  // `published_at` не сохранился и те же строки уедут в Redis повторно.
  if (!co_await common::awaitCommit(std::move(trans))) {
    LOG_ERROR << "outbox publisher: COMMIT не прошёл, published_at для " << ids.size()
              << " строк не сохранён — они будут опубликованы повторно на следующем тике "
                 "(доставка at-least-once, потребитель дедуплицирует по seq)";
    co_return 0;
  }

  co_return static_cast<int>(ids.size());
}

}  // namespace core_svc
