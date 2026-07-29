// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/yaml/RymlAdapter.h>

#include <ryml.hpp>

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

  std::string fieldContext(std::string_view context, std::string_view field);
  Result<> requireMap(ryml::ConstNodeRef node, std::string_view context);
  Result<> requireSequence(ryml::ConstNodeRef node, std::string_view context);
  Result<> validateMapKeys(ryml::ConstNodeRef node,
                           std::span<std::string_view const> allowedKeys,
                           std::string_view context,
                           UnknownKeyPolicy unknownKeyPolicy = UnknownKeyPolicy::Reject);
  Result<ryml::ConstNodeRef> requireChild(ryml::ConstNodeRef node, std::string_view key, std::string_view context);
  ryml::NodeRef appendChild(ryml::NodeRef node, std::string_view key);
  void writeScalar(ryml::NodeRef node, std::string_view value);
  void writeScalar(ryml::NodeRef node, std::string const& value);
  void writeScalar(ryml::NodeRef node, bool value);

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
    explicit MapWriter(ryml::NodeRef node);

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

    Result<> finish() &&;

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
              UnknownKeyPolicy unknownKeyPolicy = UnknownKeyPolicy::Reject);

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
