#include "repository.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace drogon;

namespace core_svc {

namespace {

// SQL below formats every timestamp as ISO-8601 UTC so the C++ side never
// has to touch date/time formatting; NULL passes through as an empty string
// via to_char's own NULL-in-NULL-out behavior, handled explicitly where it
// matters (started_at/finished_at, updated_at on optional rows).
constexpr const char *kIsoFormat = R"(YYYY-MM-DD"T"HH24:MI:SS"Z")";

std::string orEmpty(const orm::Field &f) { return f.isNull() ? std::string() : f.as<std::string>(); }

Json::Value parseJsonOrEmpty(const orm::Field &f) {
  if (f.isNull()) return Json::Value(Json::objectValue);
  Json::Value out;
  Json::CharReaderBuilder builder;
  std::string text = f.as<std::string>();
  std::string errs;
  std::istringstream iss(text);
  if (!Json::parseFromStream(builder, iss, &out, &errs)) return Json::Value(Json::objectValue);
  return out;
}

DealRow rowToDealRow(const orm::Row &row) {
  DealRow d;
  d.id = row["id"].as<std::string>();
  d.company_id = row["company_id"].as<std::string>();
  d.display_no = row["display_no"].as<long long>();
  d.year = row["year"].as<int>();
  d.counterparty_country = row["counterparty_country"].as<std::string>();
  d.counterparty_name = row["counterparty_name"].as<std::string>();
  d.operation_type = operationTypeFromString(row["operation_type"].as<std::string>())
                          .value_or(OperationType::import_);
  // amount/commission_total are read as a string here (DealRow.amount /
  // commission_total) so the exact Postgres NUMERIC text round-trips to
  // service-commission without losing precision (Task 4). dealSummaryJson
  // separately parses that same string into a double purely because the
  // JSON API contract requires a number there — don't "simplify" this into
  // a single double field, it would silently truncate money.
  d.amount = row["amount"].as<std::string>();
  d.currency = row["currency"].as<std::string>();
  d.scenario = row["scenario"].isNull() ? std::nullopt : scenarioFromString(row["scenario"].as<std::string>());
  d.commission_total =
      row["commission_total"].isNull() ? std::nullopt : std::optional(row["commission_total"].as<std::string>());
  d.commission_breakdown = parseJsonOrEmpty(row["commission_breakdown"]);
  d.stage = stageFromString(row["stage"].as<std::string>()).value_or(Stage::created);
  d.blocked_from =
      row["blocked_from"].isNull() ? std::nullopt : stageFromString(row["blocked_from"].as<std::string>());
  d.blocker_reason = orEmpty(row["blocker_reason"]);
  d.version = row["version"].as<int>();
  d.created_at = row["created_at"].as<std::string>();
  d.updated_at = row["updated_at"].as<std::string>();
  return d;
}

DocumentRow rowToDocumentRow(const orm::Row &row) {
  DocumentRow doc;
  doc.id = row["id"].as<std::string>();
  doc.kind = row["kind"].as<std::string>();
  doc.title = row["title"].as<std::string>();
  doc.purpose = row["purpose"].as<std::string>();
  doc.status = docStatusFromString(row["status"].as<std::string>()).value_or(DocStatus::missing);
  doc.reject_reason = orEmpty(row["reject_reason"]);
  doc.updated_at = row["updated_at"].as<std::string>();
  return doc;
}

// dealSummaryJson only has a DealRow (no timeline — list() never fetches
// per-deal timeline rows, that would be N+1 across a whole page of deals).
// But the state machine advances "done" steps in lockstep with stage (see
// state_machine.cc): every transition that changes stage also marks exactly
// the steps up to that point done, and a block freezes the timeline at
// blocked_from's progress without touching it. So stage (+blocked_from)
// alone reproduces progressPercent(actual timeline) for every reachable
// state; dealJson still recomputes it from the real timeline once it has
// one, as the authoritative source.
int progressPercentForStage(Stage stage, std::optional<Stage> blocked_from) {
  const Stage effective = stage == Stage::blocked ? blocked_from.value_or(Stage::created) : stage;
  int done = 1;
  switch (effective) {
    case Stage::created: done = 1; break;
    case Stage::documents: done = 2; break;
    case Stage::compliance_check: done = 3; break;
    case Stage::settlement: done = 5; break;
    case Stage::completed: done = 7; break;
    case Stage::blocked: done = 1; break;  // unreachable (effective is never `blocked`)
  }
  return static_cast<int>(std::lround(done * 100.0 / 7.0));
}

TimelineStep rowToTimelineStep(const orm::Row &row) {
  TimelineStep step;
  step.seq = row["seq"].as<int>();
  step.step = row["step"].as<std::string>();
  step.actor = row["actor"].as<std::string>();
  step.status = stepStatusFromString(row["status"].as<std::string>()).value_or(StepStatus::pending);
  step.delay_reason = orEmpty(row["delay_reason"]);
  step.started_at = orEmpty(row["started_at"]);
  step.finished_at = orEmpty(row["finished_at"]);
  return step;
}

const char *kDealColumns = R"(
    id::text AS id,
    company_id::text AS company_id,
    display_no,
    EXTRACT(YEAR FROM created_at)::int AS year,
    counterparty_country,
    counterparty_name,
    operation_type,
    amount,
    currency,
    scenario,
    commission_total,
    commission_breakdown,
    stage,
    blocked_from,
    blocker_reason,
    version,
    to_char(created_at, 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
    to_char(updated_at, 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at
)";

}  // namespace

Task<std::optional<DealDetail>> DealRepository::find(std::string deal_id, std::string company_id) {
  auto db = app().getDbClient();

  auto dealRows = co_await db->execSqlCoro(
      std::string("SELECT ") + kDealColumns + " FROM deals WHERE id = $1 AND company_id = $2", deal_id, company_id);
  if (dealRows.size() == 0) co_return std::nullopt;

  DealDetail detail;
  detail.deal = rowToDealRow(dealRows[0]);

  auto docRows = co_await db->execSqlCoro(
      "SELECT dd.id::text AS id, dd.kind, dk.title, dk.purpose, dd.status, dd.reject_reason, "
      "       to_char(dd.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS updated_at "
      "FROM deal_documents dd JOIN document_kinds dk ON dk.kind = dd.kind "
      "WHERE dd.deal_id = $1 ORDER BY dk.kind",
      deal_id);
  detail.documents.reserve(docRows.size());
  for (const auto &row : docRows) detail.documents.push_back(rowToDocumentRow(row));

  auto timelineRows = co_await db->execSqlCoro(
      "SELECT seq, step, actor, status, delay_reason, "
      "       to_char(started_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS started_at, "
      "       to_char(finished_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS finished_at "
      "FROM timeline_events WHERE deal_id = $1 ORDER BY seq",
      deal_id);
  detail.timeline.reserve(timelineRows.size());
  for (const auto &row : timelineRows) detail.timeline.push_back(rowToTimelineStep(row));

  co_return detail;
}

Task<std::vector<DealRow>> DealRepository::list(std::string company_id, int limit) {
  int clamped = std::clamp(limit, 1, 100);
  auto db = app().getDbClient();

  // Postgres infers LIMIT's parameter type as bigint (int8) by default; an
  // explicit ::int cast keeps it int4 so it matches the 4 bytes Drogon
  // sends for a C++ `int` — without it the server rejects the bind with
  // "insufficient data left in message" (int4 payload read as int8).
  auto rows = co_await db->execSqlCoro(
      std::string("SELECT ") + kDealColumns +
          " FROM deals WHERE company_id = $1 ORDER BY updated_at DESC LIMIT $2::int",
      company_id, clamped);

  std::vector<DealRow> out;
  out.reserve(rows.size());
  for (const auto &row : rows) out.push_back(rowToDealRow(row));
  co_return out;
}

DealState toDealState(const DealDetail &detail) {
  DealState state;
  state.stage = detail.deal.stage;
  state.scenario = detail.deal.scenario;
  state.blocked_from = detail.deal.blocked_from;
  state.documents.reserve(detail.documents.size());
  for (const auto &doc : detail.documents) state.documents.push_back({doc.kind, doc.status});
  state.timeline = detail.timeline;
  return state;
}

Json::Value dealSummaryJson(const DealRow &deal) {
  Json::Value j;
  j["id"] = deal.id;
  j["display_id"] = displayId(deal.year, deal.display_no);
  j["counterparty_name"] = deal.counterparty_name;
  j["counterparty_country"] = deal.counterparty_country;
  j["operation_type"] = std::string(toString(deal.operation_type));
  // See rowToDealRow: intentional lossy parse, string stays canonical.
  j["amount"] = std::stod(deal.amount);
  j["currency"] = deal.currency;
  j["scenario"] = deal.scenario ? Json::Value(std::string(toString(*deal.scenario))) : Json::Value();
  j["stage"] = std::string(toString(deal.stage));
  j["progress_percent"] = progressPercentForStage(deal.stage, deal.blocked_from);
  j["needs_attention"] = deal.stage == Stage::blocked;
  j["attention_reason"] = deal.stage == Stage::blocked ? Json::Value(deal.blocker_reason) : Json::Value();
  j["commission_total"] = deal.commission_total ? Json::Value(std::stod(*deal.commission_total)) : Json::Value();
  j["version"] = deal.version;
  j["updated_at"] = deal.updated_at;
  return j;
}

Json::Value dealJson(const DealDetail &detail) {
  Json::Value j = dealSummaryJson(detail.deal);
  j["progress_percent"] = progressPercent(detail.timeline);
  j["created_at"] = detail.deal.created_at;

  Json::Value documents(Json::arrayValue);
  for (const auto &doc : detail.documents) {
    Json::Value d;
    d["id"] = doc.id;
    d["kind"] = doc.kind;
    d["title"] = doc.title;
    d["purpose"] = doc.purpose;
    d["status"] = std::string(toString(doc.status));
    d["reject_reason"] = doc.reject_reason;
    documents.append(d);
  }
  j["documents"] = documents;

  Json::Value timeline(Json::arrayValue);
  for (const auto &step : detail.timeline) {
    Json::Value t;
    t["seq"] = step.seq;
    t["step"] = step.step;
    t["actor"] = step.actor;
    t["status"] = std::string(toString(step.status));
    t["delay_reason"] = step.delay_reason;
    t["started_at"] = step.started_at;
    t["finished_at"] = step.finished_at;
    timeline.append(t);
  }
  j["timeline"] = timeline;

  return j;
}

}  // namespace core_svc
