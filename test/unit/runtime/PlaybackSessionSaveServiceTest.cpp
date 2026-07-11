// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/PlaybackSessionSaveService.h>
#include <ao/rt/Subscription.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class ControlledSaveScheduler final : public PlaybackSessionSaveService::Scheduler
    {
    public:
      Subscription schedule(PlaybackSessionSaveService::Delay const delay,
                            PlaybackSessionSaveService::Callback callback) override
      {
        auto const id = _nextId++;
        _entries.push_back(Entry{.id = id, .delay = delay, .callback = std::move(callback), .active = true});
        return Subscription{[this, id]
                            {
                              if (auto const it = entry(id); it != _entries.end())
                              {
                                it->active = false;
                              }
                            }};
      }

      bool fireNext()
      {
        auto const it = std::ranges::find(_entries, true, &Entry::active);

        if (it == _entries.end())
        {
          return false;
        }

        return fire(*it, false);
      }

      bool forceFire(std::uint64_t const id)
      {
        auto const it = entry(id);
        return it != _entries.end() && fire(*it, true);
      }

      std::uint64_t lastScheduledId() const { return _entries.empty() ? 0 : _entries.back().id; }

      std::vector<PlaybackSessionSaveService::Delay> pendingDelays() const
      {
        auto delays = std::vector<PlaybackSessionSaveService::Delay>{};

        for (auto const& candidate : _entries)
        {
          if (candidate.active)
          {
            delays.push_back(candidate.delay);
          }
        }

        return delays;
      }

    private:
      struct Entry final
      {
        std::uint64_t id = 0;
        PlaybackSessionSaveService::Delay delay{};
        PlaybackSessionSaveService::Callback callback;
        bool active = false;
      };

      std::vector<Entry>::iterator entry(std::uint64_t const id) { return std::ranges::find(_entries, id, &Entry::id); }

      bool fire(Entry& entryValue, bool const ignoreCancellation)
      {
        if ((!entryValue.active && !ignoreCancellation) || !entryValue.callback)
        {
          return false;
        }

        entryValue.active = false;
        auto callback = std::move(entryValue.callback);
        callback();
        return true;
      }

      std::vector<Entry> _entries;
      std::uint64_t _nextId = 1;
    };

    class SavePort final
    {
    public:
      struct Outcome final
      {
        bool succeeds = true;
        bool remainsDirty = false;
      };

      PlaybackSessionSaveService::Port port()
      {
        return PlaybackSessionSaveService::Port{
          .subscribeDirty =
            [this](PlaybackSessionSaveService::Callback handler)
          {
            auto const subscriptionId = ++_subscriptionId;
            ++_subscribeCount;
            _dirtyHandler = std::move(handler);

            if (_dirty)
            {
              ++_dirtyReplayCount;
              _dirtyHandler();
            }

            return Subscription{[this, subscriptionId]
                                {
                                  if (_subscriptionId == subscriptionId)
                                  {
                                    _dirtyHandler = {};
                                  }
                                }};
          },
          .save = [this] -> Result<>
          {
            ++_saveCount;
            auto outcome = Outcome{};

            if (!_outcomes.empty())
            {
              outcome = _outcomes.front();
              _outcomes.pop_front();
            }

            if (!outcome.succeeds)
            {
              return std::unexpected{
                Error{.code = Error::Code::IoError, .message = "scripted playback-session save failure"}};
            }

            _dirty = outcome.remainsDirty;
            return {};
          },
        };
      }

      void markDirty()
      {
        if (_dirty)
        {
          return;
        }

        _dirty = true;

        if (_dirtyHandler)
        {
          ++_dirtyTransitionCount;
          _dirtyHandler();
        }
      }

      void queueFailure() { _outcomes.push_back(Outcome{.succeeds = false}); }
      void queueSuccess(bool const remainsDirty = false)
      {
        _outcomes.push_back(Outcome{.succeeds = true, .remainsDirty = remainsDirty});
      }

      bool hasSubscriber() const { return static_cast<bool>(_dirtyHandler); }
      bool isDirty() const { return _dirty; }
      std::size_t saveCount() const { return _saveCount; }
      std::size_t subscribeCount() const { return _subscribeCount; }
      std::size_t dirtyReplayCount() const { return _dirtyReplayCount; }
      std::size_t dirtyTransitionCount() const { return _dirtyTransitionCount; }

    private:
      std::deque<Outcome> _outcomes;
      PlaybackSessionSaveService::Callback _dirtyHandler;
      std::uint64_t _subscriptionId = 0;
      std::size_t _saveCount = 0;
      std::size_t _subscribeCount = 0;
      std::size_t _dirtyReplayCount = 0;
      std::size_t _dirtyTransitionCount = 0;
      bool _dirty = false;
    };

    struct SaveServiceFixture final
    {
      SaveServiceFixture()
        : servicePtr{std::make_unique<PlaybackSessionSaveService>(savePort.port(), scheduler, timing)}
      {
      }

      SavePort savePort;
      ControlledSaveScheduler scheduler;
      PlaybackSessionSaveService::Timing timing{
        .dirtyDebounceDelay = std::chrono::milliseconds{5},
        .initialRetryDelay = std::chrono::milliseconds{10},
        .maximumRetryDelay = std::chrono::milliseconds{40},
      };
      std::unique_ptr<PlaybackSessionSaveService> servicePtr;
    };
  } // namespace

  TEST_CASE("PlaybackSessionSaveService - late dirty replay schedules one debounced save",
            "[runtime][unit][playback-session]")
  {
    auto fixture = SaveServiceFixture{};
    fixture.savePort.markDirty();

    fixture.servicePtr->start();
    fixture.servicePtr->start();

    CHECK(fixture.savePort.subscribeCount() == 1);
    CHECK(fixture.savePort.dirtyReplayCount() == 1);
    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{5}});
    CHECK(fixture.savePort.saveCount() == 0);

    // Runtime publishes only clean-to-dirty, so a second mutation while dirty is silent.
    fixture.savePort.markDirty();
    CHECK(fixture.savePort.dirtyTransitionCount() == 0);
    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{5}});

    REQUIRE(fixture.scheduler.fireNext());
    CHECK(fixture.savePort.saveCount() == 1);
    CHECK_FALSE(fixture.savePort.isDirty());
    CHECK(fixture.savePort.subscribeCount() == 2);
    CHECK(fixture.scheduler.pendingDelays().empty());
  }

  TEST_CASE("PlaybackSessionSaveService - successful exact-revision save replays a newer dirty revision",
            "[runtime][unit][playback-session]")
  {
    auto fixture = SaveServiceFixture{};
    fixture.savePort.queueSuccess(true);
    fixture.savePort.queueSuccess(false);
    fixture.savePort.markDirty();
    fixture.servicePtr->start();

    REQUIRE(fixture.scheduler.fireNext());

    CHECK(fixture.savePort.saveCount() == 1);
    CHECK(fixture.savePort.isDirty());
    CHECK(fixture.savePort.dirtyReplayCount() == 2);
    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{5}});

    REQUIRE(fixture.scheduler.fireNext());
    CHECK(fixture.savePort.saveCount() == 2);
    CHECK_FALSE(fixture.savePort.isDirty());
    CHECK(fixture.scheduler.pendingDelays().empty());
  }

  TEST_CASE("PlaybackSessionSaveService - failures retry without another dirty event and cap backoff",
            "[runtime][unit][playback-session]")
  {
    auto fixture = SaveServiceFixture{};
    fixture.savePort.queueFailure();
    fixture.savePort.queueFailure();
    fixture.savePort.queueFailure();
    fixture.savePort.queueFailure();
    fixture.savePort.queueSuccess();
    fixture.savePort.markDirty();
    fixture.servicePtr->start();

    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{5}});
    REQUIRE(fixture.scheduler.fireNext());
    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{10}});
    REQUIRE(fixture.scheduler.fireNext());
    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{20}});
    REQUIRE(fixture.scheduler.fireNext());
    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{40}});
    REQUIRE(fixture.scheduler.fireNext());
    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{40}});
    REQUIRE(fixture.scheduler.fireNext());

    CHECK(fixture.savePort.saveCount() == 5);
    CHECK_FALSE(fixture.savePort.isDirty());
    CHECK(fixture.scheduler.pendingDelays().empty());
    CHECK(fixture.savePort.dirtyTransitionCount() == 0);

    // Success resets the exponential sequence for the next dirty lifecycle.
    fixture.savePort.queueFailure();
    fixture.savePort.markDirty();
    CHECK(fixture.savePort.dirtyTransitionCount() == 1);
    REQUIRE(fixture.scheduler.fireNext());
    CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{10}});
  }

  TEST_CASE("PlaybackSessionSaveService - frontend safety triggers share cancellation and retry policy",
            "[runtime][unit][playback-session]")
  {
    SECTION("significant event")
    {
      auto fixture = SaveServiceFixture{};
      fixture.savePort.queueFailure();
      fixture.savePort.queueSuccess();
      fixture.savePort.markDirty();
      fixture.servicePtr->start();
      auto const cancelledDebounceId = fixture.scheduler.lastScheduledId();

      fixture.servicePtr->saveSignificantEvent();

      CHECK(fixture.savePort.saveCount() == 1);
      CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{10}});
      REQUIRE(fixture.scheduler.forceFire(cancelledDebounceId));
      CHECK(fixture.savePort.saveCount() == 1);
      CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{10}});

      REQUIRE(fixture.scheduler.fireNext());
      CHECK(fixture.savePort.saveCount() == 2);
      CHECK(fixture.scheduler.pendingDelays().empty());
    }

    SECTION("periodic safety net")
    {
      auto fixture = SaveServiceFixture{};
      fixture.savePort.queueFailure();
      fixture.savePort.queueSuccess();
      fixture.savePort.markDirty();
      fixture.servicePtr->start();

      fixture.servicePtr->savePeriodic();

      CHECK(fixture.savePort.saveCount() == 1);
      CHECK(fixture.scheduler.pendingDelays() == std::vector{std::chrono::milliseconds{10}});
      REQUIRE(fixture.scheduler.fireNext());
      CHECK(fixture.savePort.saveCount() == 2);
      CHECK(fixture.scheduler.pendingDelays().empty());
    }
  }

  TEST_CASE("PlaybackSessionSaveService - shutdown cancels deferred work and makes one final attempt",
            "[runtime][unit][playback-session]")
  {
    auto fixture = SaveServiceFixture{};
    fixture.savePort.markDirty();
    fixture.servicePtr->start();
    auto const cancelledDebounceId = fixture.scheduler.lastScheduledId();
    fixture.savePort.queueFailure();

    auto const result = fixture.servicePtr->shutdown();

    REQUIRE_FALSE(result);
    CHECK(result.error().message == "scripted playback-session save failure");
    CHECK(fixture.savePort.saveCount() == 1);
    CHECK_FALSE(fixture.savePort.hasSubscriber());
    CHECK(fixture.scheduler.pendingDelays().empty());
    REQUIRE(fixture.scheduler.forceFire(cancelledDebounceId));
    CHECK(fixture.savePort.saveCount() == 1);

    REQUIRE(fixture.servicePtr->shutdown());
    CHECK(fixture.savePort.saveCount() == 1);
  }

  TEST_CASE("PlaybackSessionSaveService - destruction neutralizes a cancelled scheduler callback",
            "[runtime][unit][playback-session][lifecycle]")
  {
    auto savePort = SavePort{};
    auto scheduler = ControlledSaveScheduler{};
    auto const timing = PlaybackSessionSaveService::Timing{
      .dirtyDebounceDelay = std::chrono::milliseconds{5},
      .initialRetryDelay = std::chrono::milliseconds{10},
      .maximumRetryDelay = std::chrono::milliseconds{40},
    };
    std::uint64_t cancelledTaskId = 0;

    {
      auto service = PlaybackSessionSaveService{savePort.port(), scheduler, timing};
      savePort.markDirty();
      service.start();
      cancelledTaskId = scheduler.lastScheduledId();
    }

    CHECK_FALSE(savePort.hasSubscriber());
    CHECK(scheduler.pendingDelays().empty());
    REQUIRE(scheduler.forceFire(cancelledTaskId));
    CHECK(savePort.saveCount() == 0);
  }
} // namespace ao::rt::test
