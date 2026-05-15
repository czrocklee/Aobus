// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <cstdint>
#include <variant>

namespace ao::audio
{
  /**
   * @brief Unique identifiers for runtime backend properties.
   */
  enum class PropertyId : std::uint8_t
  {
    Volume,
    Muted,
  };

  /**
   * @brief A type-safe container for any supported property value.
   */
  using PropertyValue = std::variant<float, bool>;

  /**
   * @brief Metadata describing the capabilities and status of a property.
   */
  struct PropertyInfo final
  {
    bool canRead = false;
    bool canWrite = false;
    bool isAvailable = false;
    bool emitsChangeNotifications = false;

    constexpr bool operator==(PropertyInfo const&) const noexcept = default;
  };

  /**
   * @brief A type-tag for compile-time property safety.
   */
  template<typename T, PropertyId Id>
  struct TypedProperty final
  {
    using ValueType = T;
    static constexpr PropertyId kId = Id;
  };

  /**
   * @brief Standard property definitions for convenient access.
   */
  namespace props
  {
    inline constexpr auto kVolume = TypedProperty<float, PropertyId::Volume>{};
    inline constexpr auto kMuted = TypedProperty<bool, PropertyId::Muted>{};
  }
} // namespace ao::audio
