// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
}

namespace Gtk
{
  class Window;
  class Widget;
}

namespace ao::gtk::layout
{
  struct ActionActivationContext final
  {
    rt::AppRuntime& runtime;
    Gtk::Window& parentWindow;
    Gtk::Widget& anchorWidget;
    std::string componentId;
  };

  enum class ActionCapability : std::uint8_t
  {
    None = 0,
    RequiresAnchor = 1U << 0U,
    RequiresActiveTrack = 1U << 1U,
    RequiresFocusedView = 1U << 2U,
    PresentsMenu = 1U << 3U
  };

  struct ActionCapabilities final
  {
    std::uint32_t mask = 0;

    constexpr ActionCapabilities() = default;
    constexpr ActionCapabilities(ActionCapability cap)
      : mask{static_cast<std::uint32_t>(cap)}
    {
    }
    constexpr explicit ActionCapabilities(std::uint32_t mask)
      : mask{mask}
    {
    }

    constexpr bool has(ActionCapability cap) const
    {
      auto const val = static_cast<std::uint32_t>(cap);
      return (mask & val) == val;
    }

    constexpr ActionCapabilities operator|(ActionCapabilities other) const
    {
      return ActionCapabilities{mask | other.mask};
    }

    constexpr ActionCapabilities& operator|=(ActionCapabilities other)
    {
      mask |= other.mask;
      return *this;
    }
  };

  constexpr ActionCapabilities operator|(ActionCapability lhs, ActionCapability rhs)
  {
    return ActionCapabilities{lhs} | ActionCapabilities{rhs};
  }

  /**
   * @brief Metadata describing an action.
   */
  struct ActionDescriptor final
  {
    std::string id;
    std::string label;
    std::string category;
    ActionCapabilities capabilities = ActionCapability::None;
  };

  /**
   * @brief Current runtime state of an action.
   */
  struct ActionState final
  {
    bool enabled = true;
    std::string disabledReason;
  };

  using ActionHandler = std::function<void(ActionActivationContext&)>;
  using ActionStateProvider = std::function<ActionState(ActionActivationContext const&)>;

  enum class ActionSlot : std::uint8_t
  {
    PrimaryClick,
    PrimaryLongPress,
    SecondaryClick,
    SecondaryLongPress,
    MenuItem,
    Shortcut
  };

  struct ActionBindingProperty final
  {
    ActionSlot slot = ActionSlot::PrimaryClick;
  };

  struct ActionBindingContext final
  {
    ActionSlot slot = ActionSlot::PrimaryClick;
    bool hasAnchor = false;
    bool hasFocusedView = false;
    std::string_view componentType;
  };

  enum class ActionActivationResult : std::uint8_t
  {
    Activated,
    UnknownAction,
    Disabled,
    InvalidBinding // Reserved for future structural runtime validation
  };

  /**
   * @brief Full result of an activation attempt, including the captured state.
   */
  struct ActionActivationOutcome final
  {
    ActionActivationResult result = ActionActivationResult::UnknownAction;
    ActionState state = {};
  };

  class ActionRegistry final
  {
  public:
    ActionRegistry();
    ~ActionRegistry();

    ActionRegistry(ActionRegistry const&) = delete;
    ActionRegistry& operator=(ActionRegistry const&) = delete;
    ActionRegistry(ActionRegistry&&) = delete;
    ActionRegistry& operator=(ActionRegistry&&) = delete;

    bool registerAction(ActionDescriptor descriptor,
                        ActionHandler handler,
                        ActionStateProvider stateProvider = {});

    std::optional<ActionDescriptor> descriptor(std::string_view id) const;
    std::vector<ActionDescriptor> descriptors() const;

    bool canBind(std::string_view id, ActionBindingContext const& ctx) const;

    /**
     * @brief Attempts to bind an action to a specific context.
     * @return true if the action exists and is compatible with the context.
     */
    bool tryBind(std::string_view id, ActionBindingContext const& ctx) const;

    ActionState state(std::string_view id, ActionActivationContext const& ctx) const;

    /**
     * @brief Activates an action if it exists and is currently enabled.
     * @return The outcome of the activation attempt, including captured state.
     */
    ActionActivationOutcome activate(std::string_view id, ActionActivationContext& ctx) const;

    /**
     * @brief Safe activation helper that performs registry checks and logging.
     */
    ActionActivationOutcome tryActivate(std::string_view id, ActionActivationContext& ctx) const;

  private:
    struct Entry final
    {
      ActionDescriptor descriptor;
      ActionHandler handler;
      ActionStateProvider stateProvider;
    };

    std::vector<Entry> _entries;
  };
} // namespace ao::gtk::layout
