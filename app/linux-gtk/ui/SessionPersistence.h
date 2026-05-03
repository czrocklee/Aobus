// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibrarySession.h"
#include "TrackPresentation.h"
#include <common/AppConfig.h>

#include <ao/audio/Backend.h>
#include <gtkmm.h>

#include <memory>
#include <string>

namespace ao::gtk
{
  class SessionPersistence final
  {
  public:
    SessionPersistence();
    ~SessionPersistence() = default;

    void load(Gtk::Window& window,
              Gtk::Paned& paned,
              TrackColumnLayoutModel& trackColumnLayoutModel,
              std::string& outLibraryPath,
              ao::audio::BackendId& outBackend,
              ao::audio::ProfileId& outProfile,
              ao::audio::DeviceId& outDeviceId);

    void save(Gtk::Window const& window,
              Gtk::Paned const& paned,
              TrackColumnLayoutModel const& trackColumnLayoutModel,
              LibrarySession const* librarySession);

    void updateAudioBackend(ao::audio::BackendId const& backend,
                            ao::audio::ProfileId const& profile,
                            ao::audio::DeviceId const& deviceId);

  private:
    ao::app::AppConfig _appConfig;
  };
} // namespace ao::gtk
