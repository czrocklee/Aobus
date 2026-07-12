// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace ao::tui
{
  // Accumulated timing for one render segment over a window of frames. Pure data
  // (no clock, environment, or I/O), so it is unit-testable in isolation.
  struct FrameSegmentStats final
  {
    std::uint64_t count = 0;
    std::chrono::nanoseconds totalDuration{0};
    std::chrono::nanoseconds peakDuration{0};

    void add(std::chrono::nanoseconds sampleDuration) noexcept;
    std::chrono::nanoseconds averageDuration() const noexcept;
  };

  // A closed window's per-segment timing, produced when a window fills. The two
  // segments mirror the TUI draw path: element-tree build (the renderer lambda)
  // and present (ftxui::Render plus the terminal flush).
  struct FrameTimingReport final
  {
    std::uint64_t frames = 0;
    FrameSegmentStats build{};
    FrameSegmentStats present{};
  };

  // Pure, side-effect-free frame-timing accumulator. Records one (build, present)
  // pair per frame and returns a FrameTimingReport every reportInterval frames,
  // resetting the window. Split out from FrameTimer so the windowing and
  // statistics are unit-testable without a clock, environment, or logging.
  class FrameTimingWindow final
  {
  public:
    explicit FrameTimingWindow(std::uint64_t reportInterval) noexcept;

    // Records one frame's build and present durations. Returns a report (and
    // resets the window) once reportInterval frames have accumulated.
    std::optional<FrameTimingReport> record(std::chrono::nanoseconds buildDuration,
                                            std::chrono::nanoseconds presentDuration) noexcept;

    // Returns the in-progress window when it holds any frames, without resetting.
    std::optional<FrameTimingReport> pending() const noexcept;

    // Returns and resets the in-progress window. Used to emit a trailing partial
    // report before logging shuts down.
    std::optional<FrameTimingReport> flush() noexcept;

  private:
    std::uint64_t _reportInterval;
    FrameTimingReport _window{};
  };

  // Opt-in per-frame render timing for the TUI. Enabled by setting the
  // AOBUS_TUI_FRAME_TIMING environment variable to a truthy value (anything other
  // than unset, "", "0", "false", "off", or "no"). When disabled every method is
  // a cheap no-op with no clock reads, so the normal path pays only a bool check.
  //
  // Attribution rationale: the stock FTXUI loop bundles the renderer build,
  // ftxui::Render, and the terminal flush inside a private ScreenInteractive::Draw,
  // so Render and flush cannot be split apart without replacing the loop. This
  // timer measures the two segments that are cleanly separable -- build versus
  // present -- excluding the loop's blocking event wait (present is measured
  // from build-end, not from the loop entry).
  //
  // The TUI owns the terminal via the alternate screen, so reports go to the
  // runtime log (APP_LOG at info level), never to stdout/stderr. Run with log
  // level Info or below (the default) to observe them.
  class FrameTimer final
  {
  public:
    using Clock = std::chrono::steady_clock;

    FrameTimer();
    explicit FrameTimer(bool enabled) noexcept;
    ~FrameTimer() = default;

    FrameTimer(FrameTimer const&) = delete;
    FrameTimer& operator=(FrameTimer const&) = delete;
    FrameTimer(FrameTimer&&) = delete;
    FrameTimer& operator=(FrameTimer&&) = delete;

    bool enabled() const noexcept { return _enabled; }

    // Scope guard timing the element-tree build. Construct it at the top of the
    // renderer lambda; on destruction (after the returned Element is built, on
    // whichever return path the renderer takes) it records the build duration and
    // the build-end timestamp from which the following present duration derives.
    class [[nodiscard]] BuildScope final
    {
    public:
      explicit BuildScope(FrameTimer& timer) noexcept;
      ~BuildScope();

      BuildScope(BuildScope const&) = delete;
      BuildScope& operator=(BuildScope const&) = delete;
      BuildScope(BuildScope&&) = delete;
      BuildScope& operator=(BuildScope&&) = delete;

    private:
      FrameTimer* _timer;
      Clock::time_point _start{};
    };

    BuildScope measureBuild() noexcept { return BuildScope{*this}; }

    // Call once immediately after each loop draw step (RunOnceBlocking). When a
    // build occurred since the previous call, records present = now - buildEnd
    // (the ftxui::Render plus terminal flush that ran between the build and here).
    void recordPresentIfDrawn();

    // Emits and resets a trailing partial report. Call before the logging system
    // shuts down.
    void flush();

  private:
    void onBuildComplete(Clock::time_point start) noexcept;
    void emit(FrameTimingReport const& report) const;

    bool _enabled;
    bool _hasPendingFrame = false;
    Clock::time_point _buildEnd{};
    Clock::duration _pendingBuildDuration{0};
    FrameTimingWindow _window;
  };
} // namespace ao::tui
