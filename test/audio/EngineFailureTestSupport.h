// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Engine.h>

#include <future>

namespace ao::audio::test
{
  std::future<Engine::PlaybackFailure> captureNextFailure(Engine& engine);
  Engine::PlaybackFailure requireFailure(std::future<Engine::PlaybackFailure>& future);
} // namespace ao::audio::test
