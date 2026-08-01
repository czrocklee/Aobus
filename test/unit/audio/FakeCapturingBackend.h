// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/Property.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <cstdint>
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
      PcmFormat format;
    };

    FakeCapturingBackend();
    ~FakeCapturingBackend() override;

    FakeCapturingBackend(FakeCapturingBackend const&) = delete;
    FakeCapturingBackend& operator=(FakeCapturingBackend const&) = delete;
    FakeCapturingBackend(FakeCapturingBackend&&) = delete;
    FakeCapturingBackend& operator=(FakeCapturingBackend&&) = delete;

    std::optional<PcmFormat> prewarmFormatHint(SignalFormat const& format) const noexcept override;
    Result<OpenedPcmMode> open(SignalFormat const& format, RenderTarget* target) override;
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
    void setPrewarmEncoding(std::optional<SampleEncoding> optEncoding);
    void setSelectedEncoding(std::optional<SampleEncoding> optEncoding);

    /**
     * @brief Makes open() report a confirmed endpoint of the given precision.
     *
     * Endpoint evidence is absent by default. Presence describes a direct
     * endpoint but never authorizes this fake to return a lossy client mode.
     */
    void setConfirmedEndpointPrecision(std::optional<std::uint8_t> optPrecisionBits);
    void setPropertyError(std::optional<Error::Code> optErr);
    std::vector<Event> events() const;
    void clearEvents();
    void setEventObserver(std::function<void(std::string_view)> observer);
    RenderTarget* target() const;
    PcmFormat currentFormat() const;

    // Trigger callbacks
    void emitRouteReady(std::string_view anchor);
    void emitFormatChanged(PcmFormat const& format);
    void emitBackendError(std::string_view message);
    void emitDrainComplete();
    void emitPropertyChanged(PropertyId id);

  private:
    PropertyInfo propertyInfoUnlocked(PropertyId id) const;
    PropertySnapshot propertySnapshotUnlocked(PropertyId id) const;
    void recordEvent(std::string_view name, PcmFormat const& format);

    mutable std::mutex _mutex;
    std::vector<Event> _events;
    std::function<void(std::string_view)> _eventObserver;
    RenderTarget* _target = nullptr;
    PcmFormat _format{};
    Result<> _openResult{};
    std::optional<SampleEncoding> _optPrewarmEncoding;
    std::optional<SampleEncoding> _optSelectedEncoding;
    std::optional<std::uint8_t> _optEndpointPrecisionBits;
    std::optional<Error::Code> _optPropError{};
    std::map<PropertyId, PropertyInfo> _mockPropertyInfos{};
    float _volume = 1.0F;
    bool _muted = false;
  };
} // namespace ao::audio::test
