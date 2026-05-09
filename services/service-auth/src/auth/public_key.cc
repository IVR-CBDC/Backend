#include "auth_controller.h"

#include <cstdlib>
#include <drogon/drogon.h>
#include <fstream>
#include <sstream>

using namespace auth_svc;
using namespace drogon;

void AuthController::publicKey(
    const HttpRequestPtr &, std::function<void(const HttpResponsePtr &)> &&cb) {

  static const std::string pem = []() {
    const char *path = std::getenv("JWT_PUBLIC_KEY_PATH");
    if (!path)
      return std::string{};
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }();

  auto resp = HttpResponse::newHttpResponse();
  resp->setContentTypeString("application/x-pem-file");
  resp->setBody(pem);
  cb(resp);
}
