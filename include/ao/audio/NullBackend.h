// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/IBackend.h>

namespace ao::audio
{
  /**
   * @brief A backend that does nothing (used when no hardware is selected).
   */
  class NullBackend : public IBackend
  {
  public:
    NullBackend() = default;
    ~NullBackend() override = default;

    ao::Result<> open(Format const& /*format*/, RenderCallbacks /*callbacks*/) override { return {}; }
    void reset() override {}
    void start() override {}
    void pause() override {}
    void resume() override {}
    void flush() override {}
    void drain() override {}
    void stop() override {}
    void close() override {}

    void setExclusiveMode(bool /*exclusive*/) override {}
    bool isExclusiveMode() const noexcept override { return false; }

    BackendId backendId() const noexcept override { return BackendId{kBackendNone}; }
    ProfileId profileId() const noexcept override { return ProfileId{kProfileShared}; }
  };
} // namespace ao::audio
