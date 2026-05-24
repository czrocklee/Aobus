// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkLayoutConfig.h"

#include "UIState.h"
#include "ao/utility/Log.h"
#include "runtime/ConfigStore.h"

#include <filesystem>
#include <memory>
#include <system_error>

namespace ao::gtk
{
  GtkLayoutConfig::GtkLayoutConfig(std::filesystem::path const& libraryPath)
  {
    auto const configPath = libraryPath / "gtk_layout.yaml";
    auto const oldConfigPath = libraryPath / "gtk_workspace.yaml";

    // Fallback: if gtk_layout.yaml doesn't exist but gtk_workspace.yaml does, rename it.
    if (!std::filesystem::exists(configPath) && std::filesystem::exists(oldConfigPath))
    {
      auto ec = std::error_code{};
      std::filesystem::rename(oldConfigPath, configPath, ec);

      if (ec)
      {
        APP_LOG_WARN("GtkLayoutConfig: Failed to migrate old config: {}", ec.message());
      }
    }

    _store = std::make_unique<rt::ConfigStore>(configPath);
  }

  GtkLayoutConfig::~GtkLayoutConfig() = default;

  GtkLayoutConfig::GtkLayoutConfig(GtkLayoutConfig&&) noexcept = default;
  GtkLayoutConfig& GtkLayoutConfig::operator=(GtkLayoutConfig&&) noexcept = default;

  void GtkLayoutConfig::load(ColumnLayoutState& state) const
  {
    if (auto const res = _store->load("trackView", state); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("GtkLayoutConfig: Failed to load: {}", res.error().message);
    }
  }

  void GtkLayoutConfig::save(ColumnLayoutState const& state)
  {
    _store->save("trackView", state);

    if (auto const res = _store->flush(); !res)
    {
      APP_LOG_ERROR("GtkLayoutConfig: Failed to flush: {}", res.error().message);
    }
  }
} // namespace ao::gtk
