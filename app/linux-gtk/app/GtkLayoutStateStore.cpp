// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkLayoutStateStore.h"

#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>

#include <filesystem>
#include <memory>

namespace ao::gtk
{
  GtkLayoutStateStore::GtkLayoutStateStore(std::filesystem::path const& libraryPath)
  {
    auto const configPath = libraryPath / "gtk_layout.yaml";
    _storePtr = std::make_unique<rt::ConfigStore>(configPath);
  }

  GtkLayoutStateStore::~GtkLayoutStateStore() = default;

  GtkLayoutStateStore::GtkLayoutStateStore(GtkLayoutStateStore&&) noexcept = default;
  GtkLayoutStateStore& GtkLayoutStateStore::operator=(GtkLayoutStateStore&&) noexcept = default;

  void GtkLayoutStateStore::load(uimodel::TrackColumnLayoutState& layoutState,
                                 uimodel::ListPresentationPreferenceState& prefState) const
  {
    auto const loadedLayouts =
      _storePtr->load("trackView.columnLayouts", layoutState, uimodel::TrackColumnLayoutYamlSchema{});

    if (!loadedLayouts)
    {
      APP_LOG_DEBUG("GtkLayoutStateStore: Failed to load column layouts: {}", loadedLayouts.error().message);
    }

    auto const loadedPreferences =
      _storePtr->load("trackView.presentations", prefState, uimodel::ListPresentationPreferenceYamlSchema{});

    if (!loadedPreferences)
    {
      APP_LOG_DEBUG(
        "GtkLayoutStateStore: Failed to load presentation preferences: {}", loadedPreferences.error().message);
    }
  }

  void GtkLayoutStateStore::save(uimodel::TrackColumnLayoutState const& layoutState,
                                 uimodel::ListPresentationPreferenceState const& prefState)
  {
    if (auto const res = _storePtr->saveTogether(
          rt::configWrite("trackView.columnLayouts", layoutState, uimodel::TrackColumnLayoutYamlSchema{}),
          rt::configWrite("trackView.presentations", prefState, uimodel::ListPresentationPreferenceYamlSchema{}));
        !res)
    {
      APP_LOG_ERROR("GtkLayoutStateStore: Failed to save: {}", res.error().message);
    }
  }
} // namespace ao::gtk
