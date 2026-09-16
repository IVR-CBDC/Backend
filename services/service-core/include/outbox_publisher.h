#pragma once

// Drains the transactional outbox (Task 1's `outbox` table, written by
// DealRepository::create/apply — Task 4) into Redis pub/sub, so the BFF/UI
// can subscribe to `deal-events:<company_id>` instead of polling Postgres.

#include <drogon/drogon.h>

namespace core_svc {

class OutboxPublisher {
 public:
  static void start();

  // Publishes up to 100 unpublished rows and marks them published_at, in
  // one transaction (see outbox_publisher.cc for the at-least-once
  // reasoning). Returns how many rows were published.
  static drogon::Task<int> publishOnce();
};

}  // namespace core_svc
