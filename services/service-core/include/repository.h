#pragma once

// Read/write access to the `deals` schema (Task 1) plus the JSON <-> DealState
// bridge into the pure state machine (Task 2). Drogon and Json types are
// allowed here — domain.h/state_machine.h stay free of them so the state
// machine can be unit-tested without the full service dependency graph.

#include <drogon/HttpTypes.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <optional>
#include <string>
#include <vector>

#include "domain.h"
#include "state_machine.h"

namespace core_svc {

struct DealRow {
  std::string id;
  std::string company_id;
  std::string counterparty_country;
  std::string counterparty_name;
  std::string currency;
  long long display_no;
  int year;
  OperationType operation_type;
  // Kept as a string (not double) so the value round-trips to
  // service-commission and back without losing NUMERIC precision.
  std::string amount;
  std::optional<Scenario> scenario;
  std::optional<std::string> commission_total;
  Json::Value commission_breakdown;
  Stage stage;
  std::optional<Stage> blocked_from;
  std::string blocker_reason;
  int version;
  std::string created_at;
  std::string updated_at;
};

struct DocumentRow {
  std::string id;
  std::string kind;
  std::string title;
  std::string purpose;
  DocStatus status;
  std::string reject_reason;
  std::string updated_at;
};

struct DealDetail {
  DealRow deal;
  std::vector<DocumentRow> documents;
  std::vector<TimelineStep> timeline;
};

// Inputs the state machine can't produce by itself (Task 2's Transition
// only knows stage/timeline/notifications) but that a write still needs to
// persist: the authoritative commission just fetched, or which document
// changed status. Optional fields are "leave column unchanged"; document_*
// only take effect when document_kind is set (paired at the call site).
struct DealMutation {
  std::optional<Scenario> scenario;
  std::optional<std::string> commission_total;
  Json::Value commission_breakdown;
  std::optional<std::string> document_kind;
  std::optional<DocStatus> document_status;
  std::string document_reject_reason;
  std::optional<long long> next_action_in_sec;
};

struct ApplyResult {
  enum class Status { ok, not_found, version_conflict } status;
  std::optional<DealDetail> deal;
};

class DealRepository {
 public:
  // Loads a deal by id, scoped to company_id (cross-tenant lookups return
  // nullopt, same as "not found" — the API never reveals that a deal exists
  // in another company). Three queries (deal, documents, timeline); no N+1.
  static drogon::Task<std::optional<DealDetail>> find(std::string deal_id,
                                                       std::string company_id);

  // Company's deals, most recently updated first. `limit` is clamped to
  // 1..100 by the caller before reaching here (see deals.cc).
  static drogon::Task<std::vector<DealRow>> list(std::string company_id, int limit);

  // Creates a deal (stage=created, version=0), its seven initialTimeline()
  // rows, and a `deal.created` outbox event, all in one transaction. Callers
  // (deals.cc) validate the fields before calling this — create() trusts
  // them as-is.
  static drogon::Task<DealDetail> create(std::string company_id, std::string counterparty_country,
                                         std::string counterparty_name, OperationType operation_type,
                                         std::string amount, std::string currency);

  // The one writer every state-machine-driven endpoint (Task 4's
  // chooseScenario/submitDocument, and Task 5/6's compliance/settlement/
  // document-review handlers) goes through: re-checks `expected_version`
  // under a row lock, applies `transition` and `mutation`, and returns the
  // fresh DealDetail — all in one transaction so a concurrent request never
  // sees a half-applied stage change.
  static drogon::Task<ApplyResult> apply(std::string deal_id, std::string company_id, int expected_version,
                                         const Transition &transition, const DealMutation &mutation);
};

// Bridges the persisted row/detail into the pure state machine's DealState.
DealState toDealState(const DealDetail &detail);

// API serialization. dealSummaryJson is the shape used both standalone
// (list) and embedded in dealJson (detail).
Json::Value dealSummaryJson(const DealRow &deal);
Json::Value dealJson(const DealDetail &detail);

}  // namespace core_svc
