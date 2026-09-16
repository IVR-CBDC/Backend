#include "commission_client.h"

#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <cstdlib>

using namespace drogon;

namespace core_svc {

namespace {

constexpr double kTimeoutSec = 5.0;

std::string commissionBaseUrl() {
  const char *env = std::getenv("COMMISSION_URL");
  return (env && *env) ? std::string(env) : "http://service-commission:8000";
}

// One persistent client for the process, same pattern the class docs
// recommend (a fresh HttpClient per call would reconnect every time). Safe
// to share across event-loop threads: HttpClient binds to the framework's
// own loop when constructed with loop=nullptr (the default here).
HttpClientPtr commissionClient() {
  static HttpClientPtr client = HttpClient::newHttpClient(commissionBaseUrl());
  return client;
}

Quote parseQuote(const Json::Value &q) {
  Quote quote;
  quote.scenario = scenarioFromString(q["scenario"].asString()).value_or(Scenario::cbdc);
  quote.available = q["available"].asBool();
  quote.unavailable_reason = q["unavailable_reason"].isNull() ? std::string() : q["unavailable_reason"].asString();
  if (!q["commission"].isNull()) {
    quote.breakdown = q["commission"];
    quote.total = q["commission"]["total"].asDouble();
  }
  return quote;
}

// Pulls {code, error} out of an upstream JSON error body, falling back to
// the given defaults when the body is missing or doesn't carry them (e.g. a
// proxy-generated error page instead of service-commission's own envelope).
CommissionError bodyError(int status, const HttpResponsePtr &resp, std::string fallbackCode,
                          std::string fallbackMessage) {
  if (auto body = resp->getJsonObject()) {
    if (body->isMember("code")) fallbackCode = (*body)["code"].asString();
    if (body->isMember("error")) fallbackMessage = (*body)["error"].asString();
  }
  return CommissionError{status, std::move(fallbackCode), std::move(fallbackMessage)};
}

CommissionError unavailable() {
  return CommissionError{503, "UPSTREAM_UNAVAILABLE", "Сервис расчёта комиссии недоступен"};
}

}  // namespace

Task<QuotesResult> fetchQuotes(std::string authorization, std::string from_country, std::string to_country,
                               std::string currency, std::string amount) {
  Json::Value body;
  body["from_country"] = from_country;
  body["to_country"] = to_country;
  body["currency"] = currency;
  // Canonical NUMERIC text -> JSON number only at this HTTP boundary; the
  // deal row itself keeps the string (see DealRow::amount comment).
  body["amount"] = std::stod(amount);

  auto req = HttpRequest::newHttpJsonRequest(body);
  req->setMethod(Post);
  req->setPath("/api/commission/quotes");
  req->addHeader("Authorization", authorization);

  HttpResponsePtr resp;
  try {
    resp = co_await commissionClient()->sendRequestCoro(req, kTimeoutSec);
  } catch (const HttpException &) {
    // Timeout or transport failure (connection refused, reset, ...) — no
    // retries here per the brief: the BFF/user sees 503 and retries.
    co_return unavailable();
  }
  if (!resp) co_return unavailable();

  const auto status = resp->getStatusCode();
  if (status == k200OK) {
    std::vector<Quote> quotes;
    if (auto json = resp->getJsonObject(); json && json->isMember("quotes")) {
      for (const auto &q : (*json)["quotes"]) quotes.push_back(parseQuote(q));
    }
    co_return quotes;
  }
  if (status == k401Unauthorized) co_return bodyError(401, resp, "INVALID_TOKEN", "Недействительный или истёкший токен");
  if (status == k400BadRequest || status == k404NotFound)
    co_return bodyError(400, resp, "VALIDATION_ERROR", "Некорректные параметры запроса");

  // Anything else (5xx, unexpected 4xx) — treat the upstream as unavailable
  // rather than guessing at a user-facing code for a shape we don't expect.
  co_return unavailable();
}

}  // namespace core_svc
