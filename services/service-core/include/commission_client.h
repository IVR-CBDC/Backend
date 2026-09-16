#pragma once

// HTTP client for service-commission's authoritative commission quotes.
// Scenario confirmation (Task 4) never trusts a client-supplied commission
// figure — it re-prices server-side by calling this before persisting
// anything. No I/O-free unit tests here (needs a live Drogon HttpClient);
// exercised via the manual stand check in the task brief.

#include <drogon/utils/coroutine.h>
#include <json/json.h>
#include <string>
#include <variant>
#include <vector>

#include "domain.h"

namespace core_svc {

struct Quote {
  Scenario scenario;
  bool available = false;
  std::string unavailable_reason;
  double total = 0;
  Json::Value breakdown;
};

struct CommissionError {
  int status;
  std::string code;
  std::string message;
};

using QuotesResult = std::variant<std::vector<Quote>, CommissionError>;

// Calls POST /api/commission/quotes on service-commission, forwarding the
// caller's Authorization header as-is. `amount` is the canonical NUMERIC
// string (see DealRow::amount) — converted to a JSON number only at this
// HTTP boundary, same tradeoff as dealSummaryJson.
drogon::Task<QuotesResult> fetchQuotes(std::string authorization, std::string from_country,
                                       std::string to_country, std::string currency, std::string amount);

}  // namespace core_svc
