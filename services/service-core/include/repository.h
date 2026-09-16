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
};

// Bridges the persisted row/detail into the pure state machine's DealState.
DealState toDealState(const DealDetail &detail);

// API serialization. dealSummaryJson is the shape used both standalone
// (list) and embedded in dealJson (detail).
Json::Value dealSummaryJson(const DealRow &deal);
Json::Value dealJson(const DealDetail &detail);

}  // namespace core_svc
