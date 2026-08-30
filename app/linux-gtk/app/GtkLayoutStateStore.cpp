// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkLayoutStateStore.h"

#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

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

  void GtkLayoutStateStore::load(uimodel::TrackColumnLayouts::Snapshot& layoutState,
                                 uimodel::ListPresentations::Snapshot& prefState) const
  {
    auto const loadedLayoutsRes =
      _storePtr->load(uimodel::kTrackColumnLayoutsConfigGroup, layoutState, uimodel::TrackColumnLayoutYamlSchema{});

    if (!loadedLayoutsRes)
    {
      APP_LOG_DEBUG("GtkLayoutStateStore: Failed to load column layouts: {}", loadedLayoutsRes.error().message);
    }

    auto const loadedPreferencesRes = _storePtr->load(
      uimodel::kListPresentationsConfigGroup, prefState, uimodel::ListPresentationPreferenceYamlSchema{});

    if (!loadedPreferencesRes)
    {
      APP_LOG_DEBUG(
        "GtkLayoutStateStore: Failed to load presentation preferences: {}", loadedPreferencesRes.error().message);
    }
  }

  void GtkLayoutStateStore::save(uimodel::TrackColumnLayouts::Snapshot const& layoutState,
                                 uimodel::ListPresentations::Snapshot const& prefState)
  {
    if (auto const resRes = _storePtr->saveTogether(
          rt::configWrite(uimodel::kTrackColumnLayoutsConfigGroup, layoutState, uimodel::TrackColumnLayoutYamlSchema{}),
          rt::configWrite(
            uimodel::kListPresentationsConfigGroup, prefState, uimodel::ListPresentationPreferenceYamlSchema{}));
        !resRes)
    {
      APP_LOG_ERROR("GtkLayoutStateStore: Failed to save: {}", resRes.error().message);
    }
  }
} // namespace ao::gtk
