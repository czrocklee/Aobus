// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutComponentStateStore.h"

#include <ao/Exception.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateYaml.h>
#include <ao/utility/AtomicFile.h>
#include <ao/yaml/Utils.h>

#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    void validatePresetId(std::string_view presetId)
    {
      if (presetId.empty() || presetId.find('/') != std::string_view::npos ||
          presetId.find('\\') != std::string_view::npos || presetId.find("..") != std::string_view::npos ||
          presetId.find('\0') != std::string_view::npos)
      {
        throwException<Exception>("Invalid preset ID: path traversal attempt or empty ID");
      }
    }
  } // namespace
  ShellLayoutComponentStateStore::ShellLayoutComponentStateStore(std::filesystem::path stateDir)
    : _stateDir{std::move(stateDir)}
  {
  }

  std::filesystem::path ShellLayoutComponentStateStore::filePath(std::string_view presetId) const
  {
    validatePresetId(presetId);
    return _stateDir / std::format("{}.yaml", presetId);
  }

  std::optional<uimodel::LayoutComponentStateDocument> ShellLayoutComponentStateStore::load(
    std::string_view presetId) const
  {
    auto const lock = std::scoped_lock{_mutex};
    return loadUnlocked(presetId);
  }

  std::optional<uimodel::LayoutComponentStateDocument> ShellLayoutComponentStateStore::loadUnlocked(
    std::string_view presetId) const
  {
    auto const path = filePath(presetId);

    try
    {
      auto buffer = yaml::readFile(path);
      auto const fileName = path.string();
      auto tree = ryml::Tree{yaml::callbacks(fileName.c_str())};
      ryml::parse_in_place(yaml::toSubstr(buffer), &tree);

      auto doc = uimodel::LayoutComponentStateDocument{};

      if (!yaml::read(tree.rootref(), doc))
      {
        APP_LOG_WARN("ShellLayoutComponentStateStore: Failed to decode state file ({})", path.string());
        return std::nullopt;
      }

      if (doc.preset != presetId)
      {
        APP_LOG_WARN("ShellLayoutComponentStateStore: Ignoring state file ({}) with mismatched preset '{}'",
                     path.string(),
                     doc.preset);
        return std::nullopt;
      }

      return doc;
    }
    catch (std::exception const& e)
    {
      // Missing files are normal (no runtime state yet); log others as warnings.
      if (std::filesystem::exists(path))
      {
        APP_LOG_WARN("ShellLayoutComponentStateStore: Failed to load state file ({}): {}", path.string(), e.what());
      }

      return std::nullopt;
    }
  }

  void ShellLayoutComponentStateStore::save(std::string_view presetId, uimodel::LayoutComponentStateDocument const& doc)
  {
    auto const lock = std::scoped_lock{_mutex};
    saveUnlocked(presetId, doc);
  }

  bool ShellLayoutComponentStateStore::saveUnlocked(std::string_view presetId,
                                                    uimodel::LayoutComponentStateDocument const& doc)
  {
    auto const path = filePath(presetId);
    auto stored = doc;
    stored.preset = presetId;

    try
    {
      auto tree = ryml::Tree{};
      yaml::write(tree.rootref(), stored);
      auto const text = ryml::emitrs_yaml<std::string>(tree);

      if (auto const result = utility::writeAtomically(path, text, utility::AtomicFilePermissions::OwnerReadWrite);
          !result)
      {
        APP_LOG_ERROR(
          "ShellLayoutComponentStateStore: Failed to save state file ({}): {}", path.string(), result.error().message);
        return false;
      }

      return true;
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("ShellLayoutComponentStateStore: Failed to save state file ({}): {}", path.string(), e.what());
      return false;
    }
  }

  bool ShellLayoutComponentStateStore::prune(std::string_view presetId, uimodel::LayoutDocument const& effectiveDoc)
  {
    auto const lock = std::scoped_lock{_mutex};

    auto doc = loadUnlocked(presetId).value_or(uimodel::LayoutComponentStateDocument{.preset = std::string{presetId}});
    auto const beforeCount = doc.components.size();
    uimodel::pruneComponentState(doc, effectiveDoc);
    auto const changed =
      doc.components.size() != beforeCount || (doc.components.empty() && std::filesystem::exists(filePath(presetId)));

    if (doc.components.empty())
    {
      removePresetUnlocked(presetId);
      return changed;
    }

    if (!changed)
    {
      return false;
    }

    return saveUnlocked(presetId, doc);
  }

  bool ShellLayoutComponentStateStore::removePreset(std::string_view presetId)
  {
    auto const lock = std::scoped_lock{_mutex};
    return removePresetUnlocked(presetId);
  }

  bool ShellLayoutComponentStateStore::removePresetUnlocked(std::string_view presetId)
  {
    auto const path = filePath(presetId);
    auto ec = std::error_code{};

    auto const existed = std::filesystem::exists(path, ec);
    std::filesystem::remove(path, ec);

    if (ec && existed)
    {
      APP_LOG_WARN("ShellLayoutComponentStateStore: Failed to remove state file ({}): {}", path.string(), ec.message());
      return false;
    }

    return true;
  }
} // namespace ao::gtk
