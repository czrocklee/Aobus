// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <functional>

namespace ao::audio
{
  struct OutputDeviceSelection;
}

namespace ao::uimodel
{
  /**
   * @brief Where a surface sends the exact output route a user asked for.
   *
   * Restoring a route in a later session depends on the requested tuple being
   * written down, and the engine-confirmed selection Runtime publishes is not
   * that tuple. Every surface that can raise a selection therefore names a
   * destination, and a surface that keeps no preference says so with
   * `discarded()`.
   *
   * The distinction matters because an absent destination and a deliberate
   * non-recording surface are indistinguishable once both are an empty
   * callback. This type is not default-constructible, so a dependency bundle
   * carrying one cannot be assembled without deciding which it is, and a shell
   * that forgets to forward its recorder fails to compile rather than silently
   * dropping every selection its user makes.
   */
  class OutputDeviceIntent final
  {
  public:
    using Recorder = std::function<void(audio::OutputDeviceSelection const&)>;

    /// Send every requested route to @p recorder, which must be callable.
    static OutputDeviceIntent recordedBy(Recorder recorder);

    /// Keep no preference, for a surface that presents routes it does not own.
    static OutputDeviceIntent discarded();

    void record(audio::OutputDeviceSelection const& selection) const;

  private:
    explicit OutputDeviceIntent(Recorder recorder);

    Recorder _recorder;
  };
} // namespace ao::uimodel
