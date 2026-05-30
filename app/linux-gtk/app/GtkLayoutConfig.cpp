// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkLayoutConfig.h"

#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>
#include <ao/utility/Log.h>

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

  void GtkLayoutConfig::load(uimodel::track::ColumnLayoutState& state) const
  {
    if (auto const res = _storePtr->load("trackView", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("GtkLayoutConfig: Failed to load: {}", res.error().message);
    }
  }

  void GtkLayoutConfig::save(uimodel::track::ColumnLayoutState const& state)
  {
    _storePtr->save("trackView", state);

    if (auto const res = _storePtr->flush(); !res)
    {
      APP_LOG_ERROR("GtkLayoutConfig: Failed to flush: {}", res.error().message);
    }
  }
} // namespace ao::gtk
