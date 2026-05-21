// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace Gtk
{
  class Window;
}

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::gtk
{
  class TrackColumnLayoutModel;

  /**
   * Manages persistence of GTK window and track view state.
   */
  class WindowStatePersistence final
  {
  public:
    explicit WindowStatePersistence(rt::ConfigStore& globalConfig);

    void loadWindow(Gtk::Window& window) const;
    void saveWindow(Gtk::Window const& window) const;

    void loadTrackView(TrackColumnLayoutModel& model) const;
    void saveTrackView(TrackColumnLayoutModel const& model) const;

  private:
    rt::ConfigStore& _globalConfig;
  };
} // namespace ao::gtk
