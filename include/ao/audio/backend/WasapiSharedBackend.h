// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>

#include <memory>

namespace ao::audio
{
  struct Format;
  class RenderTarget;
}

namespace ao::audio::backend::detail
{
  class WasapiGraphRegistry;
}

namespace ao::audio::backend
{
  /**
   * @brief Audio backend using WASAPI in shared mode.
   *
   * The stream is opened with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, so the
   * Windows audio engine converts sample rate/format/channels to the endpoint
   * mix format; the decoder-side format is passed through unchanged.
   */
  class WasapiSharedBackend final : public Backend
  {
  public:
    explicit WasapiSharedBackend(Device const& device, ProfileId const& profile);
    explicit WasapiSharedBackend(Device const& device,
                                 ProfileId const& profile,
                                 std::shared_ptr<detail::WasapiGraphRegistry> graphRegistryPtr);
    ~WasapiSharedBackend() override;

    WasapiSharedBackend(WasapiSharedBackend const&) = delete;
    WasapiSharedBackend& operator=(WasapiSharedBackend const&) = delete;
    WasapiSharedBackend(WasapiSharedBackend&&) = delete;
    WasapiSharedBackend& operator=(WasapiSharedBackend&&) = delete;

    Result<> open(Format const& format, RenderTarget* target) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    void close() override;

    BackendId backendId() const override;
    ProfileId profileId() const override;

    Result<> setProperty(PropertyId id, PropertyValue const& value) override;
    Result<PropertyValue> property(PropertyId id) const override;
    PropertyInfo queryProperty(PropertyId id) const noexcept override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::audio::backend
