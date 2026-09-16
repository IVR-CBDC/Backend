#pragma once

// Pure deal state machine: given a DealState snapshot and an event, decides
// the next stage/timeline/notifications, or rejects the transition. No I/O,
// no Drogon, no database — callers (the repository layer, task 3/4) load a
// DealState, call one of these functions, and persist the result.

#include <string>
#include <variant>
#include <vector>

#include "domain.h"

namespace core_svc {

struct TimelineChange {
  int seq;
  StepStatus status;
  std::string delay_reason;
};

struct NotificationDraft {
  std::string severity;
  std::string message;
};

struct Transition {
  Stage stage;
  std::optional<Stage> blocked_from;
  std::string blocker_reason;
  std::vector<TimelineChange> timeline;
  std::vector<NotificationDraft> notifications;
  bool schedule_next = false;  // true when the emulator should act next
  // True only for the blocked -> documents transition produced by
  // onDocumentSubmitted (a resubmission that unblocks the deal): tells
  // apply() to also re-arm next_action_at for every *other* document of this
  // deal still sitting in uploaded/under_review, so a sibling that was
  // stranded when the deal first blocked (see F3 in the plan-03 final
  // review) gets picked up by the emulator again instead of sitting in
  // under_review forever.
  bool rearm_pending_documents = false;
};

struct TransitionError {
  std::string code;
  std::string message;
};

using TransitionResult = std::variant<Transition, TransitionError>;

// Seven timeline steps per spec §4.2; the first is already "done", the rest
// "pending". Step 6's step/actor are the scenario-agnostic placeholder
// ("Расчёт" / "Банк") — the repository updates them via scenarioStepLabel
// once a scenario is chosen.
std::vector<TimelineStep> initialTimeline();

// Document kinds required for a scenario, in document_kinds order. Mirrors
// the seeded document_kinds.required_for arrays; Task 6 tests that they
// stay in sync.
std::vector<std::string> requiredDocuments(Scenario scenario);

// Label of the 6th timeline step ("расчёт") once a scenario is confirmed.
std::string scenarioStepLabel(Scenario scenario);

TransitionResult onScenarioConfirmed(const DealState &deal);
TransitionResult onDocumentSubmitted(const DealState &deal, const std::string &kind);
TransitionResult onDocumentReviewed(const DealState &deal, const std::string &kind, bool approved,
                                     const std::string &reject_reason);
TransitionResult onComplianceResult(const DealState &deal, bool approved, const std::string &reason);
// Timeline-only pause on the "Проверка ФНС" step (seq 5), no stage change:
// routes the emulator's compliance-delay branch through apply() like every
// other transition, so it gets a deal.updated event, a version bump, and an
// updated_at touch instead of bypassing all three (see F4 in the plan-03
// final review). Only valid while the deal is in compliance_check.
TransitionResult onComplianceDelayed(const DealState &deal);
TransitionResult onSettlementDone(const DealState &deal);

}  // namespace core_svc
