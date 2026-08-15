// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/winui/input/KeymapAcceleratorPlan.h>

#include <winrt/Microsoft.UI.Xaml.h>

#include <functional>
#include <span>
#include <string_view>

namespace ao::winui
{
  /// Runs the named shell action and reports whether anything handled it.
  using KeymapActionInvoker = std::function<bool(std::string_view actionId)>;

  /**
   * @brief Installs @p plans on @p scope as WinUI keyboard accelerators.
   *
   * Replaces whatever @p scope carried, so it is safe to call again after the
   * user edits their shortcuts. What belongs in @p plans is decided by
   * `planKeymapAccelerators`; this only carries the decision into XAML.
   *
   * @p invoke is held by every accelerator installed here, so it must remain
   * callable for as long as they do. `clearKeymapAccelerators` is how an owner
   * releases them before going away. An owner that installs during its own
   * construction should also gate @p invoke on its own lifetime, since a throw
   * after this returns leaves the handlers in place with no destructor to run.
   *
   * Installing is all-or-nothing: if XAML refuses part-way, whatever was
   * already appended is removed before the failure propagates, so @p scope is
   * never left answering keys with handlers no one owns.
   */
  void applyKeymapAccelerators(winrt::Microsoft::UI::Xaml::UIElement const& scope,
                               std::span<KeymapAcceleratorPlan const> plans,
                               KeymapActionInvoker invoke);

  /**
   * @brief Drops every accelerator @p scope carries, before the invoker they hold goes away.
   *
   * For abandoning the accelerators, not for replacing them. Reports a failure
   * rather than throwing, because a caller here is either releasing the invoker
   * or unwinding with an exception already in flight, and throwing would end the
   * process over a shell that was already going away.
   *
   * `applyKeymapAccelerators` therefore does not use this to make room for a new
   * set: a swallowed failure there would leave the old accelerators installed
   * under the new ones.
   */
  void clearKeymapAccelerators(winrt::Microsoft::UI::Xaml::UIElement const& scope) noexcept;
} // namespace ao::winui
