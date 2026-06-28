// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkLayoutConfig.h"

#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/track/TrackColumnLayoutStore.h>
#include <ao/uimodel/track/TrackPresentationPreferenceStore.h>

#include <filesystem>
#include <memory>

namespace ao::gtk
{
  GtkLayoutConfig::GtkLayoutConfig(std::filesystem::path const& libraryPath)
  {
    auto const configPath = libraryPath / "gtk_layout.yaml";
    _storePtr = std::make_unique<rt::ConfigStore>(configPath);
  }

  GtkLayoutConfig::~GtkLayoutConfig() = default;

  GtkLayoutConfig::GtkLayoutConfig(GtkLayoutConfig&&) noexcept = default;
  GtkLayoutConfig& GtkLayoutConfig::operator=(GtkLayoutConfig&&) noexcept = default;

  void GtkLayoutConfig::load(uimodel::track::TrackColumnLayoutState& layoutState,
                             uimodel::track::ListPresentationPreferenceState& prefState) const
  {
    if (auto const res = _storePtr->load("trackView.columnLayouts", layoutState);
        !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("GtkLayoutConfig: Failed to load column layouts: {}", res.error().message);
    }

    if (auto const res = _storePtr->load("trackView.presentations", prefState);
        !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("GtkLayoutConfig: Failed to load presentations: {}", res.error().message);
    }
  }

  void GtkLayoutConfig::save(uimodel::track::TrackColumnLayoutState const& layoutState,
                             uimodel::track::ListPresentationPreferenceState const& prefState)
  {
    _storePtr->save("trackView.columnLayouts", layoutState);
    _storePtr->save("trackView.presentations", prefState);

    if (auto const res = _storePtr->flush(); !res)
    {
      APP_LOG_ERROR("GtkLayoutConfig: Failed to flush: {}", res.error().message);
    }
  }
} // namespace ao::gtk
