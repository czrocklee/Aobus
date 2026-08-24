// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/utility/FromChars.h>

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    template<typename T>
    std::optional<T> parseLayoutNumber(std::string_view value)
    {
      T parsed = {};
      auto const* const begin = value.data();
      auto const* const end = value.data() + value.size();
      auto const [ptr, ec] = utility::fromChars(begin, end, parsed);

      if (ec == std::errc{} && ptr == end)
      {
        return parsed;
      }

      return std::nullopt;
    }
  } // namespace

  std::string LayoutValue::asString(std::string const& defaultValue) const
  {
    return std::visit(
      [&defaultValue](auto const& val) -> std::string
      {
        using T = std::decay_t<decltype(val)>;

        if constexpr (std::is_same_v<T, std::string>)
        {
          return val;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
          return val ? "true" : "false";
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
          return std::format("{}", val);
        }
        else
        {
          return defaultValue;
        }
      },
      data);
  }

  std::int64_t LayoutValue::asInt(std::int64_t defaultValue) const
  {
    return std::visit(
      [defaultValue](auto const& val) -> std::int64_t
      {
        using T = std::decay_t<decltype(val)>;

        if constexpr (std::is_arithmetic_v<T>)
        {
          return static_cast<std::int64_t>(val);
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
          if (auto optParsed = parseLayoutNumber<std::int64_t>(val); optParsed)
          {
            return *optParsed;
          }

          return defaultValue;
        }
        else
        {
          return defaultValue;
        }
      },
      data);
  }

  bool LayoutValue::asBool(bool defaultValue) const
  {
    return std::visit(
      [defaultValue](auto const& val) -> bool
      {
        using T = std::decay_t<decltype(val)>;

        if constexpr (std::is_same_v<T, bool>)
        {
          return val;
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
          if (val == "true")
          {
            return true;
          }

          if (val == "false")
          {
            return false;
          }

          return defaultValue;
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
          return static_cast<bool>(val);
        }
        else
        {
          return defaultValue;
        }
      },
      data);
  }

  bool LayoutValue::isNumber() const
  {
    return std::holds_alternative<std::int64_t>(data) || std::holds_alternative<double>(data);
  }

  double LayoutValue::asDouble(double defaultValue) const
  {
    return std::visit(
      [defaultValue](auto const& val) -> double
      {
        using T = std::decay_t<decltype(val)>;

        if constexpr (std::is_arithmetic_v<T>)
        {
          return static_cast<double>(val);
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
          if (auto optParsed = parseLayoutNumber<double>(val); optParsed)
          {
            return *optParsed;
          }

          return defaultValue;
        }
        else
        {
          return defaultValue;
        }
      },
      data);
  }

  std::vector<std::string> LayoutValue::asStringList() const
  {
    if (auto const* ptr = std::get_if<std::vector<std::string>>(&data); ptr != nullptr)
    {
      return *ptr;
    }

    if (auto const* ptr = std::get_if<std::string>(&data); ptr != nullptr && !ptr->empty())
    {
      return {*ptr};
    }

    return {};
  }

  BoxedLayoutNode::BoxedLayoutNode() = default;

  BoxedLayoutNode::BoxedLayoutNode(LayoutNode value)
    : nodePtr{std::make_unique<LayoutNode>(std::move(value))}
  {
  }

  BoxedLayoutNode::BoxedLayoutNode(BoxedLayoutNode const& other)
    : nodePtr{other.nodePtr ? std::make_unique<LayoutNode>(*other.nodePtr) : nullptr}
  {
  }

  BoxedLayoutNode& BoxedLayoutNode::operator=(BoxedLayoutNode const& other)
  {
    if (this != &other)
    {
      nodePtr = other.nodePtr ? std::make_unique<LayoutNode>(*other.nodePtr) : nullptr;
    }

    return *this;
  }

  BoxedLayoutNode::BoxedLayoutNode(BoxedLayoutNode&&) noexcept = default;
  BoxedLayoutNode& BoxedLayoutNode::operator=(BoxedLayoutNode&&) noexcept = default;

  BoxedLayoutNode::~BoxedLayoutNode() = default;
} // namespace ao::uimodel
