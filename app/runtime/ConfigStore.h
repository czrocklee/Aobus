// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Error.h"
#include "runtime/yaml/ConfigTraits.h" // IWYU pragma: export
#include "runtime/yaml/Utils.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class ConfigStore final
  {
  public:
    enum class OpenMode : std::uint8_t
    {
      ReadWrite, // file may not exist yet, will be created on flush
      ReadOnly,  // file must already exist, NotFound is an error for load()
    };

    ~ConfigStore() = default;

    ConfigStore(ConfigStore const&) = delete;
    ConfigStore& operator=(ConfigStore const&) = delete;
    ConfigStore(ConfigStore&&) = delete;
    ConfigStore& operator=(ConfigStore&&) = delete;

    explicit ConfigStore(std::filesystem::path filePath, OpenMode mode = OpenMode::ReadWrite);

    Result<> flush();

    template<typename T>
    void save(std::string_view group, T const& obj)
    {
      if (_mode == OpenMode::ReadOnly)
      {
        throw std::logic_error{"save() called on ReadOnly ConfigStore"};
      }

      if (auto const result = ensureLoaded(); !result && result.error().code != Error::Code::NotFound)
      {
        return;
      }

      if (!_root.is_map(0))
      {
        _root.clear();
        _root.to_map(0);
      }

      auto const groupName = yaml::toCsubstr(group);
      auto child = _root.rootref()[groupName];

      if (!child.readable())
      {
        child = _root.rootref().append_child();
        yaml::setKey(child, group);
      }
      else
      {
        child.clear_children();
      }

      yaml::write(child, obj);
    }

    template<typename T>
    Result<> load(std::string_view group, T& obj)
    {
      if (auto const result = ensureLoaded(); !result)
      {
        return result;
      }

      if (_root.is_map(0))
      {
        if (auto const child = _root.rootref()[yaml::toCsubstr(group)]; child.readable())
        {
          try
          {
            if (!yaml::read(child, obj))
            {
              return makeError(Error::Code::FormatRejected, std::format("Failed to decode config key '{}'", group));
            }
          }
          catch (std::exception const& e)
          {
            return makeError(
              Error::Code::FormatRejected, std::format("Failed to decode config key '{}': {}", group, e.what()));
          }
        }
      }

      return {};
    }

  private:
    Result<> ensureLoaded();

    std::filesystem::path _filePath;
    OpenMode _mode = OpenMode::ReadWrite;
    ryml::Tree _root;
    std::vector<char> _inputBuffer;
    bool _loaded = false;
  };
}
