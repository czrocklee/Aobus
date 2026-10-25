// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "detail/CoreAudioUnitRun.h"
#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/SignalFormat.h>

#include <AudioToolbox/AUComponent.h>

#include <functional>
#include <memory>
#include <optional>

namespace ao::audio::backend::detail
{
  class BackendGraphRegistry;
}

namespace ao::audio
{
  struct Device;
  class RenderTarget;
}

namespace ao::audio::backend
{
  /** @brief Shared-mode macOS output through the AudioUnit HAL output unit. */
  class CoreAudioBackend final : public Backend
  {
  public:
    // Synchronous native bindings, not callbacks into Backend or RenderTarget.
    // Every binding must be nonempty at construction; this does not contain
    // exceptions thrown by an injected callable. Defaults call the SDK. The
    // maximum-frames reader is used only for that provisioned readback; the
    // backend still validates its extent and value.
    struct NativeOperations final
    {
      struct UnitOperations final
      {
        std::function<decltype(::AudioUnitReset)> reset{::AudioUnitReset};
        std::function<decltype(::AudioUnitGetProperty)> getMaximumFramesPerSlice{::AudioUnitGetProperty};
      };

      detail::CoreAudioUnitRunOperations run{};
      UnitOperations unit{};
    };

    explicit CoreAudioBackend(Device const& device, ProfileId const& profile);
    CoreAudioBackend(Device const& device,
                     ProfileId const& profile,
                     std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr);
    CoreAudioBackend(Device const& device,
                     ProfileId const& profile,
                     std::shared_ptr<detail::BackendGraphRegistry> graphRegistryPtr,
                     NativeOperations operations);
    ~CoreAudioBackend() override;

    CoreAudioBackend(CoreAudioBackend const&) = delete;
    CoreAudioBackend& operator=(CoreAudioBackend const&) = delete;
    CoreAudioBackend(CoreAudioBackend&&) = delete;
    CoreAudioBackend& operator=(CoreAudioBackend&&) = delete;

    std::optional<PcmFormat> prewarmFormatHint(SignalFormat const& sourceFormat) const noexcept override;
    BackendOpenResult open(SignalFormat const& sourceFormat, RenderTarget& target) override;
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
