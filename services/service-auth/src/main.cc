#include <drogon/drogon.h>

int main() {
  using namespace drogon;

  app().loadConfigFile("/app/config.json");

  LOG_INFO << "service-auth starting on :8080";
  app().run();
  return 0;
}
