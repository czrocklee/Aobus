// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/Property.h>
#include <ao/audio/SignalFormat.h>

#include <memory>

namespace ao::audio::backend::detail
{
  class BackendGraphRegistry;
}

namespace ao::audio::backend
{
  /** @brief Shared-mode macOS output through the AudioUnit HAL output unit. */
  class CoreAudioBackend final : public Backend
  {
  public:
    explicit CoreAudioBackend(Device const& device, ProfileId const& profile);
    CoreAudioBackend(Device const& device,
                     ProfileId const& profile,
                     std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr);
    ~CoreAudioBackend() override;

    CoreAudioBackend(CoreAudioBackend const&) = delete;
    CoreAudioBackend& operator=(CoreAudioBackend const&) = delete;
    CoreAudioBackend(CoreAudioBackend&&) = delete;
    CoreAudioBackend& operator=(CoreAudioBackend&&) = delete;

    Result<OpenedPcmMode> open(SignalFormat const& sourceFormat, RenderTarget& target) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    void close() override;

    Result<> setProperty(PropertyId id, PropertyValue const& value) override;
    Result<PropertyValue> property(PropertyId id) const override;
    PropertyInfo queryProperty(PropertyId id) const noexcept override;

    BackendId backendId() const override;
    ProfileId profileId() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio::backend
