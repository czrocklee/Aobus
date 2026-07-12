// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "FrameTimer.h"

#include <ao/rt/Log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ratio>
#include <string_view>

namespace ao::tui
{
  namespace
  {
    constexpr std::uint64_t kFrameTimingReportInterval = 120;

    bool frameTimingEnabledFromEnv() noexcept
    {
      auto const* const value = std::getenv("AOBUS_TUI_FRAME_TIMING");

      if (value == nullptr)
      {
        return false;
      }

      auto const text = std::string_view{value};

      return !(text.empty() || text == "0" || text == "false" || text == "off" || text == "no");
    }

    double toMicros(std::chrono::nanoseconds duration) noexcept
    {
      return std::chrono::duration<double, std::micro>{duration}.count();
    }
  } // namespace

  void FrameSegmentStats::add(std::chrono::nanoseconds sampleDuration) noexcept
  {
    ++count;
    totalDuration += sampleDuration;
    peakDuration = std::max(peakDuration, sampleDuration);
  }

  std::chrono::nanoseconds FrameSegmentStats::averageDuration() const noexcept
  {
    return count == 0 ? std::chrono::nanoseconds{0} : totalDuration / static_cast<std::int64_t>(count);
  }

  FrameTimingWindow::FrameTimingWindow(std::uint64_t reportInterval) noexcept
    : _reportInterval{std::max<std::uint64_t>(1, reportInterval)}
  {
  }

  std::optional<FrameTimingReport> FrameTimingWindow::record(std::chrono::nanoseconds buildDuration,
                                                             std::chrono::nanoseconds presentDuration) noexcept
  {
    ++_window.frames;
    _window.build.add(buildDuration);
    _window.present.add(presentDuration);

    if (_window.frames < _reportInterval)
    {
      return std::nullopt;
    }

    auto const report = _window;
    _window = FrameTimingReport{};

    return report;
  }

  std::optional<FrameTimingReport> FrameTimingWindow::pending() const noexcept
  {
    if (_window.frames == 0)
    {
      return std::nullopt;
    }

    return _window;
  }

  std::optional<FrameTimingReport> FrameTimingWindow::flush() noexcept
  {
    auto const optReport = pending();
    _window = FrameTimingReport{};
    return optReport;
  }

  FrameTimer::BuildScope::BuildScope(FrameTimer& timer) noexcept
    : _timer{&timer}
  {
    if (_timer->_enabled)
    {
      _start = Clock::now();
    }
  }

  FrameTimer::BuildScope::~BuildScope()
  {
    if (_timer->_enabled)
    {
      _timer->onBuildComplete(_start);
    }
  }

  FrameTimer::FrameTimer()
    : FrameTimer{frameTimingEnabledFromEnv()}
  {
  }

  FrameTimer::FrameTimer(bool enabled) noexcept
    : _enabled{enabled}, _window{kFrameTimingReportInterval}
  {
  }

  void FrameTimer::onBuildComplete(Clock::time_point start) noexcept
  {
    _buildEnd = Clock::now();
    _pendingBuildDuration = _buildEnd - start;
    _hasPendingFrame = true;
  }

  void FrameTimer::recordPresentIfDrawn()
  {
    if (!_enabled || !_hasPendingFrame)
    {
      return;
    }

    auto const presentDuration = Clock::now() - _buildEnd;
    _hasPendingFrame = false;

    if (auto const optReport = _window.record(_pendingBuildDuration, presentDuration); optReport)
    {
      emit(*optReport);
    }
  }

  void FrameTimer::flush()
  {
    if (!_enabled)
    {
      return;
    }

    if (auto const optReport = _window.flush(); optReport)
    {
      emit(*optReport);
    }
  }

  void FrameTimer::emit(FrameTimingReport const& report) const
  {
    APP_LOG_INFO("tui frame timing over {} frames: build avg={:.1f}us max={:.1f}us | present avg={:.1f}us max={:.1f}us",
                 report.frames,
                 toMicros(report.build.averageDuration()),
                 toMicros(report.build.peakDuration),
                 toMicros(report.present.averageDuration()),
                 toMicros(report.present.peakDuration));
  }
} // namespace ao::tui
