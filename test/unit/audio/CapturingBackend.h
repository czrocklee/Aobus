// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/audio/IBackend.h>
#include <string>
#include <vector>

namespace ao::audio
{
  class CapturingBackend : public IBackend
  {
  public:
    struct Event
    {
      std::string name;
      Format format;
    };

    ao::Result<> open(Format const& format, RenderCallbacks callbacks) override
    {
      _events.push_back({"open", format});
      _callbacks = callbacks;
      _format = format;
      return _openResult;
    }

    void reset() override { _events.push_back({"reset", {}}); }
    void start() override { _events.push_back({"start", {}}); }
    void pause() override { _events.push_back({"pause", {}}); }
    void resume() override { _events.push_back({"resume", {}}); }
    void flush() override { _events.push_back({"flush", {}}); }
    void drain() override { _events.push_back({"drain", {}}); }
    void stop() override { _events.push_back({"stop", {}}); }
    void close() override { _events.push_back({"close", {}}); }

    void setExclusiveMode(bool exclusive) override { _exclusive = exclusive; }
    bool isExclusiveMode() const noexcept override { return _exclusive; }

    BackendId backendId() const noexcept override { return BackendId{"capturing"}; }
    ProfileId profileId() const noexcept override { return ProfileId{"test"}; }

    // Helpers for tests
    void setOpenResult(ao::Result<> res) { _openResult = res; }
    std::vector<Event> const& events() const { return _events; }
    void clearEvents() { _events.clear(); }
    RenderCallbacks const& callbacks() const { return _callbacks; }
    Format currentFormat() const { return _format; }

    // Trigger callbacks
    void fireRouteReady(std::string_view anchor)
    {
      if (_callbacks.onRouteReady) _callbacks.onRouteReady(_callbacks.userData, anchor);
    }
    void fireFormatChanged(Format const& fmt)
    {
      _format = fmt;
      if (_callbacks.onFormatChanged) _callbacks.onFormatChanged(_callbacks.userData, fmt);
    }
    void fireBackendError(std::string_view msg)
    {
      if (_callbacks.onBackendError) _callbacks.onBackendError(_callbacks.userData, msg);
    }
    void fireDrainComplete()
    {
      if (_callbacks.onDrainComplete) _callbacks.onDrainComplete(_callbacks.userData);
    }

  private:
    std::vector<Event> _events;
    RenderCallbacks _callbacks = {};
    Format _format = {};
    ao::Result<> _openResult = {};
    bool _exclusive = false;
  };
} // namespace ao::audio
