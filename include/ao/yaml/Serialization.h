// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/yaml/RymlAdapter.h>

#include <c4/format.hpp>
#include <ryml.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::yaml
{
  enum class UnknownKeyPolicy : std::uint8_t
  {
    Reject,
    Allow,
  };

  inline std::string fieldContext(std::string_view context, std::string_view field)
  {
    auto result = std::string{};
    result.reserve(kMaximumErrorContextBytes);
    bool truncated = false;

    auto const append = [&result, &truncated](std::string_view part)
    {
      auto const remaining = kMaximumErrorContextBytes - result.size();
      auto const count = std::min(remaining, part.size());
      result.append(part.substr(0, count));
      truncated = truncated || count != part.size();
    };

    append(context);

    if (!context.empty())
    {
      if (result.size() < kMaximumErrorContextBytes)
      {
        result.push_back('.');
      }
      else
      {
        truncated = true;
      }
    }

    append(field);

    if (truncated)
    {
      constexpr auto kSuffix = std::string_view{"..."};
      result.resize(kMaximumErrorContextBytes - kSuffix.size());
      result.append(kSuffix);
    }

    return result;
  }

  inline Result<> requireMap(ryml::ConstNodeRef node, std::string_view context)
  {
    if (!node.is_map())
    {
      return makeError(Error::Code::FormatRejected, boundedErrorContext(context) + " must be a mapping");
    }

    return {};
  }

  inline Result<> requireSequence(ryml::ConstNodeRef node, std::string_view context)
  {
    if (!node.is_seq())
    {
      return makeError(Error::Code::FormatRejected, boundedErrorContext(context) + " must be a sequence");
    }

    return {};
  }

  inline Result<> validateMapKeys(ryml::ConstNodeRef node,
                                  std::span<std::string_view const> allowedKeys,
                                  std::string_view context,
                                  UnknownKeyPolicy unknownKeyPolicy = UnknownKeyPolicy::Reject)
  {
    if (auto const result = requireMap(node, context); !result)
    {
      return result;
    }

    auto seenKeys = std::vector<std::string_view>{};
    seenKeys.reserve(node.num_children());

    for (auto const& child : node.children())
    {
      if (!child.has_key())
      {
        return makeError(
          Error::Code::FormatRejected, boundedErrorContext(context) + " contains an entry without a key");
      }

      auto const key = keyView(child);

      if (std::ranges::contains(seenKeys, key))
      {
        return makeError(Error::Code::FormatRejected, fieldContext(context, key) + " appears more than once");
      }

      seenKeys.push_back(key);

      if (unknownKeyPolicy == UnknownKeyPolicy::Reject && !std::ranges::contains(allowedKeys, key))
      {
        return makeError(Error::Code::FormatRejected, fieldContext(context, key) + " is not supported");
      }
    }

    return {};
  }

  inline Result<ryml::ConstNodeRef> requireChild(ryml::ConstNodeRef node,
                                                 std::string_view key,
                                                 std::string_view context)
  {
    auto const child = findChild(node, key);

    if (!child.readable())
    {
      return makeError(Error::Code::FormatRejected, fieldContext(context, key) + " is required");
    }

    return child;
  }

  inline ryml::NodeRef appendChild(ryml::NodeRef node, std::string_view key)
  {
    auto child = node.append_child();
    setKey(child, key);
    return child;
  }

  inline void writeScalar(ryml::NodeRef node, std::string_view value)
  {
    setValue(node, value);
    node.set_val_style(ryml::VAL_DQUO);
  }

  inline void writeScalar(ryml::NodeRef node, std::string const& value)
  {
    writeScalar(node, std::string_view{value});
  }

  inline void writeScalar(ryml::NodeRef node, bool value)
  {
    node << c4::fmt::boolalpha(value);
  }

  template<typename T>
    requires(std::is_arithmetic_v<T> && !std::same_as<T, bool>)
  inline void writeScalar(ryml::NodeRef node, T value)
  {
    node << value;
  }

  template<typename T, typename ElementReader>
  Result<std::vector<T>> readSequence(ryml::ConstNodeRef node,
                                      std::string_view context,
                                      ElementReader const& elementReader);

  template<typename Range, typename ElementWriter>
  Result<> writeSequence(ryml::NodeRef node, Range const& values, ElementWriter const& elementWriter);

  template<typename T>
  Result<std::vector<T>> readScalarSequence(ryml::ConstNodeRef node, std::string_view context);

  template<typename Range>
  Result<> writeScalarSequence(ryml::NodeRef node, Range const& values);

  class MapWriter final
  {
  public:
    explicit MapWriter(ryml::NodeRef node)
      : _node{node}
    {
      _node |= ryml::MAP;
    }

    template<typename T>
    MapWriter& scalar(std::string_view key, T const& value)
    {
      if (_result)
      {
        writeScalar(appendChild(_node, key), value);
      }

      return *this;
    }

    template<typename T, typename Writer>
    MapWriter& value(std::string_view key, T const& value, Writer const& writer)
    {
      if (_result)
      {
        _result = writer(appendChild(_node, key), value);
      }

      return *this;
    }

    template<typename Range, typename ElementWriter>
    MapWriter& sequence(std::string_view key, Range const& values, ElementWriter const& elementWriter)
    {
      return value(key,
                   values,
                   [&elementWriter](ryml::NodeRef child, Range const& sequenceValues)
                   { return writeSequence(child, sequenceValues, elementWriter); });
    }

    template<typename Range>
    MapWriter& scalarSequence(std::string_view key, Range const& values)
    {
      return value(key,
                   values,
                   [](ryml::NodeRef child, Range const& sequenceValues)
                   { return writeScalarSequence(child, sequenceValues); });
    }

    Result<> finish() && { return std::move(_result); }

  private:
    ryml::NodeRef _node;
    Result<> _result;
  };

  template<typename T>
  inline Result<T> requireScalar(ryml::ConstNodeRef node, std::string_view key, std::string_view context)
  {
    auto child = requireChild(node, key, context);

    if (!child)
    {
      return std::unexpected{child.error()};
    }

    return scalarAs<T>(*child, fieldContext(context, key));
  }

  class MapReader final
  {
  public:
    MapReader(ryml::ConstNodeRef node,
              std::span<std::string_view const> allowedKeys,
              std::string_view context,
              UnknownKeyPolicy unknownKeyPolicy = UnknownKeyPolicy::Reject)
      : _node{node}
      , _context{boundedErrorContext(context)}
      , _result{validateMapKeys(node, allowedKeys, _context, unknownKeyPolicy)}
    {
    }

    template<typename T>
    MapReader& requiredScalar(std::string_view key, T& destination)
    {
      return requiredValue(key,
                           destination,
                           [](ryml::ConstNodeRef child, std::string_view context)
                           { return scalarAs<T>(child, context); });
    }

    template<typename T, typename Reader>
    MapReader& requiredValue(std::string_view key, T& destination, Reader const& reader)
    {
      if (_result)
      {
        if (auto child = requireChild(_node, key, _context); !child)
        {
          _result = std::unexpected{std::move(child.error())};
        }
        else
        {
          assign(reader(*child, fieldContext(_context, key)), destination);
        }
      }

      return *this;
    }

    template<typename T, typename ElementReader>
    MapReader& requiredSequence(std::string_view key, std::vector<T>& destination, ElementReader const& elementReader)
    {
      return requiredValue(key,
                           destination,
                           [&elementReader](ryml::ConstNodeRef child, std::string_view context)
                           { return readSequence<T>(child, context, elementReader); });
    }

    template<typename T>
    MapReader& requiredScalarSequence(std::string_view key, std::vector<T>& destination)
    {
      return requiredValue(key,
                           destination,
                           [](ryml::ConstNodeRef child, std::string_view context)
                           { return readScalarSequence<T>(child, context); });
    }

    template<typename T>
    MapReader& optionalScalar(std::string_view key, T& destination)
    {
      return optionalValue(key,
                           destination,
                           [](ryml::ConstNodeRef child, std::string_view context)
                           { return scalarAs<T>(child, context); });
    }

    template<typename T, typename Reader>
    MapReader& optionalValue(std::string_view key, T& destination, Reader const& reader)
    {
      if (_result)
      {
        if (auto const child = findChild(_node, key); child.readable())
        {
          assign(reader(child, fieldContext(_context, key)), destination);
        }
      }

      return *this;
    }

    template<typename T, typename ElementReader>
    MapReader& optionalSequence(std::string_view key, std::vector<T>& destination, ElementReader const& elementReader)
    {
      return optionalValue(key,
                           destination,
                           [&elementReader](ryml::ConstNodeRef child, std::string_view context)
                           { return readSequence<T>(child, context, elementReader); });
    }

    Result<> const& result() const noexcept { return _result; }

    template<typename T>
    Result<T> finish(T value) &&
    {
      if (!_result)
      {
        return std::unexpected{std::move(_result.error())};
      }

      return value;
    }

  private:
    template<typename T>
    void assign(Result<T> value, T& destination)
    {
      if (!value)
      {
        _result = std::unexpected{std::move(value.error())};
        return;
      }

      destination = std::move(*value);
    }

    ryml::ConstNodeRef _node;
    std::string _context;
    Result<> _result;
  };

  template<typename T, typename ElementReader>
  inline Result<std::vector<T>> readSequence(ryml::ConstNodeRef node,
                                             std::string_view context,
                                             ElementReader const& elementReader)
  {
    if (auto const result = requireSequence(node, context); !result)
    {
      return std::unexpected{result.error()};
    }

    auto values = std::vector<T>{};
    values.reserve(node.num_children());
    std::size_t index = 0;

    for (auto const& child : node.children())
    {
      auto value = elementReader(child, fieldContext(context, std::to_string(index)));

      if (!value)
      {
        return std::unexpected{value.error()};
      }

      values.push_back(std::move(*value));
      ++index;
    }

    return values;
  }

  template<typename Range, typename ElementWriter>
  inline Result<> writeSequence(ryml::NodeRef node, Range const& values, ElementWriter const& elementWriter)
  {
    node |= ryml::SEQ;

    for (auto const& value : values)
    {
      if (auto const result = elementWriter(node.append_child(), value); !result)
      {
        return result;
      }
    }

    return {};
  }

  template<typename T>
  inline Result<std::vector<T>> readScalarSequence(ryml::ConstNodeRef node, std::string_view context)
  {
    return readSequence<T>(node,
                           context,
                           [](ryml::ConstNodeRef child, std::string_view childContext)
                           { return scalarAs<T>(child, childContext); });
  }

  template<typename Range>
  inline Result<> writeScalarSequence(ryml::NodeRef node, Range const& values)
  {
    return writeSequence(node,
                         values,
                         [](ryml::NodeRef child, auto const& value) -> Result<>
                         {
                           writeScalar(child, value);
                           return {};
                         });
  }

  template<typename Range, typename ValueWriter>
  inline Result<> writeStringMap(ryml::NodeRef node,
                                 Range const& values,
                                 std::string_view context,
                                 ValueWriter const& valueWriter)
  {
    node |= ryml::MAP;

    for (auto const& [keyValue, value] : values)
    {
      auto const key = std::string_view{keyValue};

      if (key.empty())
      {
        return makeError(Error::Code::InvalidState, boundedErrorContext(context) + " contains an empty key");
      }

      if (auto const result = valueWriter(appendChild(node, key), value); !result)
      {
        return result;
      }
    }

    return {};
  }

  template<typename Map, typename ValueReader>
  inline Result<Map> readStringMap(ryml::ConstNodeRef node, std::string_view context, ValueReader const& valueReader)
  {
    if (auto const result =
          validateMapKeys(node, std::span<std::string_view const>{}, context, UnknownKeyPolicy::Allow);
        !result)
    {
      return std::unexpected{result.error()};
    }

    auto values = Map{};

    for (auto const& child : node.children())
    {
      auto const key = keyView(child);

      if (key.empty())
      {
        return makeError(Error::Code::FormatRejected, boundedErrorContext(context) + " contains an empty key");
      }

      auto value = valueReader(child, fieldContext(context, key));

      if (!value)
      {
        return std::unexpected{value.error()};
      }

      values.emplace(std::string{key}, std::move(*value));
    }

    return values;
  }
} // namespace ao::yaml
