// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <compare>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#if __has_include(<spdlog/fmt/fmt.h>)
#include <spdlog/fmt/fmt.h>
#elif __has_include(<fmt/format.h>)
#include <fmt/format.h>
#endif

namespace ao::utility
{
  /**
   * @brief A strong type wrapper for strings to prevent accidental parameter swapping.
   * @tparam Tag A unique tag type (usually an empty struct) to differentiate ID types.
   */
  template<typename Tag>
  class StrongId
  {
  public:
    StrongId() = default;
    explicit StrongId(std::string value)
      : _value(std::move(value))
    {
    }
    explicit StrongId(char const* value)
      : _value(value)
    {
    }

    std::string const& value() const noexcept { return _value; }
    std::string& value() noexcept { return _value; }

    bool empty() const noexcept { return _value.empty(); }
    void clear() noexcept { _value.clear(); }

    // Implicit conversion to string_view is safe for read-only access (logging, comparisons)
    operator std::string_view() const noexcept { return _value; }

    // Comparison operators
    auto operator<=>(StrongId const&) const = default;
    bool operator==(StrongId const&) const = default;

    // Heterogeneous comparison with string/string_view
    bool operator==(std::string_view rhs) const noexcept { return _value == rhs; }
    auto operator<=>(std::string_view rhs) const noexcept { return _value <=> rhs; }

  private:
    std::string _value;
  };
} // namespace ao::utility

namespace std
{
  /**
   * @brief Hash support for StrongId to allow use in unordered containers.
   */
  template<typename Tag>
  struct hash<ao::utility::StrongId<Tag>>
  {
    size_t operator()(ao::utility::StrongId<Tag> const& id) const noexcept { return hash<string>{}(id.value()); }
  };

  /**
   * @brief std::format support for StrongId.
   */
  template<typename Tag>
  struct formatter<ao::utility::StrongId<Tag>> : formatter<string_view>
  {
    auto format(ao::utility::StrongId<Tag> const& id, format_context& ctx) const
    {
      return formatter<string_view>::format(static_cast<string_view>(id), ctx);
    }
  };
} // namespace std

#if defined(FMT_VERSION)
namespace fmt
{
  template<typename Tag>
  struct formatter<ao::utility::StrongId<Tag>> : formatter<std::string_view>
  {
    auto format(ao::utility::StrongId<Tag> const& id, format_context& ctx) const
    {
      return formatter<std::string_view>::format(static_cast<std::string_view>(id), ctx);
    }
  };
} // namespace fmt
#endif
