// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/async/RequestCoalescer.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ao::async::test
{
  TEST_CASE("RequestCoalescer - equal keys share one ordered flight", "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::string>;

    auto coalescer = Coalescer{};
    auto optFirstToken = std::optional<Coalescer::FlightToken>{};
    auto optOtherToken = std::optional<Coalescer::FlightToken>{};
    std::int32_t starts = 0;
    auto observed = std::vector<std::string>{};
    auto first = coalescer.request(
      1,
      [&](std::string const& value) { observed.push_back("first:" + value); },
      [&](Coalescer::FlightToken token)
      {
        ++starts;
        optFirstToken = std::move(token);
      });
    auto second = coalescer.request(
      1,
      [&](std::string const& value) { observed.push_back("second:" + value); },
      [&](Coalescer::FlightToken) { ++starts; });
    auto other = coalescer.request(
      2,
      [&](std::string const& value) { observed.push_back("other:" + value); },
      [&](Coalescer::FlightToken token)
      {
        ++starts;
        optOtherToken = std::move(token);
      });

    REQUIRE(optFirstToken);
    REQUIRE(optOtherToken);
    CHECK(starts == 2);

    coalescer.complete(*optFirstToken, "ready");
    coalescer.complete(*optOtherToken, "done");

    CHECK(observed == std::vector<std::string>{"first:ready", "second:ready", "other:done"});
  }

  TEST_CASE("RequestCoalescer - prefetch and empty callbacks reserve work once",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    std::int32_t starts = 0;

    coalescer.prefetch(1, [&](Coalescer::FlightToken) { ++starts; });
    auto emptyRequest = coalescer.request(1, {}, [&](Coalescer::FlightToken) { ++starts; });

    CHECK(starts == 1);
    CHECK_FALSE(emptyRequest);
  }

  TEST_CASE("RequestCoalescer - cancellation affects only its own interest",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optToken = std::optional<Coalescer::FlightToken>{};
    auto firstCalled = std::atomic_bool{false};
    bool secondCalled = false;
    auto first = coalescer.request(
      1, [&](std::int32_t) { firstCalled = true; }, [&](Coalescer::FlightToken value) { optToken = std::move(value); });
    auto second = coalescer.request(1, [&](std::int32_t) { secondCalled = true; }, [](Coalescer::FlightToken) {});
    auto cancellationThread = std::jthread{[request = std::move(first)] mutable { request.reset(); }};
    cancellationThread.join();

    REQUIRE(optToken);
    coalescer.complete(*optToken, 3);

    CHECK_FALSE(firstCalled.load());
    CHECK(secondCalled);
  }

  TEST_CASE("RequestCoalescer - completion retires a flight after every waiter cancels",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optFirstToken = std::optional<Coalescer::FlightToken>{};
    auto optReplacementToken = std::optional<Coalescer::FlightToken>{};
    std::int32_t starts = 0;
    std::int32_t calls = 0;
    auto first = coalescer.request(
      1,
      [&](std::int32_t) { ++calls; },
      [&](Coalescer::FlightToken token)
      {
        ++starts;
        optFirstToken = std::move(token);
      });
    auto second = coalescer.request(1, [&](std::int32_t) { ++calls; }, [&](Coalescer::FlightToken) { ++starts; });

    first.reset();
    second.reset();
    REQUIRE(optFirstToken);
    coalescer.complete(*optFirstToken, 3);

    CHECK(starts == 1);
    CHECK(calls == 0);

    auto replacement = coalescer.request(
      1,
      [&](std::int32_t) { ++calls; },
      [&](Coalescer::FlightToken token)
      {
        ++starts;
        optReplacementToken = std::move(token);
      });
    REQUIRE(optReplacementToken);
    coalescer.complete(*optReplacementToken, 4);

    CHECK(starts == 2);
    CHECK(calls == 1);
  }

  TEST_CASE("RequestCoalescer - request handles may outlive the owner", "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto request = Coalescer::Request{};

    {
      auto coalescer = Coalescer{};
      request = coalescer.request(1, [](std::int32_t) {}, [](Coalescer::FlightToken) {});
    }

    CHECK_NOTHROW(request.reset());
  }

  TEST_CASE("RequestCoalescer - completion permits a reentrant same-key flight",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optFirstToken = std::optional<Coalescer::FlightToken>{};
    auto optNestedToken = std::optional<Coalescer::FlightToken>{};
    std::int32_t starts = 0;
    auto observed = std::vector<std::int32_t>{};
    auto nested = Coalescer::Request{};
    auto first = coalescer.request(
      1,
      [&](std::int32_t value)
      {
        observed.push_back(value);
        nested = coalescer.request(
          1,
          [&](std::int32_t nestedValue) { observed.push_back(nestedValue); },
          [&](Coalescer::FlightToken token)
          {
            ++starts;
            optNestedToken = std::move(token);
          });
      },
      [&](Coalescer::FlightToken token)
      {
        ++starts;
        optFirstToken = std::move(token);
      });

    REQUIRE(optFirstToken);
    coalescer.complete(*optFirstToken, 4);
    REQUIRE(optNestedToken);
    coalescer.complete(*optNestedToken, 5);

    CHECK(starts == 2);
    CHECK(observed == std::vector<std::int32_t>{4, 5});
  }

  TEST_CASE("RequestCoalescer - an earlier callback may cancel a later callback",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optToken = std::optional<Coalescer::FlightToken>{};
    auto later = Coalescer::Request{};
    bool laterCalled = false;
    auto earlier = coalescer.request(
      1, [&](std::int32_t) { later.reset(); }, [&](Coalescer::FlightToken value) { optToken = std::move(value); });
    later = coalescer.request(1, [&](std::int32_t) { laterCalled = true; }, [](Coalescer::FlightToken) {});

    REQUIRE(optToken);
    coalescer.complete(*optToken, 0);

    CHECK_FALSE(laterCalled);
  }

  TEST_CASE("RequestCoalescer - callback exceptions do not stop fanout", "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optToken = std::optional<Coalescer::FlightToken>{};
    bool laterCalled = false;
    auto throwing = coalescer.request(
      1,
      [](std::int32_t) { throw std::runtime_error{"callback"}; },
      [&](Coalescer::FlightToken value) { optToken = std::move(value); });
    auto later = coalescer.request(1, [&](std::int32_t) { laterCalled = true; }, [](Coalescer::FlightToken) {});

    REQUIRE(optToken);
    CHECK_THROWS_AS(coalescer.complete(*optToken, 0), std::runtime_error);
    CHECK(laterCalled);
  }

  TEST_CASE("RequestCoalescer - starter failure rolls back only the matching flight",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    std::int32_t starts = 0;

    CHECK_THROWS_AS(coalescer.request(
                      1,
                      [](std::int32_t) {},
                      [&](Coalescer::FlightToken)
                      {
                        ++starts;
                        throw std::runtime_error{"start"};
                      }),
                    std::runtime_error);

    auto retry = coalescer.request(1, [](std::int32_t) {}, [&](Coalescer::FlightToken) { ++starts; });

    CHECK(starts == 2);
  }

  TEST_CASE("RequestCoalescer - duplicate and foreign completions are ignored",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto foreignCoalescer = Coalescer{};
    auto optToken = std::optional<Coalescer::FlightToken>{};
    auto optForeignToken = std::optional<Coalescer::FlightToken>{};
    std::int32_t calls = 0;
    auto request = coalescer.request(
      1, [&](std::int32_t) { ++calls; }, [&](Coalescer::FlightToken value) { optToken = std::move(value); });
    auto foreignRequest = foreignCoalescer.request(
      1, [](std::int32_t) {}, [&](Coalescer::FlightToken value) { optForeignToken = std::move(value); });

    REQUIRE(optToken);
    REQUIRE(optForeignToken);
    coalescer.complete(*optForeignToken, 0);
    coalescer.complete(*optToken, 0);
    coalescer.complete(*optToken, 0);

    CHECK(calls == 1);
  }

  TEST_CASE("RequestCoalescer - clear fences late completion from a replacement flight",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optOldToken = std::optional<Coalescer::FlightToken>{};
    auto optReplacementToken = std::optional<Coalescer::FlightToken>{};
    auto observed = std::vector<std::int32_t>{};
    auto oldRequest = coalescer.request(
      1,
      [&](std::int32_t value) { observed.push_back(value); },
      [&](Coalescer::FlightToken token) { optOldToken = std::move(token); });

    REQUIRE(optOldToken);
    coalescer.clear();

    auto replacementRequest = coalescer.request(
      1,
      [&](std::int32_t value) { observed.push_back(value); },
      [&](Coalescer::FlightToken token) { optReplacementToken = std::move(token); });

    REQUIRE(optReplacementToken);
    coalescer.complete(*optOldToken, 1);
    CHECK(observed.empty());

    coalescer.complete(*optReplacementToken, 2);
    CHECK(observed == std::vector<std::int32_t>{2});
  }

  TEST_CASE("RequestCoalescer - exact-flight dependencies are retained until retirement",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optToken = std::optional<Coalescer::FlightToken>{};
    std::int32_t releases = 0;
    bool releasedBeforeCallback = false;
    auto request = coalescer.request(
      1,
      [&](std::int32_t) { releasedBeforeCallback = releases == 1; },
      [&](Coalescer::FlightToken token) { optToken = std::move(token); });

    REQUIRE(optToken);
    CHECK(coalescer.retainDependency(*optToken, utility::ScopedRegistration{[&] { ++releases; }}));
    CHECK(releases == 0);

    CHECK(releases == 0);
    coalescer.complete(*optToken, 4);

    CHECK(releases == 1);
    CHECK(releasedBeforeCallback);
  }

  TEST_CASE("RequestCoalescer - clear and stale tokens release dependencies immediately",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optToken = std::optional<Coalescer::FlightToken>{};
    std::int32_t releases = 0;
    coalescer.prefetch(1, [&](Coalescer::FlightToken token) { optToken = std::move(token); });

    REQUIRE(optToken);
    CHECK(coalescer.retainDependency(*optToken, utility::ScopedRegistration{[&] { ++releases; }}));
    coalescer.clear();
    CHECK(releases == 1);

    CHECK_FALSE(coalescer.retainDependency(*optToken, utility::ScopedRegistration{[&] { ++releases; }}));
    CHECK(releases == 2);
  }

  TEST_CASE("RequestCoalescer - retired tokens cannot attach to a reentrant replacement",
            "[core][unit][request-coalescer][concurrency]")
  {
    using Coalescer = RequestCoalescer<std::int32_t, std::int32_t>;

    auto coalescer = Coalescer{};
    auto optFirstToken = std::optional<Coalescer::FlightToken>{};
    auto optReplacementToken = std::optional<Coalescer::FlightToken>{};
    std::int32_t staleReleases = 0;
    auto replacement = Coalescer::Request{};
    auto first = coalescer.request(
      1,
      [&](std::int32_t)
      {
        replacement = coalescer.request(
          1, [](std::int32_t) {}, [&](Coalescer::FlightToken token) { optReplacementToken = std::move(token); });
        CHECK_FALSE(coalescer.retainDependency(*optFirstToken, utility::ScopedRegistration{[&] { ++staleReleases; }}));
      },
      [&](Coalescer::FlightToken token) { optFirstToken = std::move(token); });

    REQUIRE(optFirstToken);
    coalescer.complete(*optFirstToken, 1);

    REQUIRE(optReplacementToken);
    CHECK(staleReleases == 1);
  }
} // namespace ao::async::test
