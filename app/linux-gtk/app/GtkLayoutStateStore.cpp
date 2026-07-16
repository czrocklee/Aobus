// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkLayoutStateStore.h"

#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceCodec.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutCodec.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <filesystem>
#include <memory>
#include <utility>

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
    auto const containsLayouts = _storePtr->contains("trackView.columnLayouts");

    if (!containsLayouts)
    {
      APP_LOG_DEBUG("GtkLayoutStateStore: Failed to inspect column layouts: {}", containsLayouts.error().message);
    }
    else if (*containsLayouts)
    {
      auto document = uimodel::TrackColumnLayoutDocument{};

      if (auto const loaded = _storePtr->loadExact("trackView.columnLayouts", document); !loaded)
      {
        APP_LOG_DEBUG("GtkLayoutStateStore: Failed to load column layouts: {}", loaded.error().message);
      }
      else if (auto decoded = uimodel::decodeTrackColumnLayout(document); !decoded)
      {
        APP_LOG_DEBUG("GtkLayoutStateStore: Rejected column layouts: {}", decoded.error().message);
      }
      else
      {
        layoutState = std::move(*decoded);
      }
    }

    auto const containsPreferences = _storePtr->contains("trackView.presentations");

    if (!containsPreferences)
    {
      APP_LOG_DEBUG(
        "GtkLayoutStateStore: Failed to inspect presentation preferences: {}", containsPreferences.error().message);
    }
    else if (*containsPreferences)
    {
      auto document = uimodel::ListPresentationPreferenceDocument{};

      if (auto const loaded = _storePtr->loadExact("trackView.presentations", document); !loaded)
      {
        APP_LOG_DEBUG("GtkLayoutStateStore: Failed to load presentation preferences: {}", loaded.error().message);
      }
      else if (auto decoded = uimodel::decodeListPresentationPreferences(document); !decoded)
      {
        APP_LOG_DEBUG("GtkLayoutStateStore: Rejected presentation preferences: {}", decoded.error().message);
      }
      else
      {
        prefState = std::move(*decoded);
      }
    }
  }

  void GtkLayoutStateStore::save(uimodel::TrackColumnLayoutState const& layoutState,
                                 uimodel::ListPresentationPreferenceState const& prefState)
  {
    auto layoutDocument = uimodel::encodeTrackColumnLayout(layoutState);

    if (!layoutDocument)
    {
      APP_LOG_ERROR("GtkLayoutStateStore: Failed to encode column layouts: {}", layoutDocument.error().message);
      return;
    }

    auto preferenceDocument = uimodel::encodeListPresentationPreferences(prefState);

    if (!preferenceDocument)
    {
      APP_LOG_ERROR(
        "GtkLayoutStateStore: Failed to encode presentation preferences: {}", preferenceDocument.error().message);
      return;
    }

    if (auto const res =
          _storePtr->save("trackView.columnLayouts", *layoutDocument, "trackView.presentations", *preferenceDocument);
        !res)
    {
      APP_LOG_ERROR("GtkLayoutStateStore: Failed to save: {}", res.error().message);
    }
  }
} // namespace ao::gtk
