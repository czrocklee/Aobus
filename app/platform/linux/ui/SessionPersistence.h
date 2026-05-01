// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#pragma once

#include "platform/linux/AppConfig.h"
#include "platform/linux/ui/LibrarySession.h"
#include "platform/linux/ui/TrackPresentation.h"

#include <gtkmm.h>
#include <rs/audio/Backend.h>

#include <memory>
#include <string>

namespace app::ui
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
              rs::audio::BackendKind& outBackendKind,
              std::string& outDeviceId);

    void save(Gtk::Window const& window,
              Gtk::Paned const& paned,
              TrackColumnLayoutModel const& trackColumnLayoutModel,
              LibrarySession const* librarySession);

    void updateAudioBackend(rs::audio::BackendKind kind, std::string const& deviceId);

  private:
    rs::library::AppConfig _appConfig;
  };
} // namespace app::ui
