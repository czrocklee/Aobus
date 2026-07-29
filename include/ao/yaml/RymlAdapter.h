// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <c4/std/string_view.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace ao::yaml
{
  inline constexpr std::size_t kMaximumErrorContextBytes = 160;

  class ErrorCallbackState final
  {
  public:
    explicit ErrorCallbackState(std::string filename = "<buffer>");

    std::string const& filename() const noexcept;

  private:
    std::string _filename;
  };

  void throwOnErrorWithContext(c4::basic_substring<char const> msg, c4::yml::ErrorDataBasic const& dat, void* userData);
  void throwOnParseErrorWithContext(c4::basic_substring<char const> msg,
                                    c4::yml::ErrorDataParse const& dat,
                                    void* userData);
  void throwOnVisitErrorWithContext(c4::basic_substring<char const> msg,
                                    c4::yml::ErrorDataVisit const& dat,
                                    void* userData);

  ryml::Callbacks callbacks();
  ryml::Callbacks callbacks(ErrorCallbackState& state);

  ryml::csubstr toCsubstr(std::string_view value) noexcept;
  ryml::substr toSubstr(std::vector<char>& buffer) noexcept;
  void parseInPlace(ryml::Tree& tree, std::vector<char>& buffer, ErrorCallbackState& state);
  void parseInArena(ryml::Tree& tree, std::string_view source, ErrorCallbackState& state);
  ryml::csubstr copyToArena(ryml::Tree& tree, std::string_view value);
  ryml::csubstr copyToArena(ryml::NodeRef node, std::string_view value);
  ryml::ConstNodeRef findChild(ryml::ConstNodeRef node, std::string_view key) noexcept;
  ryml::NodeRef findChild(ryml::NodeRef node, std::string_view key) noexcept;
  void setKey(ryml::NodeRef node, std::string_view key);
  void setValue(ryml::NodeRef node, std::string_view value);
  std::string boundedErrorContext(std::string_view context);

  /**
   * @brief Reads a file into a buffer suitable for ryml::parse_in_place.
   */
  Result<std::vector<char>> readFileResult(std::filesystem::path const& path,
                                           std::optional<std::size_t> optMaxBytes = std::nullopt);

  /**
   * @brief Throwing compatibility wrapper around readFileResult().
   */
  std::vector<char> readFile(std::filesystem::path const& path);

  /**
   * @brief Returns a std::string_view for a ryml node's scalar value.
   */
  std::string_view scalarView(ryml::ConstNodeRef const& node);

  /**
   * @brief Returns a std::string_view for a ryml node's key.
   */
  std::string_view keyView(ryml::ConstNodeRef const& node);

  template<std::integral T>
    requires(!std::same_as<T, bool>)
  inline bool tryParseScalar(std::string_view text, T& value)
  {
    if (text.empty())
    {
      return false;
    }

    if constexpr (std::signed_integral<T>)
    {
      std::int64_t parsed = 0;
      auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);

      if (ec != std::errc{} || ptr != text.data() + text.size() ||
          parsed < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
          parsed > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
      {
        return false;
      }

      value = static_cast<T>(parsed);
      return true;
    }
    else
    {
      std::uint64_t parsed = 0;
      auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);

      if (ec != std::errc{} || ptr != text.data() + text.size() ||
          parsed > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
      {
        return false;
      }

      value = static_cast<T>(parsed);
      return true;
    }
  }

  template<std::floating_point T>
  inline bool tryParseScalar(std::string_view text, T& value)
  {
    if (text.empty())
    {
      return false;
    }

    T parsed = {};
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);

    if (ec != std::errc{} || ptr != text.data() + text.size())
    {
      return false;
    }

    value = parsed;
    return true;
  }

  bool tryParseScalar(std::string_view text, bool& value) noexcept;
  bool tryReadScalar(ryml::ConstNodeRef const& node, std::string_view& value) noexcept;
  bool tryReadScalar(ryml::ConstNodeRef const& node, std::string& value);

  template<typename T>
    requires(std::is_arithmetic_v<T>)
  inline bool tryReadScalar(ryml::ConstNodeRef const& node, T& value)
  {
    if (!node.has_val())
    {
      return false;
    }

    T parsed = {};

    if (!tryParseScalar(scalarView(node), parsed))
    {
      return false;
    }

    value = parsed;
    return true;
  }

  template<typename T>
  inline Result<T> scalarAs(ryml::ConstNodeRef const& node, std::string_view context)
  {
    auto parsed = Result<T>{std::in_place};

    if (!tryReadScalar(node, *parsed))
    {
      return makeError(Error::Code::FormatRejected, boundedErrorContext(context) + " must be a valid scalar");
    }

    return parsed;
  }

  /**
   * @brief Parses a boolean from a ryml node.
   */
  bool asBool(ryml::ConstNodeRef const& node, bool defaultValue = false);

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

    auto val = T{};
    return tryReadScalar(node, val) ? val : defaultValue;
  }
} // namespace ao::yaml
