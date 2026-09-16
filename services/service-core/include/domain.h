#pragma once

// Domain types for deals. Deliberately free of Drogon/Postgres includes so
// this header (and domain.cc) can be unit-tested without pulling in the
// service's full dependency graph — see tests/CMakeLists.txt, which links
// only Catch2.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core_svc {

// Values below must match the CHECK constraints in
// services/service-core/migrations/002_deals.sql exactly.

enum class Stage { created, documents, compliance_check, settlement, completed, blocked };

std::string_view toString(Stage stage);
std::optional<Stage> stageFromString(std::string_view value);

enum class DocStatus { missing, uploaded, under_review, approved, rejected };

std::string_view toString(DocStatus status);
std::optional<DocStatus> docStatusFromString(std::string_view value);

enum class StepStatus { pending, in_progress, done, delayed };

std::string_view toString(StepStatus status);
std::optional<StepStatus> stepStatusFromString(std::string_view value);

enum class Scenario { cbdc, bank_transfer, smart_contract, trade_finance };

std::string_view toString(Scenario scenario);
std::optional<Scenario> scenarioFromString(std::string_view value);

// Fixed display order for scenario choices in the UI.
constexpr std::array<Scenario, 4> kScenarioOrder{
    Scenario::cbdc, Scenario::bank_transfer, Scenario::smart_contract, Scenario::trade_finance};

// import_/export_ carry a trailing underscore only because `export` is a
// C++ keyword; they still serialize to "import"/"export".
enum class OperationType { import_, export_ };

std::string_view toString(OperationType type);
std::optional<OperationType> operationTypeFromString(std::string_view value);

struct DocumentState {
  std::string kind;
  DocStatus status;
};

struct TimelineStep {
  int seq;
  std::string step;
  std::string actor;
  StepStatus status;
  std::string delay_reason;
};

struct DealState {
  Stage stage;
  std::optional<Scenario> scenario;
  std::optional<Stage> blocked_from;
  std::vector<DocumentState> documents;
  std::vector<TimelineStep> timeline;
};

// "DEAL-<year>-<display_no zero-padded to at least 4 digits>"
std::string displayId(int year, long long display_no);

// round(done * 100.0 / total); empty timeline -> 0.
int progressPercent(const std::vector<TimelineStep> &timeline);

}  // namespace core_svc
