// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutStore.h"

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::gtk
{
  ShellLayoutStore::ShellLayoutStore(std::filesystem::path layoutsDir, uimodel::LayoutDocumentLimits limits)
    : _layoutsDir{std::move(layoutsDir)}, _limits{limits}
  {
  }

  ShellLayoutStore::~ShellLayoutStore() = default;
  ShellLayoutStore::ShellLayoutStore(ShellLayoutStore&&) noexcept = default;
  ShellLayoutStore& ShellLayoutStore::operator=(ShellLayoutStore&&) noexcept = default;

  std::filesystem::path ShellLayoutStore::filePath(std::string_view presetId) const
  {
    if (presetId.empty() || presetId.contains('/') || presetId.contains('\\') || presetId.contains(".."))
    {
      ao::throwException<ao::Exception>("Invalid preset ID: path traversal attempt or empty ID");
    }

    return _layoutsDir / std::format("{}.yaml", presetId);
  }

  Result<std::optional<uimodel::LayoutDocument>> ShellLayoutStore::load(std::string_view presetId) const
  {
    auto const path = filePath(presetId);
    auto ec = std::error_code{};
    auto const exists = std::filesystem::exists(path, ec);

    if (ec)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to inspect shell layout file '{}': {}", path.string(), ec.message()));
    }

    if (!exists)
    {
      return std::nullopt;
    }

    auto store = rt::ConfigStore{path, rt::ConfigStore::OpenMode::ReadOnly, _limits.maxFileBytes};
    auto doc = uimodel::LayoutDocument{};

    auto const loaded = uimodel::loadLayout(store, "layout", doc);

    if (!loaded)
    {
      return std::unexpected{loaded.error()};
    }

    if (!*loaded)
    {
      return makeError(
        Error::Code::FormatRejected, std::format("Shell layout file '{}' has no 'layout' group", path.string()));
    }

    if (auto prepared = uimodel::prepareLayout(doc, _limits); !prepared)
    {
      return std::unexpected{prepared.error()};
    }

    return std::optional<uimodel::LayoutDocument>{std::move(doc)};
  }

  Result<> ShellLayoutStore::save(uimodel::LayoutDocument const& doc, std::string_view presetId)
  {
    if (auto prepared = uimodel::prepareLayout(doc, _limits); !prepared)
    {
      return std::unexpected{prepared.error()};
    }

    auto const path = filePath(presetId);
    auto ec = std::error_code{};
    auto const existed = std::filesystem::exists(path, ec);

    if (ec)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to inspect shell layout file '{}': {}", path.string(), ec.message()));
    }

    auto store = rt::ConfigStore{path, rt::ConfigStore::OpenMode::ReadWrite, _limits.maxFileBytes};

    if (existed)
    {
      auto previous = uimodel::LayoutDocument{};
      auto const loaded = uimodel::loadLayout(store, "layout", previous);

      if (!loaded)
      {
        return std::unexpected{loaded.error()};
      }

      if (!*loaded)
      {
        return makeError(
          Error::Code::FormatRejected, std::format("Shell layout file '{}' has no 'layout' group", path.string()));
      }

      if (auto prepared = uimodel::prepareLayout(previous, _limits); !prepared)
      {
        return std::unexpected{prepared.error()};
      }
    }

    return uimodel::saveLayout(store, "layout", doc);
  }

  Result<> ShellLayoutStore::remove(std::string_view presetId)
  {
    auto const path = filePath(presetId);
    auto ec = std::error_code{};

    std::filesystem::remove(path, ec);

    if (ec)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to remove shell layout file '{}': {}", path.string(), ec.message()));
    }

    return {};
  }
} // namespace ao::gtk
