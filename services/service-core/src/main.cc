#include "core_controller.h"
#include "emulator.h"
#include "outbox_publisher.h"

#include <drogon/drogon.h>

int main() {
    drogon::app().loadConfigFile("/app/config.json");

    // Registered before app().run() so the outbox/emulator timers and the
    // manual-tick route only start once the DB/Redis clients app().run()
    // sets up are actually ready.
    drogon::app().registerBeginningAdvice([]() {
        core_svc::OutboxPublisher::start();

        if (core_svc::emulatorManual()) {
            // EMULATOR_MANUAL=true: no background loop, only the on-demand
            // route (used by deterministic test stands).
            drogon::app().registerHandler("/internal/emulator/tick", &core_svc::CoreController::tickEmulator,
                                          {drogon::Post});
        } else if (core_svc::emulatorEnabled()) {
            core_svc::Emulator::start(core_svc::emulatorConfigFromEnv());
        }
    });

    LOG_INFO << "service-core starting on :8080";
    drogon::app().run();
    return 0;
}
