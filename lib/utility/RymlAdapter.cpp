// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/yaml/RymlAdapter.h>

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>

#include <c4/substr.hpp>
#include <ryml.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::yaml
{
  ErrorCallbackState::ErrorCallbackState(std::string filename)
    : _filename{std::move(filename)}
  {
  }

  std::string const& ErrorCallbackState::filename() const noexcept
  {
    return _filename;
  }

  void throwOnErrorWithContext(c4::basic_substring<char const> msg, c4::yml::ErrorDataBasic const& dat, void* userData)
  {
    auto const* const state = userData != nullptr ? static_cast<ErrorCallbackState const*>(userData) : nullptr;
    auto const filename = state != nullptr ? state->filename() : std::string{"<buffer>"};
    throwException<Exception>("YAML error at {}:{}:{}: {}",
                              filename,
                              dat.location.line,
                              dat.location.col,
                              std::string_view{msg.data(), msg.size()});
  }

  void throwOnParseErrorWithContext(c4::basic_substring<char const> msg,
                                    c4::yml::ErrorDataParse const& dat,
                                    void* userData)
  {
    auto const* const state = userData != nullptr ? static_cast<ErrorCallbackState const*>(userData) : nullptr;
    auto const filename = state != nullptr ? state->filename() : std::string{"<buffer>"};
    throwException<Exception>("YAML parse error at {}:{}:{}: {}",
                              filename,
                              dat.ymlloc.line,
                              dat.ymlloc.col,
                              std::string_view{msg.data(), msg.size()});
  }

  void throwOnVisitErrorWithContext(c4::basic_substring<char const> msg,
                                    c4::yml::ErrorDataVisit const& dat,
                                    void* userData)
  {
    auto const* const state = userData != nullptr ? static_cast<ErrorCallbackState const*>(userData) : nullptr;
    auto const filename = state != nullptr ? state->filename() : std::string{"<buffer>"};
    throwException<Exception>("YAML visit error at {}:{}:{}: {}",
                              filename,
                              dat.cpploc.line,
                              dat.cpploc.col,
                              std::string_view{msg.data(), msg.size()});
  }

  ryml::Callbacks callbacks()
  {
    auto result = ryml::Callbacks{};
    result.set_error_basic(throwOnErrorWithContext);
    result.set_error_parse(throwOnParseErrorWithContext);
    result.set_error_visit(throwOnVisitErrorWithContext);
    return result;
  }

  ryml::Callbacks callbacks(ErrorCallbackState& state)
  {
    auto result = callbacks();
    result.set_user_data(&state);
    return result;
  }

  ryml::csubstr toCsubstr(std::string_view value) noexcept
  {
    return {value.data(), value.size()};
  }

  ryml::substr toSubstr(std::vector<char>& buffer) noexcept
  {
    return {buffer.data(), buffer.size()};
  }

  void parseInPlace(ryml::Tree& tree, std::vector<char>& buffer, ErrorCallbackState& state)
  {
    tree.callbacks(callbacks(state));
    ryml::parse_in_place(toCsubstr(state.filename()), toSubstr(buffer), &tree);
  }

  void parseInArena(ryml::Tree& tree, std::string_view source, ErrorCallbackState& state)
  {
    tree.callbacks(callbacks(state));
    ryml::parse_in_arena(toCsubstr(state.filename()), toCsubstr(source), &tree);
  }

  ryml::csubstr copyToArena(ryml::Tree& tree, std::string_view value)
  {
    return tree.to_arena(toCsubstr(value));
  }

  ryml::csubstr copyToArena(ryml::NodeRef node, std::string_view value)
  {
    return node.tree()->to_arena(toCsubstr(value));
  }

  ryml::ConstNodeRef findChild(ryml::ConstNodeRef node, std::string_view key) noexcept
  {
    return node.find_child(toCsubstr(key));
  }

  ryml::NodeRef findChild(ryml::NodeRef node, std::string_view key) noexcept
  {
    return node.find_child(toCsubstr(key));
  }

  void setKey(ryml::NodeRef node, std::string_view key)
  {
    node.set_key(copyToArena(node, key));
  }

  void setValue(ryml::NodeRef node, std::string_view value)
  {
    node.set_val(copyToArena(node, value));
  }

  std::string boundedErrorContext(std::string_view context)
  {
    if (context.size() <= kMaximumErrorContextBytes)
    {
      return std::string{context};
    }

    constexpr auto kSuffix = std::string_view{"..."};
    auto result = std::string{};
    result.reserve(kMaximumErrorContextBytes);
    result.append(context.substr(0, kMaximumErrorContextBytes - kSuffix.size()));
    result.append(kSuffix);
    return result;
  }

  Result<std::vector<char>> readFileResult(std::filesystem::path const& path, std::optional<std::size_t> optMaxBytes)
  {
    auto ifs = std::ifstream{path, std::ios::binary | std::ios::ate};

    if (!ifs)
    {
      return makeError(Error::Code::IoError, "Failed to open file: " + path.string());
    }

    auto const endOffset = static_cast<std::streamoff>(ifs.tellg());

    if (endOffset < 0)
    {
      return makeError(Error::Code::IoError, "Failed to inspect file size: " + path.string());
    }

    auto const unsignedSize = static_cast<std::uintmax_t>(endOffset);

    if (!std::in_range<std::size_t>(unsignedSize) || !std::in_range<std::streamsize>(unsignedSize))
    {
      return makeError(Error::Code::ValueTooLarge, "File is too large to read: " + path.string());
    }

    auto const size = static_cast<std::size_t>(unsignedSize);
    ifs.seekg(0, std::ios::beg);

    if (!ifs)
    {
      return makeError(Error::Code::IoError, "Failed to seek file: " + path.string());
    }

    if (optMaxBytes && size > *optMaxBytes)
    {
      return makeError(Error::Code::ValueTooLarge,
                       "File '" + path.string() + "' is " + std::to_string(size) + " bytes; maximum allowed is " +
                         std::to_string(*optMaxBytes));
    }

    auto buffer = std::vector<char>(size);

    if (!ifs.read(buffer.data(), static_cast<std::streamsize>(size)))
    {
      return makeError(Error::Code::IoError, "Failed to read file: " + path.string());
    }

    return buffer;
  }

  std::vector<char> readFile(std::filesystem::path const& path)
  {
    auto result = readFileResult(path);

    if (!result)
    {
      throwException<Exception>(std::string_view{result.error().message}, result.error().location);
    }

    return std::move(*result);
  }

  std::string_view scalarView(ryml::ConstNodeRef const& node)
  {
    if (!node.has_val())
    {
      return {};
    }

    auto const val = node.val();
    return {val.data(), val.size()};
  }

  std::string_view keyView(ryml::ConstNodeRef const& node)
  {
    if (!node.has_key())
    {
      return {};
    }

    auto const key = node.key();
    return {key.data(), key.size()};
  }

  bool tryParseScalar(std::string_view text, bool& value) noexcept
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

  bool tryReadScalar(ryml::ConstNodeRef const& node, std::string_view& value) noexcept
  {
    if (!node.has_val() || node.val_is_null())
    {
      return false;
    }

    value = scalarView(node);
    return true;
  }

  bool tryReadScalar(ryml::ConstNodeRef const& node, std::string& value)
  {
    auto view = std::string_view{};

    if (!tryReadScalar(node, view))
    {
      return false;
    }

    value = view;
    return true;
  }

  bool asBool(ryml::ConstNodeRef const& node, bool defaultValue)
  {
    if (!node.has_val())
    {
      return defaultValue;
    }

    bool value = false;
    return tryReadScalar(node, value) ? value : defaultValue;
  }
} // namespace ao::yaml
