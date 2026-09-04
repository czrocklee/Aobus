// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace ao::uimodel
{
  struct LayoutValue final
  {
    using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::vector<std::string>>;

    LayoutValue() = default;

    template<typename T>
      requires(!std::is_same_v<std::remove_cvref_t<T>, LayoutValue>)
    explicit LayoutValue(T&& value)
      : data{std::forward<T>(value)}
    {
    }

    template<typename T>
    T const* getIf() const
    {
      return std::get_if<T>(&data);
    }

    template<typename T>
    T as(T defaultValue) const
    {
      if (auto const* ptr = getIf<T>(); ptr != nullptr)
      {
        return *ptr;
      }

      return defaultValue;
    }

    std::string asString(std::string const& defaultValue = "") const;
    std::int64_t asInt(std::int64_t defaultValue = 0) const;
    bool asBool(bool defaultValue = false) const;
    bool isNumber() const;

    double asDouble(double defaultValue = 0.0) const;

    Value data{};
  };

  using LayoutValueMap = std::map<std::string, LayoutValue, std::less<>>;

  struct LayoutNode;

  struct BoxedLayoutNode final
  {
    std::unique_ptr<LayoutNode> nodePtr{};

    BoxedLayoutNode();
    explicit BoxedLayoutNode(LayoutNode value);

    BoxedLayoutNode(BoxedLayoutNode const& other);
    BoxedLayoutNode& operator=(BoxedLayoutNode const& other);

    BoxedLayoutNode(BoxedLayoutNode&&) noexcept;
    BoxedLayoutNode& operator=(BoxedLayoutNode&&) noexcept;

    ~BoxedLayoutNode();
  };

  struct LayoutNode final
  {
    template<typename T>
    T propertyOr(std::string_view key, T defaultValue) const
    {
      if (auto const it = props.find(key); it != props.end())
      {
        return it->second.as<T>(defaultValue);
      }

      return defaultValue;
    }

    template<typename T>
    T layoutOr(std::string_view key, T defaultValue) const
    {
      if (auto const it = layout.find(key); it != layout.end())
      {
        return it->second.as<T>(defaultValue);
      }

      return defaultValue;
    }

    std::string id{};
    std::string type{};
    LayoutValueMap props{};
    LayoutValueMap layout{};
    std::vector<LayoutNode> children{};
    std::optional<BoxedLayoutNode> optTooltip{};
  };
} // namespace ao::uimodel
