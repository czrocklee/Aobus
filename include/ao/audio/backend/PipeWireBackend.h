// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/IBackend.h>

#include <memory>

namespace ao::audio::backend
{
  /**
   * @brief Audio backend using PipeWire.
   */
  class PipeWireBackend final : public IBackend
  {
  public:
    struct Impl;
    explicit PipeWireBackend(Device const& device, ProfileId const& profile);
    ~PipeWireBackend() override;

    PipeWireBackend(PipeWireBackend const&) = delete;
    PipeWireBackend& operator=(PipeWireBackend const&) = delete;
    PipeWireBackend(PipeWireBackend&&) = delete;
    PipeWireBackend& operator=(PipeWireBackend&&) = delete;

    Result<> open(Format const& format, IRenderTarget* target) override;
    void start() override;
    void pause() override;
    void resume() override;
    void flush() override;
    void stop() override;
    void close() override;

    BackendId backendId() const noexcept override;
    ProfileId profileId() const noexcept override;

    void setExclusiveMode(bool exclusive);
    bool isExclusiveMode() const noexcept;

    Result<> setProperty(PropertyId id, PropertyValue const& value) override;
    Result<PropertyValue> getProperty(PropertyId id) const override;
    PropertyInfo queryProperty(PropertyId id) const noexcept override;

  private:
    std::unique_ptr<Impl> _impl;
    std::string _targetDeviceId;
    bool _exclusiveMode = false;
  };
} // namespace ao::audio::backend
