// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/MprisArtUrlSession.h"

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::platform::test
{
  namespace
  {
    struct PendingArt final
    {
      ResourceId resourceId = kInvalidResourceId;
      MprisArtUrlSession::OnUrlReady complete;
    };
  } // namespace

  TEST_CASE("MprisArtUrlSession - replacing a resource invalidates the cancelled request callback",
            "[gtk][regression][mpris][concurrency]")
  {
    constexpr auto kFirstResourceId = ResourceId{11};
    constexpr auto kSecondResourceId = ResourceId{22};
    auto pending = std::vector<PendingArt>{};
    std::int32_t cancellationCount = 0;
    std::int32_t changeCount = 0;
    auto session = MprisArtUrlSession{
      [&](ResourceId const resourceId, MprisArtUrlSession::OnUrlReady complete)
      {
        pending.push_back({.resourceId = resourceId, .complete = std::move(complete)});
        auto const pendingIndex = pending.size() - 1;
        return utility::ScopedRegistration{[&cancellationCount, &pending, pendingIndex]
                                           {
                                             ++cancellationCount;
                                             pending[pendingIndex].complete("file:///tmp/unregister-stale.png");
                                           }};
      },
      [&changeCount] { ++changeCount; }};

    session.refresh(kFirstResourceId);
    session.refresh(kSecondResourceId);

    REQUIRE(pending.size() == 2);
    CHECK(pending[0].resourceId == kFirstResourceId);
    CHECK(pending[1].resourceId == kSecondResourceId);
    CHECK(cancellationCount == 1);
    CHECK(changeCount == 0);
    CHECK(session.urlFor(kFirstResourceId).empty());
    CHECK(session.urlFor(kSecondResourceId).empty());

    pending[0].complete("file:///tmp/stale.png");
    CHECK(changeCount == 0);
    CHECK(session.urlFor(kSecondResourceId).empty());

    pending[1].complete("file:///tmp/current.png");
    CHECK(cancellationCount == 2);
    CHECK(changeCount == 1);
    CHECK(session.urlFor(kSecondResourceId) == "file:///tmp/current.png");

    pending[1].complete("file:///tmp/repeated.png");
    CHECK(changeCount == 1);
    CHECK(session.urlFor(kSecondResourceId) == "file:///tmp/current.png");
  }

  TEST_CASE("MprisArtUrlSession - synchronous completion publishes once without retaining the request",
            "[gtk][regression][mpris][concurrency]")
  {
    constexpr auto kResourceId = ResourceId{33};
    auto capturedCompletion = MprisArtUrlSession::OnUrlReady{};
    std::int32_t cancellationCount = 0;
    std::int32_t changeCount = 0;
    auto session =
      MprisArtUrlSession{[&](ResourceId const resourceId, MprisArtUrlSession::OnUrlReady complete)
                         {
                           CHECK(resourceId == kResourceId);
                           capturedCompletion = complete;
                           complete("file:///tmp/synchronous.png");
                           return utility::ScopedRegistration{[&]
                                                              {
                                                                ++cancellationCount;
                                                                capturedCompletion("file:///tmp/unregister-stale.png");
                                                              }};
                         },
                         [&changeCount] { ++changeCount; }};

    session.refresh(kResourceId);

    CHECK(cancellationCount == 1);
    CHECK(changeCount == 1);
    CHECK(session.urlFor(kResourceId) == "file:///tmp/synchronous.png");

    capturedCompletion("file:///tmp/repeated.png");
    CHECK(changeCount == 1);
    CHECK(session.urlFor(kResourceId) == "file:///tmp/synchronous.png");
  }

  TEST_CASE("MprisArtUrlSession - destruction invalidates completion before cancelling an active request",
            "[gtk][regression][mpris][concurrency]")
  {
    constexpr auto kResourceId = ResourceId{44};
    auto capturedCompletion = MprisArtUrlSession::OnUrlReady{};
    std::int32_t cancellationCount = 0;
    std::int32_t changeCount = 0;

    {
      auto session = MprisArtUrlSession{
        [&](ResourceId, MprisArtUrlSession::OnUrlReady complete)
        {
          capturedCompletion = std::move(complete);
          return utility::ScopedRegistration{[&]
                                             {
                                               ++cancellationCount;
                                               capturedCompletion("file:///tmp/unregister-stale.png");
                                             }};
        },
        [&changeCount] { ++changeCount; }};
      session.refresh(kResourceId);
    }

    CHECK(cancellationCount == 1);
    CHECK(changeCount == 0);

    capturedCompletion("file:///tmp/after-destruction.png");
    CHECK(changeCount == 0);
  }

  TEST_CASE("MprisArtUrlSession - requester exceptions invalidate the completion before rethrowing",
            "[gtk][regression][mpris][concurrency]")
  {
    constexpr auto kResourceId = ResourceId{55};
    auto capturedCompletion = MprisArtUrlSession::OnUrlReady{};
    std::int32_t changeCount = 0;
    auto session = MprisArtUrlSession{
      [&](ResourceId const resourceId, MprisArtUrlSession::OnUrlReady complete) -> utility::ScopedRegistration
      {
        CHECK(resourceId == kResourceId);
        capturedCompletion = std::move(complete);
        throwException<Exception>("request failed");
      },
      [&changeCount] { ++changeCount; }};

    REQUIRE_THROWS_AS(session.refresh(kResourceId), Exception);
    REQUIRE(capturedCompletion);
    CHECK(changeCount == 0);
    CHECK(session.urlFor(kResourceId).empty());

    capturedCompletion("file:///tmp/late.png");
    CHECK(changeCount == 0);
    CHECK(session.urlFor(kResourceId).empty());
  }
} // namespace ao::gtk::platform::test
