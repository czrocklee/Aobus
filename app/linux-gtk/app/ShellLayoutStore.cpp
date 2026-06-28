// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutStore.h"

#include <ao/Exception.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>

#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::gtk
{
  ShellLayoutStore::ShellLayoutStore(std::filesystem::path layoutsDir)
    : _layoutsDir{std::move(layoutsDir)}
  {
  }

  ShellLayoutStore::~ShellLayoutStore() = default;
  ShellLayoutStore::ShellLayoutStore(ShellLayoutStore&&) noexcept = default;
  ShellLayoutStore& ShellLayoutStore::operator=(ShellLayoutStore&&) noexcept = default;

  std::filesystem::path ShellLayoutStore::filePath(std::string_view presetId) const
  {
    if (presetId.empty() || presetId.find('/') != std::string_view::npos ||
        presetId.find('\\') != std::string_view::npos || presetId.find("..") != std::string_view::npos)
    {
      ao::throwException<ao::Exception>("Invalid preset ID: path traversal attempt or empty ID");
    }

    return _layoutsDir / std::format("{}.yaml", presetId);
  }

  std::optional<uimodel::LayoutDocument> ShellLayoutStore::load(std::string_view presetId) const
  {
    auto const path = filePath(presetId);

    if (!std::filesystem::exists(path))
    {
      return std::nullopt;
    }

    auto store = rt::ConfigStore{path, rt::ConfigStore::OpenMode::ReadOnly};
    auto doc = uimodel::LayoutDocument{};

    if (auto const res = uimodel::loadLayout(store, "layout", doc); !res)
    {
      APP_LOG_WARN("ShellLayoutStore: Failed to load layout ({}): {}", path.string(), res.error().message);
      return std::nullopt;
    }

    return doc;
  }

  void ShellLayoutStore::save(uimodel::LayoutDocument const& doc, std::string_view presetId)
  {
    auto const path = filePath(presetId);

    auto store = rt::ConfigStore{path, rt::ConfigStore::OpenMode::ReadWrite};
    uimodel::saveLayout(store, "layout", doc);

    if (auto const res = store.flush(); !res)
    {
      APP_LOG_ERROR("ShellLayoutStore: Failed to flush layout ({}): {}", path.string(), res.error().message);
    }
  }

  void ShellLayoutStore::remove(std::string_view presetId)
  {
    auto const path = filePath(presetId);
    auto ec = std::error_code{};

    std::filesystem::remove(path, ec);
  }
} // namespace ao::gtk
