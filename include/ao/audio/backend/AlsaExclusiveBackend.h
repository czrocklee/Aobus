// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Error.h"
#include "ao/audio/Backend.h"
#include "ao/audio/Format.h"
#include "ao/audio/IBackend.h"
#include "ao/audio/IRenderTarget.h"
#include "ao/audio/Property.h"

#include <memory>

namespace ao::audio::backend
{
  /**
   * @brief Audio backend using ALSA in exclusive (hardware) mode.
   */
  class AlsaExclusiveBackend final : public IBackend
  {
  public:
    explicit AlsaExclusiveBackend(Device const& device, ProfileId const& profile);
    ~AlsaExclusiveBackend() override;

    AlsaExclusiveBackend(AlsaExclusiveBackend const&) = delete;
    AlsaExclusiveBackend& operator=(AlsaExclusiveBackend const&) = delete;
    AlsaExclusiveBackend(AlsaExclusiveBackend&&) = delete;
    AlsaExclusiveBackend& operator=(AlsaExclusiveBackend&&) = delete;

    Result<> open(Format const& format, IRenderTarget* target) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    void close() override;

    BackendId backendId() const noexcept override;
    ProfileId profileId() const noexcept override;

    Result<> setProperty(PropertyId id, PropertyValue const& value) override;
    Result<PropertyValue> getProperty(PropertyId id) const override;
    PropertyInfo queryProperty(PropertyId id) const noexcept override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
} // namespace ao::audio::backend
