#include "outbox_publisher.h"

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
  auto rows = co_await trans->execSqlCoro(
      "SELECT id, company_id::text AS company_id, payload FROM outbox "
      "WHERE published_at IS NULL ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 100");
  if (rows.size() == 0) co_return 0;

  auto redis = app().getRedisClient();
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

  co_return static_cast<int>(ids.size());
}

}  // namespace core_svc
