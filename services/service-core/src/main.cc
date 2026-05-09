#include <drogon/drogon.h>

int main() {
    drogon::app().loadConfigFile("/app/config.json");
    LOG_INFO << "service-core starting on :8080";
    drogon::app().run();
    return 0;
}
