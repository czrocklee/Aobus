// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineFailureTestSupport.h"

#include <ao/audio/Engine.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>

namespace ao::audio::test
{
  std::future<Engine::PlaybackFailure> captureNextFailure(Engine& engine)
  {
    auto promisePtr = std::make_shared<std::promise<Engine::PlaybackFailure>>();
    auto future = promisePtr->get_future();

    engine.setOnPlaybackFailure([promisePtr](Engine::PlaybackFailure const& failure)
                                { promisePtr->set_value(failure); });

    return future;
  }

  Engine::PlaybackFailure requireFailure(std::future<Engine::PlaybackFailure>& future)
  {
    REQUIRE(future.wait_for(std::chrono::seconds{15}) == std::future_status::ready);
    return future.get();
  }
} // namespace ao::audio::test
