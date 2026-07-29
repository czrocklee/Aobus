// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Format.h>
#include <ao/audio/Property.h>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio
{
  class RenderTarget;
}

namespace ao::audio::test
{
  class FakeCapturingBackend final : public Backend
  {
  public:
    struct Event final
    {
      std::string name;
      Format format;
    };

    FakeCapturingBackend();
    ~FakeCapturingBackend() override;

    FakeCapturingBackend(FakeCapturingBackend const&) = delete;
    FakeCapturingBackend& operator=(FakeCapturingBackend const&) = delete;
    FakeCapturingBackend(FakeCapturingBackend&&) = delete;
    FakeCapturingBackend& operator=(FakeCapturingBackend&&) = delete;

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

    // Helpers for tests
    void setMockPropertyInfo(PropertyId id, PropertyInfo const& info);
    void setOpenResult(Result<> res);
    void setPropertyError(std::optional<Error::Code> optErr);
    std::vector<Event> events() const;
    void clearEvents();
    void setEventObserver(std::function<void(std::string_view)> observer);
    RenderTarget* target() const;
    Format currentFormat() const;

    // Trigger callbacks
    void emitRouteReady(std::string_view anchor);
    void emitFormatChanged(Format const& format);
    void emitBackendError(std::string_view message);
    void emitDrainComplete();
    void emitPropertyChanged(PropertyId id);

  private:
    PropertyInfo propertyInfoUnlocked(PropertyId id) const;
    PropertySnapshot propertySnapshotUnlocked(PropertyId id) const;
    void recordEvent(std::string_view name, Format const& format);

    mutable std::mutex _mutex;
    std::vector<Event> _events;
    std::function<void(std::string_view)> _eventObserver;
    RenderTarget* _target = nullptr;
    Format _format{};
    Result<> _openResult{};
    std::optional<Error::Code> _optPropError{};
    std::map<PropertyId, PropertyInfo> _mockPropertyInfos{};
    float _volume = 1.0F;
    bool _muted = false;
  };
} // namespace ao::audio::test
