// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Output.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <ostream>
#include <print>
#include <string>
#include <string_view>

namespace ao::cli
{
  namespace
  {
    std::string quoteDoubleQuoted(std::string_view value)
    {
      auto out = std::string{};
      out.reserve(value.size() + 2);
      out.push_back('"');

      for (auto const ch : value)
      {
        switch (auto const byte = static_cast<unsigned char>(ch); ch)
        {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\b': out += "\\b"; break;
          case '\f': out += "\\f"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '\t': out += "\\t"; break;
          default:
            if (byte < 0x20)
            {
              out += std::format("\\u{:04x}", static_cast<std::uint32_t>(byte));
            }
            else
            {
              out.push_back(ch);
            }
        }
      }

      out.push_back('"');
      return out;
    }
  } // namespace

  std::string yamlQuote(std::string_view value)
  {
    return quoteDoubleQuoted(value);
  }

  std::string jsonQuote(std::string_view value)
  {
    return quoteDoubleQuoted(value);
  }

  std::string quote(OutputFormat format, std::string_view value)
  {
    return format == OutputFormat::Json ? jsonQuote(value) : yamlQuote(value);
  }

  JsonObject::JsonObject(std::ostream& os)
    : _os{os}
  {
    std::print(_os, "{{");
  }

  JsonObject::~JsonObject()
  {
    closeQuietly();
  }

  void JsonObject::field(std::string_view key)
  {
    if (!_first)
    {
      std::print(_os, ",");
    }

    std::print(_os, "{}:", jsonQuote(key));
    _first = false;
  }

  void JsonObject::stringField(std::string_view key, std::string_view value)
  {
    field(key);
    std::print(_os, "{}", jsonQuote(value));
  }

  void JsonObject::uintField(std::string_view key, std::uint64_t value)
  {
    field(key);
    std::print(_os, "{}", value);
  }

  void JsonObject::boolField(std::string_view key, bool value)
  {
    field(key);
    std::print(_os, "{}", value ? "true" : "false");
  }

  void JsonObject::close()
  {
    if (!_closed)
    {
      std::print(_os, "}}");
      _closed = true;
    }
  }

  void JsonObject::closeQuietly() noexcept
  {
    try
    {
      close();
    }
    catch (...)
    {
      _closed = true;
    }
  }

  JsonArray::JsonArray(std::ostream& os)
    : _os{os}
  {
    std::print(_os, "[");
  }

  JsonArray::~JsonArray()
  {
    closeQuietly();
  }

  void JsonArray::element()
  {
    if (!_first)
    {
      std::print(_os, ",");
    }

    _first = false;
  }

  void JsonArray::close()
  {
    if (!_closed)
    {
      std::print(_os, "]");
      _closed = true;
    }
  }

  void JsonArray::closeQuietly() noexcept
  {
    try
    {
      close();
    }
    catch (...)
    {
      _closed = true;
    }
  }

  YamlSequence::YamlSequence(std::ostream& os, std::int32_t indent, std::string_view key)
    : _os{os}, _indent{indent}, _key{key}
  {
  }

  YamlSequence::~YamlSequence()
  {
    closeQuietly();
  }

  void YamlSequence::itemPrefix()
  {
    ensureStarted();
    std::print(_os, "{}- ", std::string(static_cast<std::size_t>(_indent + 2), ' '));
  }

  void YamlSequence::close()
  {
    if (_closed)
    {
      return;
    }

    if (!_started)
    {
      std::println(_os, "{}{}: []", std::string(static_cast<std::size_t>(_indent), ' '), _key);
    }

    _closed = true;
  }

  void YamlSequence::closeQuietly() noexcept
  {
    try
    {
      close();
    }
    catch (...)
    {
      _closed = true;
    }
  }

  void YamlSequence::ensureStarted()
  {
    if (!_started)
    {
      std::println(_os, "{}{}:", std::string(static_cast<std::size_t>(_indent), ' '), _key);
      _started = true;
    }
  }

  void yamlKeyValue(std::ostream& os, std::int32_t indent, std::string_view key, std::string_view value)
  {
    std::println(os, "{}{}: {}", std::string(static_cast<std::size_t>(indent), ' '), key, yamlQuote(value));
  }

  void yamlKeyValue(std::ostream& os, std::int32_t indent, std::string_view key, char const* value)
  {
    yamlKeyValue(os, indent, key, std::string_view{value});
  }

  void yamlKeyValue(std::ostream& os, std::int32_t indent, std::string_view key, std::uint64_t value)
  {
    std::println(os, "{}{}: {}", std::string(static_cast<std::size_t>(indent), ' '), key, value);
  }

  void yamlKeyValue(std::ostream& os, std::int32_t indent, std::string_view key, bool value)
  {
    std::println(os, "{}{}: {}", std::string(static_cast<std::size_t>(indent), ' '), key, value ? "true" : "false");
  }
} // namespace ao::cli
