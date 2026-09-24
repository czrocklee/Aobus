// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "windows-winui/app/DispatcherQueueAdmission.h"

#include <catch2/catch_test_macros.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <atomic>
#include <chrono>
#include <semaphore>
#include <thread>
#include <utility>

namespace ao::winui::test
{
  TEST_CASE("DispatcherQueueAdmission - closure preserves accepted submissions until the final drain",
            "[winui][unit][async][concurrency]")
  {
    constexpr auto kWaitTimeout = std::chrono::seconds{5};
    auto admission = detail::DispatcherQueueAdmission{};
    auto producerEntered = std::binary_semaphore{0};
    auto releaseProducer = std::binary_semaphore{0};
    auto drainFinished = std::binary_semaphore{0};
    auto drainStarted = std::binary_semaphore{0};
    auto producerAccepted = std::atomic_bool{false};
    bool drainAccepted = false;

    auto producer = std::jthread{};
    auto closer = std::jthread{};
    bool producerReleased = false;
    auto releaseProducerOnce = [&]
    {
      if (!std::exchange(producerReleased, true))
      {
        releaseProducer.release();
      }
    };
    auto releaseProducerOnExit = gsl_lite::finally(releaseProducerOnce);

    producer = std::jthread{[&]
                            {
                              auto optTicket = admission.tryAcquire(false);
                              producerAccepted.store(optTicket.has_value(), std::memory_order_release);
                              producerEntered.release();
                              releaseProducer.acquire();
                            }};

    REQUIRE(producerEntered.try_acquire_for(kWaitTimeout));
    REQUIRE(producerAccepted.load(std::memory_order_acquire));
    REQUIRE(admission.tryBeginClosing());
    CHECK(admission.state() == detail::DispatcherQueueAdmissionState::Closing);

    auto optClosingTicket = admission.tryAcquire(false);
    REQUIRE(optClosingTicket);
    optClosingTicket.reset();

    closer = std::jthread{[&]
                          {
                            drainStarted.release();
                            drainAccepted = admission.tryBeginDraining();
                            drainFinished.release();
                          }};

    REQUIRE(drainStarted.try_acquire_for(kWaitTimeout));
    auto const deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    while (admission.state() != detail::DispatcherQueueAdmissionState::Draining &&
           std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::yield();
    }

    REQUIRE(admission.state() == detail::DispatcherQueueAdmissionState::Draining);
    REQUIRE_FALSE(drainFinished.try_acquire());
    CHECK_FALSE(admission.tryAcquire(false));

    releaseProducerOnce();
    REQUIRE(drainFinished.try_acquire_for(kWaitTimeout));
    producer.join();
    closer.join();

    REQUIRE(drainAccepted);
    auto optOwnerDrainTicket = admission.tryAcquire(true);
    REQUIRE(optOwnerDrainTicket);
    optOwnerDrainTicket.reset();
    REQUIRE(admission.tryFinishClosing());
    CHECK(admission.state() == detail::DispatcherQueueAdmissionState::Closed);
    CHECK_FALSE(admission.tryAcquire(true));
  }

  TEST_CASE("DispatcherQueueAdmission - only live wake rejection is fatal", "[winui][unit][async]")
  {
    using detail::DispatcherQueueAdmissionState;
    using detail::DispatcherQueueWakeRejectionDisposition;

    CHECK(detail::wakeRejectionDisposition(DispatcherQueueAdmissionState::Running) ==
          DispatcherQueueWakeRejectionDisposition::Fatal);
    CHECK(detail::wakeRejectionDisposition(DispatcherQueueAdmissionState::Closing) ==
          DispatcherQueueWakeRejectionDisposition::ExpectedDuringClosure);
    CHECK(detail::wakeRejectionDisposition(DispatcherQueueAdmissionState::Draining) ==
          DispatcherQueueWakeRejectionDisposition::ExpectedDuringClosure);
    CHECK(detail::wakeRejectionDisposition(DispatcherQueueAdmissionState::Closed) ==
          DispatcherQueueWakeRejectionDisposition::ExpectedDuringClosure);

    CHECK(detail::isTaskAdmissionOpen(DispatcherQueueAdmissionState::Running, false));
    CHECK(detail::isTaskAdmissionOpen(DispatcherQueueAdmissionState::Closing, false));
    CHECK_FALSE(detail::isTaskAdmissionOpen(DispatcherQueueAdmissionState::Draining, false));
    CHECK(detail::isTaskAdmissionOpen(DispatcherQueueAdmissionState::Draining, true));
    CHECK_FALSE(detail::isTaskAdmissionOpen(DispatcherQueueAdmissionState::Closed, true));
  }

  TEST_CASE("DispatcherQueueAdmission - destruction fallback refuses an active owner callback", "[winui][unit][async]")
  {
    auto admission = detail::DispatcherQueueAdmission{};
    auto optOwnerTicket = admission.tryAcquire(true);
    REQUIRE(optOwnerTicket);

    CHECK_FALSE(admission.tryCloseForDestruction());
    CHECK(admission.state() == detail::DispatcherQueueAdmissionState::Running);

    optOwnerTicket.reset();
    REQUIRE(admission.tryCloseForDestruction());
    CHECK(admission.state() == detail::DispatcherQueueAdmissionState::Closed);
    CHECK(admission.tryCloseForDestruction());
  }
} // namespace ao::winui::test
