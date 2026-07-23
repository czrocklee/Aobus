// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/RequestCoalescer.h"

#include <ao/Exception.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("RequestCoalescer - equal keys share one ordered flight", "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::string>{};
    std::int32_t starts = 0;
    auto observed = std::vector<std::string>{};
    auto first =
      coalescer.request(1, [&](std::string const& value) { observed.push_back("first:" + value); }, [&] { ++starts; });
    auto second =
      coalescer.request(1, [&](std::string const& value) { observed.push_back("second:" + value); }, [&] { ++starts; });
    auto other =
      coalescer.request(2, [&](std::string const& value) { observed.push_back("other:" + value); }, [&] { ++starts; });

    CHECK(starts == 2);

    coalescer.complete(1, "ready");
    coalescer.complete(2, "done");

    CHECK(observed == std::vector<std::string>{"first:ready", "second:ready", "other:done"});
  }

  TEST_CASE("RequestCoalescer - prefetch and empty callbacks reserve work once",
            "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
    std::int32_t starts = 0;

    coalescer.prefetch(1, [&] { ++starts; });
    auto emptyRequest = coalescer.request(1, {}, [&] { ++starts; });

    CHECK(starts == 1);
    CHECK_FALSE(emptyRequest);
  }

  TEST_CASE("RequestCoalescer - cancellation affects only its own interest",
            "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
    auto firstCalled = std::atomic_bool{false};
    bool secondCalled = false;
    auto first = coalescer.request(1, [&](std::int32_t) { firstCalled = true; }, [] {});
    auto second = coalescer.request(1, [&](std::int32_t) { secondCalled = true; }, [] {});
    auto cancellationThread = std::jthread{[request = std::move(first)] mutable { request.reset(); }};
    cancellationThread.join();

    coalescer.complete(1, 3);

    CHECK_FALSE(firstCalled.load());
    CHECK(secondCalled);
  }

  TEST_CASE("RequestCoalescer - completion retires a flight after every waiter cancels",
            "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
    std::int32_t starts = 0;
    std::int32_t calls = 0;
    auto first = coalescer.request(1, [&](std::int32_t) { ++calls; }, [&] { ++starts; });
    auto second = coalescer.request(1, [&](std::int32_t) { ++calls; }, [&] { ++starts; });

    first.reset();
    second.reset();
    coalescer.complete(1, 3);

    CHECK(starts == 1);
    CHECK(calls == 0);

    auto replacement = coalescer.request(1, [&](std::int32_t) { ++calls; }, [&] { ++starts; });
    coalescer.complete(1, 4);

    CHECK(starts == 2);
    CHECK(calls == 1);
  }

  TEST_CASE("RequestCoalescer - request handles may outlive the owner", "[gtk][unit][request-coalescer][concurrency]")
  {
    auto request = RequestCoalescer<std::int32_t, std::int32_t>::Request{};

    {
      auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
      request = coalescer.request(1, [](std::int32_t) {}, [] {});
    }

    CHECK_NOTHROW(request.reset());
  }

  TEST_CASE("RequestCoalescer - completion permits a reentrant same-key flight",
            "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
    std::int32_t starts = 0;
    auto observed = std::vector<std::int32_t>{};
    auto nested = RequestCoalescer<std::int32_t, std::int32_t>::Request{};
    auto first = coalescer.request(
      1,
      [&](std::int32_t value)
      {
        observed.push_back(value);
        nested =
          coalescer.request(1, [&](std::int32_t nestedValue) { observed.push_back(nestedValue); }, [&] { ++starts; });
      },
      [&] { ++starts; });

    coalescer.complete(1, 4);
    coalescer.complete(1, 5);

    CHECK(starts == 2);
    CHECK(observed == std::vector<std::int32_t>{4, 5});
  }

  TEST_CASE("RequestCoalescer - an earlier callback may cancel a later callback",
            "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
    auto later = RequestCoalescer<std::int32_t, std::int32_t>::Request{};
    bool laterCalled = false;
    auto earlier = coalescer.request(1, [&](std::int32_t) { later.reset(); }, [] {});
    later = coalescer.request(1, [&](std::int32_t) { laterCalled = true; }, [] {});

    coalescer.complete(1, 0);

    CHECK_FALSE(laterCalled);
  }

  TEST_CASE("RequestCoalescer - callback exceptions do not stop fanout", "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
    bool laterCalled = false;
    auto throwing = coalescer.request(1, [](std::int32_t) { throwException<Exception>("callback"); }, [] {});
    auto later = coalescer.request(1, [&](std::int32_t) { laterCalled = true; }, [] {});

    CHECK_THROWS_AS(coalescer.complete(1, 0), Exception);
    CHECK(laterCalled);
  }

  TEST_CASE("RequestCoalescer - starter failure rolls back only the matching flight",
            "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
    std::int32_t starts = 0;

    CHECK_THROWS_AS(coalescer.request(
                      1,
                      [](std::int32_t) {},
                      [&]
                      {
                        ++starts;
                        throwException<Exception>("start");
                      }),
                    Exception);

    auto retry = coalescer.request(1, [](std::int32_t) {}, [&] { ++starts; });

    CHECK(starts == 2);
  }

  TEST_CASE("RequestCoalescer - duplicate and unknown completions are ignored",
            "[gtk][unit][request-coalescer][concurrency]")
  {
    auto coalescer = RequestCoalescer<std::int32_t, std::int32_t>{};
    std::int32_t calls = 0;
    auto request = coalescer.request(1, [&](std::int32_t) { ++calls; }, [] {});

    coalescer.complete(1, 0);
    coalescer.complete(1, 0);
    coalescer.complete(2, 0);

    CHECK(calls == 1);
  }
} // namespace ao::gtk::test
