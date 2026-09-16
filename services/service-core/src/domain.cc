#include "domain.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace core_svc {

std::string_view toString(Stage stage) {
  switch (stage) {
    case Stage::created: return "created";
    case Stage::documents: return "documents";
    case Stage::compliance_check: return "compliance_check";
    case Stage::settlement: return "settlement";
    case Stage::completed: return "completed";
    case Stage::blocked: return "blocked";
  }
  return "";
}

std::optional<Stage> stageFromString(std::string_view value) {
  if (value == "created") return Stage::created;
  if (value == "documents") return Stage::documents;
  if (value == "compliance_check") return Stage::compliance_check;
  if (value == "settlement") return Stage::settlement;
  if (value == "completed") return Stage::completed;
  if (value == "blocked") return Stage::blocked;
  return std::nullopt;
}

std::string_view toString(DocStatus status) {
  switch (status) {
    case DocStatus::missing: return "missing";
    case DocStatus::uploaded: return "uploaded";
    case DocStatus::under_review: return "under_review";
    case DocStatus::approved: return "approved";
    case DocStatus::rejected: return "rejected";
  }
  return "";
}

std::optional<DocStatus> docStatusFromString(std::string_view value) {
  if (value == "missing") return DocStatus::missing;
  if (value == "uploaded") return DocStatus::uploaded;
  if (value == "under_review") return DocStatus::under_review;
  if (value == "approved") return DocStatus::approved;
  if (value == "rejected") return DocStatus::rejected;
  return std::nullopt;
}

std::string_view toString(StepStatus status) {
  switch (status) {
    case StepStatus::pending: return "pending";
    case StepStatus::in_progress: return "in_progress";
    case StepStatus::done: return "done";
    case StepStatus::delayed: return "delayed";
  }
  return "";
}

std::optional<StepStatus> stepStatusFromString(std::string_view value) {
  if (value == "pending") return StepStatus::pending;
  if (value == "in_progress") return StepStatus::in_progress;
  if (value == "done") return StepStatus::done;
  if (value == "delayed") return StepStatus::delayed;
  return std::nullopt;
}

std::string_view toString(Scenario scenario) {
  switch (scenario) {
    case Scenario::cbdc: return "cbdc";
    case Scenario::bank_transfer: return "bank_transfer";
    case Scenario::smart_contract: return "smart_contract";
    case Scenario::trade_finance: return "trade_finance";
  }
  return "";
}

std::optional<Scenario> scenarioFromString(std::string_view value) {
  if (value == "cbdc") return Scenario::cbdc;
  if (value == "bank_transfer") return Scenario::bank_transfer;
  if (value == "smart_contract") return Scenario::smart_contract;
  if (value == "trade_finance") return Scenario::trade_finance;
  return std::nullopt;
}

std::string_view toString(OperationType type) {
  switch (type) {
    case OperationType::import_: return "import";
    case OperationType::export_: return "export";
  }
  return "";
}

std::optional<OperationType> operationTypeFromString(std::string_view value) {
  if (value == "import") return OperationType::import_;
  if (value == "export") return OperationType::export_;
  return std::nullopt;
}

std::string displayId(int year, long long display_no) {
  std::ostringstream out;
  out << "DEAL-" << year << "-" << std::setw(4) << std::setfill('0') << display_no;
  return out.str();
}

int progressPercent(const std::vector<TimelineStep> &timeline) {
  if (timeline.empty()) return 0;
  long done = 0;
  for (const auto &step : timeline)
    if (step.status == StepStatus::done) ++done;
  return static_cast<int>(std::lround(done * 100.0 / static_cast<double>(timeline.size())));
}

}  // namespace core_svc
