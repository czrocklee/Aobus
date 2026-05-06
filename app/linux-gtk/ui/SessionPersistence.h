// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <filesystem>

#include "TrackPresentation.h"
#include <common/AppConfig.h>

#include <ao/audio/Backend.h>
#include <gtkmm.h>
#include <runtime/ISessionPersistence.h>

#include <memory>
#include <optional>
#include <string>

namespace ao::gtk
{
  class SessionPersistence final : public ao::app::ISessionPersistence
  {
  public:
    SessionPersistence();
    ~SessionPersistence() override = default;

    std::optional<ao::app::SessionSnapshot> loadSnapshot() override;
    void saveSnapshot(ao::app::SessionSnapshot const& snapshot) override;

    void loadUi(Gtk::Window& window, Gtk::Paned& paned, TrackColumnLayoutModel& trackColumnLayoutModel);

    void saveUi(Gtk::Window const& window,
                Gtk::Paned const& paned,
                TrackColumnLayoutModel const& trackColumnLayoutModel);

  private:
    ao::app::AppConfig _appConfig;
  };
} // namespace ao::gtk
