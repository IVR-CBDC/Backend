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

// Shared by find() (fresh connection) and apply() (inside its transaction,
// after the row lock already proved the deal exists) — a DbClientPtr works
// for both since orm::Transaction subclasses DbClient. Assumes the deal
// exists; callers that don't already know that (find()) check dealRows
// themselves first.
Task<DealDetail> loadDealDetail(std::shared_ptr<orm::DbClient> db, std::string deal_id, std::string company_id) {
  auto dealRows = co_await db->execSqlCoro(
      std::string("SELECT ") + kDealColumns + " FROM deals WHERE id = $1 AND company_id = $2", deal_id, company_id);

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

// scenarioStepActor mirrors state_machine.cc's private helper of the same
// purpose: Task 2's public interface only exposes the step *label*
// (scenarioStepLabel), not the actor, so apply() keeps its own copy for the
// one extra field it writes when a scenario is confirmed (see step 6 below).
std::string scenarioStepActor(Scenario scenario) {
  switch (scenario) {
    case Scenario::cbdc: return "Платформа ЦВЦБ";
    case Scenario::bank_transfer: return "Банк";
    case Scenario::smart_contract: return "Смарт-контракт";
    case Scenario::trade_finance: return "Банк-гарант";
  }
  return "";
}

}  // namespace

Task<std::optional<DealDetail>> DealRepository::find(std::string deal_id, std::string company_id) {
  auto db = app().getDbClient();

  auto exists = co_await db->execSqlCoro("SELECT 1 FROM deals WHERE id = $1 AND company_id = $2", deal_id, company_id);
  if (exists.size() == 0) co_return std::nullopt;

  co_return co_await loadDealDetail(db, deal_id, company_id);
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

Task<DealDetail> DealRepository::create(std::string company_id, std::string counterparty_country,
                                        std::string counterparty_name, OperationType operation_type,
                                        std::string amount, std::string currency) {
  auto db = app().getDbClient();
  auto trans = co_await db->newTransactionCoro();

  auto inserted = co_await trans->execSqlCoro(
      "INSERT INTO deals (id, company_id, counterparty_country, counterparty_name, operation_type, amount, "
      "currency, stage, version) "
      "VALUES (gen_random_uuid(), $1::uuid, $2, $3, $4, $5::numeric, $6, 'created', 0) RETURNING id::text",
      company_id, counterparty_country, counterparty_name, std::string(toString(operation_type)), amount, currency);
  const auto deal_id = inserted[0]["id"].as<std::string>();

  // initialTimeline()'s seq-1 step is already "done" (the deal itself was
  // just created "by" the user); the rest start "pending" with no
  // started_at/finished_at — mirrors what apply()'s per-step UPDATE would
  // set for a step entering/leaving those statuses.
  for (const auto &step : initialTimeline()) {
    co_await trans->execSqlCoro(
        "INSERT INTO timeline_events (id, deal_id, seq, step, actor, status, started_at, finished_at) "
        "VALUES (gen_random_uuid(), $1::uuid, $2::int, $3, $4, $5, "
        "        CASE WHEN $5 = 'done' THEN now() ELSE NULL END, "
        "        CASE WHEN $5 = 'done' THEN now() ELSE NULL END)",
        deal_id, step.seq, step.step, step.actor, std::string(toString(step.status)));
  }

  Json::Value payload;
  payload["type"] = "deal.created";
  payload["deal_id"] = deal_id;
  co_await trans->execSqlCoro("INSERT INTO outbox (company_id, topic, payload) VALUES ($1::uuid, $2, $3::jsonb)",
                              company_id, std::string("deal-events"), payload);

  co_return co_await loadDealDetail(trans, deal_id, company_id);
}

Task<ApplyResult> DealRepository::apply(std::string deal_id, std::string company_id, int expected_version,
                                        const Transition &transition, const DealMutation &mutation) {
  auto db = app().getDbClient();
  auto trans = co_await db->newTransactionCoro();

  auto locked =
      co_await trans->execSqlCoro("SELECT version FROM deals WHERE id = $1::uuid AND company_id = $2::uuid FOR UPDATE",
                                  deal_id, company_id);
  if (locked.size() == 0) co_return ApplyResult{ApplyResult::Status::not_found, std::nullopt};
  if (locked[0]["version"].as<int>() != expected_version)
    co_return ApplyResult{ApplyResult::Status::version_conflict, std::nullopt};

  std::optional<std::string> scenarioText =
      mutation.scenario ? std::optional(std::string(toString(*mutation.scenario))) : std::nullopt;
  std::optional<std::string> blockedFromText =
      transition.blocked_from ? std::optional(std::string(toString(*transition.blocked_from))) : std::nullopt;
  // deals.next_action_at is where the deal itself schedules its own next
  // step (Task 5's emulator dispatcher); a document-level mutation instead
  // schedules deal_documents.next_action_at below, so it leaves this column
  // untouched (NULL — no deal-level action pending).
  std::optional<long long> dealLevelDelaySec = mutation.document_kind ? std::nullopt : mutation.next_action_in_sec;

  co_await trans->execSqlCoro(
      "UPDATE deals SET "
      "  stage = $2, "
      "  blocked_from = $3, "
      "  blocker_reason = NULLIF($4, ''), "
      "  scenario = COALESCE($5, scenario), "
      "  commission_total = COALESCE($6::numeric, commission_total), "
      "  commission_breakdown = COALESCE($7::jsonb, commission_breakdown), "
      "  next_action_at = CASE WHEN $8::bigint IS NOT NULL THEN now() + ($8::bigint * interval '1 second') "
      "                        ELSE NULL END, "
      "  version = version + 1, "
      "  updated_at = now() "
      "WHERE id = $1::uuid",
      deal_id, std::string(toString(transition.stage)), blockedFromText, transition.blocker_reason, scenarioText,
      mutation.commission_total, mutation.commission_breakdown, dealLevelDelaySec);

  if (mutation.document_kind) {
    co_await trans->execSqlCoro(
        "UPDATE deal_documents SET "
        "  status = $1, "
        "  reject_reason = NULLIF($2, ''), "
        "  next_action_at = CASE WHEN $3::bigint IS NOT NULL THEN now() + ($3::bigint * interval '1 second') "
        "                        ELSE NULL END, "
        "  updated_at = now() "
        "WHERE deal_id = $4::uuid AND kind = $5",
        std::string(toString(mutation.document_status.value_or(DocStatus::missing))), mutation.document_reject_reason,
        mutation.next_action_in_sec, deal_id, *mutation.document_kind);
  }

  if (mutation.scenario) {
    // Documents required for the chosen scenario didn't exist before now —
    // create them as `missing` (ON CONFLICT guards a retried request that
    // already inserted them under a version that then failed to commit
    // client-side, though the row lock above makes that window vanishingly
    // small in practice).
    for (const auto &kind : requiredDocuments(*mutation.scenario)) {
      co_await trans->execSqlCoro(
          "INSERT INTO deal_documents (id, deal_id, kind, status) VALUES (gen_random_uuid(), $1::uuid, $2, 'missing') "
          "ON CONFLICT (deal_id, kind) DO NOTHING",
          deal_id, kind);
    }
  }

  for (const auto &change : transition.timeline) {
    co_await trans->execSqlCoro(
        "UPDATE timeline_events SET "
        "  status = $2, "
        "  delay_reason = NULLIF($3, ''), "
        "  started_at = COALESCE(started_at, now()), "
        "  finished_at = CASE WHEN $2 = 'done' THEN now() ELSE finished_at END "
        "WHERE deal_id = $1::uuid AND seq = $4::int",
        deal_id, std::string(toString(change.status)), change.delay_reason, change.seq);
  }

  if (mutation.scenario) {
    // Step 6 ("Расчёт") is scenario-agnostic until a scenario is chosen;
    // rename it now even though its status doesn't change here — it stays
    // "pending" until settlement actually starts (Task 5/6).
    co_await trans->execSqlCoro(
        "UPDATE timeline_events SET step = $2, actor = $3 WHERE deal_id = $1::uuid AND seq = 6", deal_id,
        scenarioStepLabel(*mutation.scenario), scenarioStepActor(*mutation.scenario));
  }

  for (const auto &draft : transition.notifications) {
    auto notification = co_await trans->execSqlCoro(
        "INSERT INTO notifications (id, company_id, deal_id, severity, message) "
        "VALUES (gen_random_uuid(), $1::uuid, $2::uuid, $3, $4) RETURNING id::text",
        company_id, deal_id, draft.severity, draft.message);
    const auto notification_id = notification[0]["id"].as<std::string>();

    Json::Value payload;
    payload["type"] = "notification.created";
    payload["notification_id"] = notification_id;
    payload["deal_id"] = deal_id;
    co_await trans->execSqlCoro("INSERT INTO outbox (company_id, topic, payload) VALUES ($1::uuid, $2, $3::jsonb)",
                                company_id, std::string("deal-events"), payload);
  }

  Json::Value payload;
  payload["type"] = "deal.updated";
  payload["deal_id"] = deal_id;
  co_await trans->execSqlCoro("INSERT INTO outbox (company_id, topic, payload) VALUES ($1::uuid, $2, $3::jsonb)",
                              company_id, std::string("deal-events"), payload);

  auto detail = co_await loadDealDetail(trans, deal_id, company_id);
  co_return ApplyResult{ApplyResult::Status::ok, std::move(detail)};
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
