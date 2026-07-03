// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/Exception.h>

#include <c4/std/string_view.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::yaml
{
  class CallbackContext final
  {
  public:
    explicit CallbackContext(std::string filename = "<buffer>")
      : _filename{std::move(filename)}
    {
    }

    std::string const& filename() const noexcept { return _filename; }

  private:
    std::string _filename;
  };

  inline void throwOnErrorWithContext(c4::basic_substring<char const> msg,
                                      c4::yml::ErrorDataBasic const& dat,
                                      void* userData)
  {
    auto const* const context = userData != nullptr ? static_cast<CallbackContext const*>(userData) : nullptr;
    auto const filename = context != nullptr ? context->filename() : std::string{"<buffer>"};
    throwException<Exception>("YAML error at {}:{}:{}: {}",
                              filename,
                              dat.location.line,
                              dat.location.col,
                              std::string_view{msg.data(), msg.size()});
  }

  inline void throwOnParseErrorWithContext(c4::basic_substring<char const> msg,
                                           c4::yml::ErrorDataParse const& dat,
                                           void* userData)
  {
    auto const* const context = userData != nullptr ? static_cast<CallbackContext const*>(userData) : nullptr;
    auto const filename = context != nullptr ? context->filename() : std::string{"<buffer>"};
    throwException<Exception>("YAML parse error at {}:{}:{}: {}",
                              filename,
                              dat.ymlloc.line,
                              dat.ymlloc.col,
                              std::string_view{msg.data(), msg.size()});
  }

  inline void throwOnVisitErrorWithContext(c4::basic_substring<char const> msg,
                                           c4::yml::ErrorDataVisit const& dat,
                                           void* userData)
  {
    auto const* const context = userData != nullptr ? static_cast<CallbackContext const*>(userData) : nullptr;
    auto const filename = context != nullptr ? context->filename() : std::string{"<buffer>"};
    throwException<Exception>("YAML visit error at {}:{}:{}: {}",
                              filename,
                              dat.cpploc.line,
                              dat.cpploc.col,
                              std::string_view{msg.data(), msg.size()});
  }

  inline ryml::Callbacks callbacks()
  {
    auto callbacks = ryml::Callbacks{};
    callbacks.set_error_basic(throwOnErrorWithContext);
    callbacks.set_error_parse(throwOnParseErrorWithContext);
    callbacks.set_error_visit(throwOnVisitErrorWithContext);
    return callbacks;
  }

  inline ryml::Callbacks callbacks(CallbackContext& context)
  {
    auto callbacks = ryml::Callbacks{};
    callbacks.set_user_data(&context);
    callbacks.set_error_basic(throwOnErrorWithContext);
    callbacks.set_error_parse(throwOnParseErrorWithContext);
    callbacks.set_error_visit(throwOnVisitErrorWithContext);
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

  inline void parseInPlace(ryml::Tree& tree, std::vector<char>& buffer, CallbackContext& context)
  {
    tree.callbacks(callbacks(context));
    ryml::parse_in_place(toCsubstr(context.filename()), toSubstr(buffer), &tree);
  }

  inline void parseInArena(ryml::Tree& tree, std::string_view source, CallbackContext& context)
  {
    tree.callbacks(callbacks(context));
    ryml::parse_in_arena(toCsubstr(context.filename()), toCsubstr(source), &tree);
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
  inline Result<std::vector<char>> readFileResult(std::filesystem::path const& path)
  {
    auto ifs = std::ifstream{path, std::ios::binary | std::ios::ate};

    if (!ifs)
    {
      return makeError(Error::Code::IoError, "Failed to open file: " + path.string());
    }

    auto const end = ifs.tellg();

    if (end == std::streampos{-1})
    {
      return makeError(Error::Code::IoError, "Failed to inspect file size: " + path.string());
    }

    ifs.seekg(0, std::ios::beg);

    if (!ifs)
    {
      return makeError(Error::Code::IoError, "Failed to seek file: " + path.string());
    }

    auto const size = static_cast<std::size_t>(end);
    auto buffer = std::vector<char>(size);

    if (!ifs.read(buffer.data(), static_cast<std::streamsize>(size)))
    {
      return makeError(Error::Code::IoError, "Failed to read file: " + path.string());
    }

    return buffer;
  }

  /**
   * @brief Throwing compatibility wrapper around readFileResult().
   */
  inline std::vector<char> readFile(std::filesystem::path const& path)
  {
    auto result = readFileResult(path);

    if (!result)
    {
      throwException<Exception>(std::string_view{result.error().message}, result.error().location);
    }

    return std::move(*result);
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

    auto parsed = T{};
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);

    if (ec != std::errc{} || ptr != text.data() + text.size())
    {
      return false;
    }

    value = parsed;
    return true;
  }

  inline bool tryParseScalar(std::string_view text, bool& value) noexcept
  {
    if (text == "true")
    {
      value = true;
      return true;
    }

    if (text == "false")
    {
      value = false;
      return true;
    }

    return false;
  }

  inline bool tryReadScalar(ryml::ConstNodeRef const& node, std::string_view& value) noexcept
  {
    if (!node.has_val())
    {
      return false;
    }

    value = scalarView(node);
    return true;
  }

  inline bool tryReadScalar(ryml::ConstNodeRef const& node, std::string& value)
  {
    auto view = std::string_view{};

    if (!tryReadScalar(node, view))
    {
      return false;
    }

    value = view;
    return true;
  }

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
    if (T value = {}; tryReadScalar(node, value))
    {
      return value;
    }

    return makeError(Error::Code::FormatRejected, std::string{context} + " must be a valid scalar");
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
    return tryReadScalar(node, val) ? val : defaultValue;
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

    auto val = T{};
    return tryReadScalar(node, val) ? val : defaultValue;
  }
} // namespace ao::yaml
