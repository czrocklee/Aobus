// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::async::test
{
  TEST_CASE("Signal - invokes move-only handlers in connection order", "[core][unit][signal]")
  {
    auto signal = Signal<std::int32_t>{};
    auto observed = std::vector<std::int32_t>{};

    CHECK_FALSE(signal.hasConnectedHandlers());

    auto firstSub = signal.connect([&](std::int32_t value) { observed.push_back(value); });
    auto secondSub = signal.connect([tokenPtr = std::make_unique<std::int32_t>(10), &observed](std::int32_t value)
                                    { observed.push_back(*tokenPtr + value); });

    CHECK(signal.hasConnectedHandlers());

    signal.emit(2);

    CHECK(observed == std::vector<std::int32_t>{2, 12});

    firstSub.reset();
    secondSub.reset();
    CHECK_FALSE(signal.hasConnectedHandlers());
  }

  TEST_CASE("Signal - connection and disconnection during emission affect later eligibility", "[core][unit][signal]")
  {
    auto signal = Signal<std::int32_t>{};
    auto observed = std::vector<std::int32_t>{};
    auto addedSubs = std::vector<Subscription>{};
    auto thirdSub = Subscription{};

    auto firstSub = signal.connect(
      [&](std::int32_t value)
      {
        observed.push_back(value);

        if (value == 1)
        {
          addedSubs.push_back(signal.connect([&](std::int32_t inner) { observed.push_back(100 + inner); }));
          thirdSub.reset();
        }
      });
    auto secondSub = signal.connect([&](std::int32_t value) { observed.push_back(10 + value); });
    thirdSub = signal.connect([&](std::int32_t value) { observed.push_back(20 + value); });

    signal.emit(1);
    CHECK(observed == std::vector<std::int32_t>{1, 11});

    signal.emit(2);
    CHECK(observed == std::vector<std::int32_t>{1, 11, 2, 12, 102});
  }

  TEST_CASE("Signal - nested emission defers disconnected handler cleanup until the outer emission returns",
            "[core][unit][signal]")
  {
    auto signal = Signal<std::int32_t>{};
    auto observed = std::vector<std::int32_t>{};
    auto tokenPtr = std::make_shared<std::int32_t>(7);
    auto weakTokenPtr = std::weak_ptr<std::int32_t>{tokenPtr};
    auto nestedSub = Subscription{};

    nestedSub = signal.connect(
      [&, tokenPtr = std::move(tokenPtr)](std::int32_t value)
      {
        observed.push_back(value);

        if (value == 1)
        {
          signal.emit(2);
          observed.push_back(*tokenPtr);
        }
        else
        {
          nestedSub.reset();
          observed.push_back(-value);
        }
      });
    auto otherSub = signal.connect([&](std::int32_t value) { observed.push_back(value * 10); });

    signal.emit(1);

    CHECK(observed == std::vector<std::int32_t>{1, 2, -2, 20, 7, 10});
    CHECK(weakTokenPtr.expired());

    signal.emit(3);
    CHECK(observed == std::vector<std::int32_t>{1, 2, -2, 20, 7, 10, 30});
  }

  TEST_CASE("Signal - disconnectAll prevents remaining and future delivery", "[core][unit][signal]")
  {
    auto signal = Signal<>{};
    std::int32_t callCount = 0;

    auto firstSub = signal.connect(
      [&]
      {
        ++callCount;
        signal.disconnectAll();
      });
    auto secondSub = signal.connect([&] { callCount += 10; });

    signal.emit();
    signal.emit();

    CHECK(callCount == 1);
    CHECK_FALSE(signal.hasConnectedHandlers());
  }

  TEST_CASE("Signal - subscription move assignment disconnects the replaced handler", "[core][unit][signal]")
  {
    auto signal = Signal<>{};
    auto observed = std::vector<std::int32_t>{};
    auto firstSub = signal.connect([&] { observed.push_back(1); });
    auto secondSub = signal.connect([&] { observed.push_back(2); });

    firstSub = std::move(secondSub);

    CHECK(firstSub);
    CHECK_FALSE(secondSub);

    signal.emit();

    CHECK(observed == std::vector<std::int32_t>{2});
  }

  TEST_CASE("Signal - rethrows the first observer exception after later observers run", "[core][unit][signal]")
  {
    auto signal = Signal<>{};
    auto observed = std::vector<std::int32_t>{};
    auto tokenPtr = std::make_shared<std::int32_t>(1);
    auto weakTokenPtr = std::weak_ptr<std::int32_t>{tokenPtr};
    auto firstSub = Subscription{};

    firstSub = signal.connect(
      [&, tokenPtr = std::move(tokenPtr)]
      {
        [[maybe_unused]] auto const& retainedTokenPtr = tokenPtr;
        observed.push_back(1);
        firstSub.reset();
        throwException<Exception>("first observer failure");
      });
    auto secondSub = signal.connect(
      [&]
      {
        observed.push_back(2);
        throwException<Exception>("second observer failure");
      });
    auto laterSub = signal.connect([&] { observed.push_back(3); });

    CHECK_THROWS_WITH(signal.emit(), "first observer failure");
    CHECK(observed == std::vector<std::int32_t>{1, 2, 3});
    CHECK(weakTokenPtr.expired());
  }

  TEST_CASE("Signal - destroying the owner during emission invalidates remaining subscriptions", "[core][unit][signal]")
  {
    auto signalPtr = std::make_unique<Signal<std::int32_t>>();
    auto observed = std::vector<std::int32_t>{};
    auto firstSub = Subscription{};
    auto secondSub = Subscription{};

    firstSub = signalPtr->connect(
      [&](std::int32_t value)
      {
        observed.push_back(value);
        firstSub.reset();
        signalPtr.reset();
      });
    secondSub = signalPtr->connect([&](std::int32_t value) { observed.push_back(100 + value); });

    auto* const signal = signalPtr.get();
    signal->emit(1);

    CHECK(signalPtr == nullptr);
    CHECK(observed == std::vector<std::int32_t>{1});

    secondSub.reset();
  }

  TEST_CASE("Signal - post owns decayed payload and defers reentrant posts to a later turn", "[core][unit][signal]")
  {
    auto executor = LoopExecutor{};
    auto signal = Signal<std::string const&>{};
    auto observed = std::vector<std::string>{};
    auto sub = signal.connect(
      [&](std::string const& value)
      {
        observed.push_back(value);

        if (value == "first")
        {
          signal.post(executor, std::string{"second"});
          observed.emplace_back("handler returned");
        }
      });
    auto payload = std::string{"first"};

    signal.post(executor, std::move(payload));
    payload = "changed";

    CHECK(observed.empty());
    REQUIRE(executor.runReadyTurn());
    CHECK(observed == std::vector<std::string>{"first", "handler returned"});
    REQUIRE(executor.runReadyTurn());
    CHECK(observed == std::vector<std::string>{"first", "handler returned", "second"});
    CHECK_FALSE(executor.runReadyTurn());
  }

  TEST_CASE("Signal - posted emission becomes a no-op after owner destruction", "[core][unit][signal]")
  {
    auto executor = LoopExecutor{};
    auto signalPtr = std::make_unique<Signal<std::int32_t>>();
    std::int32_t callCount = 0;
    auto sub = signalPtr->connect([&](std::int32_t) { ++callCount; });

    signalPtr->post(executor, 1);
    signalPtr.reset();

    REQUIRE(executor.runReadyTurn());
    CHECK(callCount == 0);

    sub.reset();
  }
} // namespace ao::async::test
