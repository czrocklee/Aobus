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

  Result<bool> ShellLayoutStore::isNewerFile(std::filesystem::path const& path) const
  {
    auto store = rt::ConfigStore{path, rt::ConfigStore::OpenMode::ReadOnly, _limits.maxFileBytes};
    auto doc = uimodel::LayoutDocument{};

    if (auto const loadedRes = uimodel::loadLayout(store, kShellLayoutConfigGroup, doc); !loadedRes)
    {
      if (store.hasNewerGroup(kShellLayoutConfigGroup))
      {
        return true;
      }

      // Only rejected content carries these codes. Any other failure, such as
      // a file this build cannot open or one above its size ceiling, leaves
      // the version unread. A file this build cannot parse counts as rejected
      // content, so a newer preset is kept only while it stays YAML this
      // build parses.
      if (auto const code = loadedRes.error().code;
          code != Error::Code::FormatRejected && code != Error::Code::NotSupported)
      {
        return std::unexpected{loadedRes.error()};
      }
    }

    return false;
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

    auto const loadedRes = uimodel::loadLayout(store, kShellLayoutConfigGroup, doc);

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

  bool ShellLayoutStore::isNewer(std::string_view const presetId) const
  {
    if (!_hasLocation)
    {
      return false;
    }

    auto const path = filePath(presetId);
    auto ec = std::error_code{};
    return std::filesystem::exists(path, ec) && !ec && isNewerFile(path).value_or(false);
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
      auto const loadedRes = uimodel::loadLayout(store, kShellLayoutConfigGroup, previous);

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

    return uimodel::saveLayout(store, kShellLayoutConfigGroup, doc);
  }

  Result<> ShellLayoutStore::remove(std::string_view presetId)
  {
    if (!_hasLocation)
    {
      return {};
    }

    auto const path = filePath(presetId);
    auto ec = std::error_code{};

    // An older build never removes a preset a newer version saved (decision
    // 0020), nor one whose bytes it cannot read. A preset it reads but rejects
    // for any other reason, including one it cannot parse, stays removable.
    if (std::filesystem::exists(path, ec) && !ec)
    {
      auto const newerRes = isNewerFile(path);

      if (!newerRes)
      {
        return std::unexpected{newerRes.error()};
      }

      if (*newerRes)
      {
        return makeError(Error::Code::NotSupported,
                         std::format("Shell layout file '{}' was saved by a newer version of Aobus; it is kept",
                                     utility::pathToUtf8(path)));
      }
    }

    if (ec)
    {
      return makeError(
        Error::Code::IoError,
        std::format("Failed to inspect shell layout file '{}': {}", utility::pathToUtf8(path), ec.message()));
    }

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
