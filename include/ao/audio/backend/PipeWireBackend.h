// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>

#include <memory>
#include <string>

namespace ao::audio
{
  struct Format;
  class RenderTarget;
}

namespace ao::audio::backend
{
  /**
   * @brief Audio backend using PipeWire.
   */
  class PipeWireBackend final : public Backend
  {
  public:
    struct Impl;
    explicit PipeWireBackend(Device const& device, ProfileId const& profile);
    ~PipeWireBackend() override;

    PipeWireBackend(PipeWireBackend const&) = delete;
    PipeWireBackend& operator=(PipeWireBackend const&) = delete;
    PipeWireBackend(PipeWireBackend&&) = delete;
    PipeWireBackend& operator=(PipeWireBackend&&) = delete;

    Result<> open(Format const& format, RenderTarget* target) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    void close() override;

    BackendId backendId() const noexcept override;
    ProfileId profileId() const noexcept override;

    Result<> setProperty(PropertyId id, PropertyValue const& value) override;
    Result<PropertyValue> property(PropertyId id) const override;
    PropertyInfo queryProperty(PropertyId id) const noexcept override;

  private:
    std::unique_ptr<Impl> _implPtr;
    std::string _targetDeviceId;
    bool _exclusiveMode = false;
  };
} // namespace ao::audio::backend
