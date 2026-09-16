#include "core_controller.h"
#include "emulator.h"

#include <common/helpers.h>
#include <drogon/drogon.h>

using namespace core_svc;
using namespace drogon;
using common::jsonError;

// Manual tick for deterministic test stands (EMULATOR_MANUAL=true, see
// main.cc — the background loop doesn't run in that mode, so tests advance
// deals by calling this instead of sleeping on a timer).
Task<> CoreController::tickEmulator(HttpRequestPtr, std::function<void(const HttpResponsePtr &)> cb) {
  try {
    const int processed = co_await Emulator::tickOnce();
    Json::Value out;
    out["processed"] = processed;
    cb(HttpResponse::newHttpJsonResponse(out));
  } catch (const std::exception &e) {
    LOG_ERROR << "tickEmulator error: " << e.what();
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  } catch (...) {
    LOG_ERROR << "tickEmulator error: unknown exception";
    cb(jsonError(k500InternalServerError, "INTERNAL_ERROR", "Внутренняя ошибка сервиса"));
  }
}
