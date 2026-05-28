// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Exception.h>

#include <c4/std/string_view.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string_view>
#include <vector>

namespace ao::rt::yaml
{
  /**
   * @brief Custom error callback for ryml that throws ao::Exception.
   */
  inline void throwOnError(c4::basic_substring<char const> msg, c4::yml::ErrorDataBasic const& dat, void* userData)
  {
    auto const* const filename = userData != nullptr ? static_cast<char const*>(userData) : "unknown";
    throw Exception{std::format("YAML error at {}:{}:{}: {}",
                                filename,
                                dat.location.line,
                                dat.location.col,
                                std::string_view{msg.data(), msg.size()}),
                    __FILE__,
                    __LINE__};
  }

  inline ryml::Callbacks callbacks(char const* filename = nullptr)
  {
    auto callbacks = ryml::Callbacks{};
    callbacks.set_user_data(
      static_cast<void*>(const_cast<char*>(filename))); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    callbacks.set_error_basic(throwOnError);
    return callbacks;
  }

  inline ryml::csubstr toCsubstr(std::string_view value) noexcept
  {
    return ryml::csubstr{value.data(), value.size()};
  }

  inline ryml::substr toSubstr(std::vector<char>& buffer) noexcept
  {
    return ryml::substr{buffer.data(), buffer.size()};
  }

  inline ryml::csubstr copyToArena(ryml::Tree& tree, std::string_view value)
  {
    return tree.to_arena(toCsubstr(value));
  }

  inline ryml::csubstr copyToArena(ryml::NodeRef node, std::string_view value)
  {
    return node.tree()->to_arena(toCsubstr(value));
  }

  inline ryml::ConstNodeRef findChild(ryml::ConstNodeRef node, std::string_view key) noexcept
  {
    return node.find_child(toCsubstr(key));
  }

  inline ryml::NodeRef findChild(ryml::NodeRef node, std::string_view key) noexcept
  {
    return node.find_child(toCsubstr(key));
  }

  inline void setKey(ryml::NodeRef node, std::string_view key)
  {
    node.set_key(copyToArena(node, key));
  }

  inline void setValue(ryml::NodeRef node, std::string_view value)
  {
    node.set_val(copyToArena(node, value));
  }

  /**
   * @brief Reads a file into a buffer suitable for ryml::parse_in_place.
   */
  inline std::vector<char> readFile(std::filesystem::path const& path)
  {
    auto ifs = std::ifstream{path, std::ios::binary | std::ios::ate};

    if (!ifs)
    {
      throwException<Exception>("Failed to open file: {}", path.string());
    }

    auto const size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    auto buffer = std::vector<char>(size);

    if (!ifs.read(buffer.data(), size))
    {
      throwException<Exception>("Failed to read file: {}", path.string());
    }

    return buffer;
  }

  /**
   * @brief Returns a std::string_view for a ryml node's scalar value.
   */
  inline std::string_view scalarView(ryml::ConstNodeRef const& node)
  {
    if (!node.has_val())
    {
      return {};
    }

    auto const val = node.val();
    return {val.data(), val.size()};
  }

  /**
   * @brief Returns a std::string_view for a ryml node's key.
   */
  inline std::string_view keyView(ryml::ConstNodeRef const& node)
  {
    if (!node.has_key())
    {
      return {};
    }

    auto const keyStr = node.key();
    return {keyStr.data(), keyStr.size()};
  }

  /**
   * @brief Parses a boolean from a ryml node.
   */
  inline bool asBool(ryml::ConstNodeRef const& node, bool defaultValue = false)
  {
    if (!node.has_val())
    {
      return defaultValue;
    }

    bool val = false;
    node >> val;
    return val;
  }

  /**
   * @brief Parses an integer from a ryml node.
   */
  template<typename T>
  inline T asInt(ryml::ConstNodeRef const& node, T defaultValue = 0)
  {
    if (!node.has_val())
    {
      return defaultValue;
    }

    T val;
    node >> val;
    return val;
  }
} // namespace ao::rt::yaml
