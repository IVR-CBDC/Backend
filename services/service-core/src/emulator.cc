#include "emulator.h"

#include "repository.h"
#include "state_machine.h"

#include <algorithm>
#include <cstdlib>
#include <string>

using namespace drogon;

namespace core_svc {

namespace {

long long envLongLong(const char *name, long long fallback) {
  const char *env = std::getenv(name);
  if (!env || !*env) return fallback;
  try {
    return std::stoll(env);
  } catch (...) {
    return fallback;
  }
}

double envDouble(const char *name, double fallback) {
  const char *env = std::getenv(name);
  if (!env || !*env) return fallback;
  try {
    return std::stod(env);
  } catch (...) {
    return fallback;
  }
}

bool envFlag(const char *name, bool fallback) {
  const char *env = std::getenv(name);
  if (!env || !*env) return fallback;
  return std::string(env) == "true";
}

}  // namespace

unsigned long long fnv1a(std::string_view data) {
  // Standard 64-bit FNV-1a constants (offset basis / prime). Deliberately
  // not std::hash: libstdc++ makes no cross-run/cross-platform stability
  // guarantee, and this hash's whole point is to be the same everywhere.
  unsigned long long hash = 14695981039346656037ULL;
  for (unsigned char c : data) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

EmulatorConfig emulatorConfigFromEnv() {
  const char *env = std::getenv("EMULATOR_SPEED");
  const bool realistic = env && std::string(env) == "realistic";
  if (realistic) return EmulatorConfig{120, 300, 180};
  return EmulatorConfig{5, 8, 6};
}

bool emulatorEnabled() { return envFlag("EMULATOR_ENABLED", true); }

bool emulatorManual() { return envFlag("EMULATOR_MANUAL", false); }

bool documentApproved(std::string_view deal_id, std::string_view kind, int attempt) {
  // attempt 1 (and beyond) always succeeds — see emulator.h's comment: the
  // demo must show recovery, and re-rejecting a resubmission would leave the
  // user with no lever to unblock the deal at all.
  if (attempt >= 1) return true;
  const std::string key = std::string(deal_id) + ":" + std::string(kind);
  return fnv1a(key) % 10 != 0;
}

std::string documentRejectReason(std::string_view deal_id, std::string_view kind) {
  const std::string key = std::string(deal_id) + ":" + std::string(kind);
  static const std::string kReasons[] = {"Скан нечитаем", "Сумма не совпадает с инвойсом"};
  return kReasons[fnv1a(key) % 2];
}

bool complianceDelayed(std::string_view deal_id) {
  const std::string key = std::string(deal_id) + ":compliance";
  return fnv1a(key) % 5 == 0;
}

namespace {

// The timeline seq of the "Проверка ФНС" step (see state_machine.cc's
// initialTimeline) — the one the emulator can pause on before approving
// compliance.
constexpr int kFnsStepSeq = 5;

struct DueDocument {
  std::string id;
  std::string deal_id;
  std::string kind;
  std::string status;
  std::string company_id;
  int submit_count;
};

struct DueDeal {
  std::string id;
  std::string company_id;
  std::string stage;
};

// Schedules the deal's own next action. apply() (Task 4) always clears
// deals.next_action_at to NULL whenever a mutation carries a document_kind
// (see repository.cc's dealLevelDelaySec) — by design, since a document
// mutation is normally paired with deal_documents.next_action_at instead.
// That leaves nobody to schedule the deal-level wakeup for the one
// document-triggered transition that *does* need one (all documents
// approved -> compliance_check starts): this helper is that follow-up write.
Task<> scheduleDealAction(std::shared_ptr<orm::DbClient> db, const std::string &deal_id, long long delay_sec) {
  co_await db->execSqlCoro(
      "UPDATE deals SET next_action_at = now() + ($1::bigint * interval '1 second') WHERE id = $2::uuid", delay_sec,
      deal_id);
}

// Clears a document's next_action_at without touching its status — used by
// reviewDocument() when the document can no longer be usefully reviewed
// (see the Stage::documents guard there): stops the tick from picking the
// row up again, without pretending a review happened.
Task<> clearDocumentAction(std::shared_ptr<orm::DbClient> db, const std::string &document_id) {
  co_await db->execSqlCoro("UPDATE deal_documents SET next_action_at = NULL WHERE id = $1::uuid", document_id);
}

// Reviews one under_review document: decides the verdict deterministically,
// then drives it through the same onDocumentReviewed/apply path a user
// action would use. Only a benign version_conflict is expected here (the
// deal moved on between the claiming SELECT and this call) — anything else
// is logged, not thrown, per the brief's "DB errors inside a tick are
// logged and don't take the service down".
Task<bool> reviewDocument(const EmulatorConfig &cfg, const DueDocument &doc) {
  auto detail = co_await DealRepository::find(doc.deal_id, doc.company_id);
  if (!detail) co_return false;

  if (detail->deal.stage != Stage::documents) {
    // The deal left `documents` before this tick got to review doc (most
    // commonly: a sibling document, reviewed earlier in this same tick or a
    // previous one, was rejected and blocked the deal first). Nothing in
    // this system moves the deal back to `documents` on its own — only a
    // user resubmitting the *rejected* document does, and that resubmission
    // goes through onDocumentSubmitted/apply (documents.cc), which sets its
    // own next_action_at independently of this row. So retrying `doc` is
    // never going to succeed on its own: treat it as terminal (clear
    // next_action_at, leave status as under_review) rather than looping —
    // rescheduling would just re-hit this same branch forever and leave the
    // document stuck in a UI-visible "под проверкой" limbo indefinitely.
    LOG_INFO << "emulator: leaving document " << doc.id << " (deal " << doc.deal_id
             << ") unreviewed — deal left stage=documents before its turn";
    co_await clearDocumentAction(app().getDbClient(), doc.id);
    co_return false;
  }

  // submit_count is incremented by apply() at the moment this very
  // submission set the document to `uploaded` (see repository.cc), so by the
  // time the tick reviews it, submit_count - 1 is the number of *prior*
  // submissions of this kind — exactly the `attempt` documentApproved wants.
  const int attempt = doc.submit_count > 0 ? doc.submit_count - 1 : 0;
  const bool approved = documentApproved(doc.deal_id, doc.kind, attempt);
  const std::string reason = approved ? "" : documentRejectReason(doc.deal_id, doc.kind);

  auto transitionResult = onDocumentReviewed(toDealState(*detail), doc.kind, approved, reason);
  if (std::holds_alternative<TransitionError>(transitionResult)) {
    LOG_ERROR << "emulator: onDocumentReviewed rejected deal " << doc.deal_id << ": "
              << std::get<TransitionError>(transitionResult).message;
    co_return false;
  }
  const auto &transition = std::get<Transition>(transitionResult);

  DealMutation mutation;
  mutation.document_kind = doc.kind;
  mutation.document_status = approved ? DocStatus::approved : DocStatus::rejected;
  mutation.document_reject_reason = reason;
  // next_action_in_sec left unset -> the document's own next_action_at goes
  // to NULL (its review is finished either way).

  auto result = co_await DealRepository::apply(doc.deal_id, doc.company_id, detail->deal.version, transition,
                                                mutation);
  if (result.status != ApplyResult::Status::ok) co_return false;

  if (transition.schedule_next) {
    // Only the "all required documents approved" branch sets schedule_next
    // for this transition, and it always moves to compliance_check.
    co_await scheduleDealAction(app().getDbClient(), doc.deal_id, cfg.compliance_sec);
  }
  co_return true;
}

Task<bool> advanceComplianceCheck(const EmulatorConfig &cfg, const DueDeal &deal) {
  auto detail = co_await DealRepository::find(deal.id, deal.company_id);
  if (!detail) co_return false;

  auto stepIt = std::find_if(detail->timeline.begin(), detail->timeline.end(),
                              [](const TimelineStep &s) { return s.seq == kFnsStepSeq; });
  const bool alreadyDelayed = stepIt != detail->timeline.end() && stepIt->status == StepStatus::delayed;

  if (complianceDelayed(deal.id) && !alreadyDelayed) {
    // First time we see this deal delayed: mark the ФНС step, push the
    // deadline out, and leave the deal in compliance_check. The next tick
    // that finds this deal due will see alreadyDelayed == true and fall
    // through to approval below instead of delaying it again.
    //
    // Routed through onComplianceDelayed/apply() (F4) rather than raw
    // UPDATEs: this used to bypass versioning and eventing entirely (no
    // deal.updated, no updated_at/version bump), the only write path in the
    // system that did.
    auto transitionResult = onComplianceDelayed(toDealState(*detail));
    if (std::holds_alternative<TransitionError>(transitionResult)) {
      LOG_ERROR << "emulator: onComplianceDelayed rejected deal " << deal.id << ": "
                << std::get<TransitionError>(transitionResult).message;
      co_return false;
    }
    const auto &delayTransition = std::get<Transition>(transitionResult);

    DealMutation delayMutation;
    delayMutation.next_action_in_sec = cfg.compliance_sec;

    auto delayResult =
        co_await DealRepository::apply(deal.id, deal.company_id, detail->deal.version, delayTransition, delayMutation);
    co_return delayResult.status == ApplyResult::Status::ok;
  }

  auto transitionResult = onComplianceResult(toDealState(*detail), /*approved=*/true, "");
  if (std::holds_alternative<TransitionError>(transitionResult)) {
    LOG_ERROR << "emulator: onComplianceResult rejected deal " << deal.id << ": "
              << std::get<TransitionError>(transitionResult).message;
    co_return false;
  }
  const auto &transition = std::get<Transition>(transitionResult);

  DealMutation mutation;
  mutation.next_action_in_sec = cfg.settlement_sec;  // schedule_next -> settlement next

  auto result = co_await DealRepository::apply(deal.id, deal.company_id, detail->deal.version, transition, mutation);
  co_return result.status == ApplyResult::Status::ok;
}

Task<bool> advanceSettlement(const DueDeal &deal) {
  auto detail = co_await DealRepository::find(deal.id, deal.company_id);
  if (!detail) co_return false;

  auto transitionResult = onSettlementDone(toDealState(*detail));
  if (std::holds_alternative<TransitionError>(transitionResult)) {
    LOG_ERROR << "emulator: onSettlementDone rejected deal " << deal.id << ": "
              << std::get<TransitionError>(transitionResult).message;
    co_return false;
  }
  const auto &transition = std::get<Transition>(transitionResult);

  // Not `{}`: a temporary bound to apply()'s `const DealMutation &` would be
  // destroyed once this call returns its Task<> (coroutines only suspend at
  // their first co_await, but the *caller's* full-expression ends here) —
  // by the time apply()'s body actually runs the mutation after co_await,
  // the reference would already dangle. A named local outlives the co_await.
  DealMutation mutation;
  auto result = co_await DealRepository::apply(deal.id, deal.company_id, detail->deal.version, transition, mutation);
  co_return result.status == ApplyResult::Status::ok;
}

}  // namespace

void Emulator::start(const EmulatorConfig & /*cfg*/) {
  // tickOnce() re-reads emulatorConfigFromEnv() itself (see below) rather
  // than closing over `cfg` here, so the manual /internal/emulator/tick
  // route gets the same config without needing start() to ever have run.
  const double tickSec = envDouble("EMULATOR_TICK_SEC", 2.0);
  app().getLoop()->runEvery(tickSec, []() {
    drogon::async_run([]() -> Task<> {
      try {
        co_await Emulator::tickOnce();
      } catch (const std::exception &e) {
        LOG_ERROR << "emulator tick failed: " << e.what();
      } catch (...) {
        LOG_ERROR << "emulator tick failed with an unknown exception";
      }
    });
  });
}

Task<int> Emulator::tickOnce() {
  const EmulatorConfig cfg = emulatorConfigFromEnv();
  auto db = app().getDbClient();
  int processed = 0;

  try {
    // Claim due documents. This SELECT ... FOR UPDATE ... SKIP LOCKED runs
    // as its own (implicit, autocommitted) statement rather than inside a
    // held-open transaction: the reviewed-document branch below calls
    // DealRepository::apply(), which itself locks and updates this very
    // deal_documents row in a *separate* transaction/connection — holding
    // our own lock across that call would deadlock the two against each
    // other. SKIP LOCKED still protects against two overlapping ticks
    // picking exactly the same row at the same instant.
    auto docRows = co_await db->execSqlCoro(
        "SELECT d.id::text AS id, d.deal_id::text AS deal_id, d.kind, d.status, dl.company_id::text AS company_id, "
        "       d.submit_count "
        "FROM deal_documents d JOIN deals dl ON dl.id = d.deal_id "
        "WHERE d.next_action_at IS NOT NULL AND d.next_action_at <= now() "
        "FOR UPDATE OF d SKIP LOCKED LIMIT 50");

    std::vector<DueDocument> toReview;
    for (const auto &row : docRows) {
      DueDocument doc{row["id"].as<std::string>(),      row["deal_id"].as<std::string>(),
                       row["kind"].as<std::string>(),    row["status"].as<std::string>(),
                       row["company_id"].as<std::string>(), row["submit_count"].as<int>()};
      if (doc.status == "uploaded") {
        co_await db->execSqlCoro(
            "UPDATE deal_documents SET status = 'under_review', "
            "  next_action_at = now() + ($1::bigint * interval '1 second') "
            "WHERE id = $2::uuid",
            cfg.doc_review_sec, doc.id);
        ++processed;
      } else if (doc.status == "under_review") {
        toReview.push_back(std::move(doc));
      }
    }

    for (const auto &doc : toReview) {
      if (co_await reviewDocument(cfg, doc)) ++processed;
    }
  } catch (const std::exception &e) {
    LOG_ERROR << "emulator: document tick failed: " << e.what();
  }

  try {
    auto dealRows = co_await db->execSqlCoro(
        "SELECT id::text AS id, company_id::text AS company_id, stage FROM deals "
        "WHERE next_action_at IS NOT NULL AND next_action_at <= now() AND stage <> 'completed' "
        "FOR UPDATE SKIP LOCKED LIMIT 50");

    std::vector<DueDeal> dueDeals;
    dueDeals.reserve(dealRows.size());
    for (const auto &row : dealRows)
      dueDeals.push_back(
          DueDeal{row["id"].as<std::string>(), row["company_id"].as<std::string>(), row["stage"].as<std::string>()});

    for (const auto &deal : dueDeals) {
      bool ok = false;
      if (deal.stage == "compliance_check") {
        ok = co_await advanceComplianceCheck(cfg, deal);
      } else if (deal.stage == "settlement") {
        ok = co_await advanceSettlement(deal);
      }
      if (ok) ++processed;
    }
  } catch (const std::exception &e) {
    LOG_ERROR << "emulator: deal tick failed: " << e.what();
  }

  co_return processed;
}

}  // namespace core_svc
