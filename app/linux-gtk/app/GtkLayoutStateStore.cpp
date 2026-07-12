// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkLayoutStateStore.h"

#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

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
    if (auto const res = _storePtr->load("trackView.columnLayouts", layoutState);
        !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("GtkLayoutStateStore: Failed to load column layouts: {}", res.error().message);
    }

    if (auto const res = _storePtr->load("trackView.presentations", prefState);
        !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("GtkLayoutStateStore: Failed to load presentations: {}", res.error().message);
    }
  }

  void GtkLayoutStateStore::save(uimodel::TrackColumnLayoutState const& layoutState,
                                 uimodel::ListPresentationPreferenceState const& prefState)
  {
    _storePtr->save("trackView.columnLayouts", layoutState);
    _storePtr->save("trackView.presentations", prefState);

    if (auto const res = _storePtr->flush(); !res)
    {
      APP_LOG_ERROR("GtkLayoutStateStore: Failed to flush: {}", res.error().message);
    }
  }
} // namespace ao::gtk
