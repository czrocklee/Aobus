// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutStore.h"

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/utility/Path.h>

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

  ShellLayoutStore::ShellLayoutStore(rt::ConfigStore::NoLocation /*noLocation*/, uimodel::LayoutDocumentLimits limits)
    : _limits{std::move(limits)}, _hasLocation{false}
  {
  }

  ShellLayoutStore::~ShellLayoutStore() = default;
  ShellLayoutStore::ShellLayoutStore(ShellLayoutStore&&) noexcept = default;
  ShellLayoutStore& ShellLayoutStore::operator=(ShellLayoutStore&&) noexcept = default;

  std::filesystem::path ShellLayoutStore::filePath(std::string_view presetId) const
  {
    AO_EXPECTS(!(presetId.empty() || presetId.contains('/') || presetId.contains('\\') || presetId.contains("..")),
               "Invalid preset ID: path traversal attempt or empty ID");

    return _layoutsDir / utility::pathFromUtf8(std::format("{}.yaml", presetId));
  }

  Result<std::optional<uimodel::LayoutDocument>> ShellLayoutStore::load(std::string_view presetId) const
  {
    if (!_hasLocation)
    {
      return std::nullopt;
    }

    auto const path = filePath(presetId);
    auto ec = std::error_code{};
    auto const exists = std::filesystem::exists(path, ec);

    if (ec)
    {
      return makeError(
        Error::Code::IoError,
        std::format("Failed to inspect shell layout file '{}': {}", utility::pathToUtf8(path), ec.message()));
    }

    if (!exists)
    {
      return std::nullopt;
    }

    auto store = rt::ConfigStore{path, rt::ConfigStore::OpenMode::ReadOnly, _limits.maxFileBytes};
    auto doc = uimodel::LayoutDocument{};

    auto const loadedRes = uimodel::loadLayout(store, "layout", doc);

    if (!loadedRes)
    {
      return std::unexpected{loadedRes.error()};
    }

    if (!*loadedRes)
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Shell layout file '{}' has no 'layout' group", utility::pathToUtf8(path)));
    }

    if (auto preparedRes = uimodel::prepareLayout(doc, _limits); !preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    return std::optional<uimodel::LayoutDocument>{std::move(doc)};
  }

  Result<> ShellLayoutStore::save(uimodel::LayoutDocument const& doc, std::string_view presetId)
  {
    if (auto preparedRes = uimodel::prepareLayout(doc, _limits); !preparedRes)
    {
      // A document that cannot be prepared is rejected wherever it was going,
      // so this check stays ahead of the no-location exit.
      return std::unexpected{preparedRes.error()};
    }

    if (!_hasLocation)
    {
      return {};
    }

    auto const path = filePath(presetId);
    auto ec = std::error_code{};
    auto const existed = std::filesystem::exists(path, ec);

    if (ec)
    {
      return makeError(
        Error::Code::IoError,
        std::format("Failed to inspect shell layout file '{}': {}", utility::pathToUtf8(path), ec.message()));
    }

    auto store = rt::ConfigStore{path, rt::ConfigStore::OpenMode::ReadWrite, _limits.maxFileBytes};

    if (existed)
    {
      auto previous = uimodel::LayoutDocument{};
      auto const loadedRes = uimodel::loadLayout(store, "layout", previous);

      if (!loadedRes)
      {
        return std::unexpected{loadedRes.error()};
      }

      if (!*loadedRes)
      {
        return makeError(Error::Code::FormatRejected,
                         std::format("Shell layout file '{}' has no 'layout' group", utility::pathToUtf8(path)));
      }

      if (auto preparedRes = uimodel::prepareLayout(previous, _limits); !preparedRes)
      {
        return std::unexpected{preparedRes.error()};
      }
    }

    return uimodel::saveLayout(store, "layout", doc);
  }

  Result<> ShellLayoutStore::remove(std::string_view presetId)
  {
    if (!_hasLocation)
    {
      return {};
    }

    auto const path = filePath(presetId);
    auto ec = std::error_code{};

    std::filesystem::remove(path, ec);

    if (ec)
    {
      return makeError(
        Error::Code::IoError,
        std::format("Failed to remove shell layout file '{}': {}", utility::pathToUtf8(path), ec.message()));
    }

    return {};
  }
} // namespace ao::gtk
