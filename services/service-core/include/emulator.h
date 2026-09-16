#pragma once

// The deterministic emulator for the external systems a real deal would wait
// on (document reviewers, compliance/ФНС, the settlement rail). It advances
// deals the same way a user action would: by loading the current DealState,
// calling the pure state machine (Task 2), and writing the result back
// through DealRepository::apply (Task 4) — so every deal.updated /
// notification.created outbox row it produces is indistinguishable from one
// a real user action would have produced.
//
// "Deterministic" matters for demos and for tests: the same deal_id (+ kind)
// must always approve/reject/delay the same way, on any machine, on any run
// — hence fnv1a instead of std::hash (libstdc++ does not promise std::hash
// stability across process runs or platforms) and hence no PRNG/clock in the
// decision functions below.
//
// The pure functions (fnv1a, documentApproved, documentRejectReason,
// complianceDelayed, emulatorConfigFromEnv, emulatorEnabled, emulatorManual)
// touch only their arguments/getenv — never app() — so they link and run
// without a started Drogon framework (see tests/CMakeLists.txt, which links
// this file's .cc into core_tests alongside Catch2).

#include <drogon/drogon.h>
#include <string>
#include <string_view>

namespace core_svc {

// 64-bit FNV-1a over the bytes of `data`. Not cryptographic — just a cheap,
// stable way to turn an id into a reproducible pseudo-random decision.
unsigned long long fnv1a(std::string_view data);

// Delays the emulator waits before acting on a stage, in seconds.
// EMULATOR_SPEED=demo (default) keeps a demo watchable in under a minute;
// EMULATOR_SPEED=realistic approximates real turnaround times.
struct EmulatorConfig {
  long long doc_review_sec;
  long long compliance_sec;
  long long settlement_sec;
};

EmulatorConfig emulatorConfigFromEnv();

// Whether the background tick loop should run at all (EMULATOR_ENABLED,
// default true) and whether it's disabled in favor of the manual
// POST /internal/emulator/tick route (EMULATOR_MANUAL, default false) —
// used by main.cc to decide what to start/register.
bool emulatorEnabled();
bool emulatorManual();

// ~10% of documents are rejected, keyed by (deal_id, kind) so resubmitting
// the *same* document kind under the same deal always gets the same
// verdict within a single deal's lifetime scenario.
bool documentApproved(std::string_view deal_id, std::string_view kind);

// Only meaningful when documentApproved() is false for the same arguments;
// picks one of two plausible reasons, deterministically.
std::string documentRejectReason(std::string_view deal_id, std::string_view kind);

// ~20% of deals get one extra "Ожидаем ответ от ФНС" pause during
// compliance_check before being approved.
bool complianceDelayed(std::string_view deal_id);

// Ticks all deals/documents whose next_action_at is due. Runs on Drogon's
// event loop (start()) or on demand via POST /internal/emulator/tick
// (EMULATOR_MANUAL=true) — either way it must never throw: DB errors are
// logged and swallowed so a bad row can't kill the timer or the request.
class Emulator {
 public:
  static void start(const EmulatorConfig &cfg);

  // Processes one batch (<=50 documents + <=50 deals) and returns how many
  // rows were touched. Reads its own EmulatorConfig (set by start(), or the
  // process's environment if start() was never called — the manual-tick
  // route calls this directly without starting the background loop).
  static drogon::Task<int> tickOnce();
};

}  // namespace core_svc
