// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "windows-winui/app/DispatcherQueueAdmission.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <semaphore>
#include <thread>

namespace ao::winui::test
{
  TEST_CASE("DispatcherQueueAdmission - closure preserves accepted submissions until the final drain",
            "[winui][unit][async][concurrency]")
  {
    auto admission = detail::DispatcherQueueAdmission{};
    auto producerEntered = std::binary_semaphore{0};
    auto releaseProducer = std::binary_semaphore{0};
    auto producerAccepted = std::atomic_bool{false};

    auto producer = std::jthread{[&]
                                 {
                                   auto optTicket = admission.tryAcquire(false);
                                   producerAccepted.store(optTicket.has_value(), std::memory_order_release);
                                   producerEntered.release();
                                   releaseProducer.acquire();
                                 }};

    producerEntered.acquire();
    REQUIRE(producerAccepted.load(std::memory_order_acquire));
    REQUIRE(admission.beginClosing());
    CHECK(admission.state() == detail::DispatcherQueueAdmissionState::Closing);

    auto optClosingTicket = admission.tryAcquire(false);
    REQUIRE(optClosingTicket);
    optClosingTicket.reset();

    auto drainFinished = std::binary_semaphore{0};
    auto drainStarted = std::binary_semaphore{0};
    bool drainAccepted = false;
    auto closer = std::jthread{[&]
                               {
                                 drainStarted.release();
                                 drainAccepted = admission.beginDraining();
                                 drainFinished.release();
                               }};

    drainStarted.acquire();

    while (admission.state() != detail::DispatcherQueueAdmissionState::Draining)
    {
      std::this_thread::yield();
    }

    CHECK_FALSE(drainFinished.try_acquire());
    CHECK_FALSE(admission.tryAcquire(false));

    releaseProducer.release();
    drainFinished.acquire();
    producer.join();
    closer.join();

    REQUIRE(drainAccepted);
    auto optOwnerDrainTicket = admission.tryAcquire(true);
    REQUIRE(optOwnerDrainTicket);
    optOwnerDrainTicket.reset();
    REQUIRE(admission.finishClosing());
    CHECK(admission.state() == detail::DispatcherQueueAdmissionState::Closed);
    CHECK_FALSE(admission.tryAcquire(true));
  }

  TEST_CASE("DispatcherQueueAdmission - only live wake rejection is fatal", "[winui][unit][async][regression]")
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

  TEST_CASE("DispatcherQueueAdmission - destruction fallback refuses an active owner callback",
            "[winui][unit][async][regression]")
  {
    auto admission = detail::DispatcherQueueAdmission{};
    auto optOwnerTicket = admission.tryAcquire(true);
    REQUIRE(optOwnerTicket);

    CHECK_FALSE(admission.closeForDestruction());
    CHECK(admission.state() == detail::DispatcherQueueAdmissionState::Running);

    optOwnerTicket.reset();
    REQUIRE(admission.closeForDestruction());
    CHECK(admission.state() == detail::DispatcherQueueAdmissionState::Closed);
    CHECK(admission.closeForDestruction());
  }
} // namespace ao::winui::test
