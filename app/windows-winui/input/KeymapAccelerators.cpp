// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "input/KeymapAccelerators.h"

#include "pch.h"
#include <ao/Contract.h>
#include <ao/rt/Log.h>
#include <ao/winui/input/KeyChordAccelerator.h>
#include <ao/winui/input/KeymapAcceleratorPlan.h>

#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.System.h>

#include <span>
#include <string>

namespace ao::winui
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::UIElement;
    using winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator;
    using winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs;
    using winrt::Windows::System::VirtualKey;
    using winrt::Windows::System::VirtualKeyModifiers;

    KeyboardAccelerator makeAccelerator(KeymapAcceleratorPlan const& plan, KeymapActionInvoker const& invoke)
    {
      auto accelerator = KeyboardAccelerator{};
      accelerator.Key(static_cast<VirtualKey>(plan.key.virtualKey));
      accelerator.Modifiers(static_cast<VirtualKeyModifiers>(plan.key.modifiers));

      // The handler lives as long as the accelerator, and the collection owns
      // the accelerator, so clearing the collection is what releases the
      // invoker. That is the whole revocation contract for these.
      accelerator.Invoked(
        [actionId = plan.actionId, invoke](KeyboardAccelerator const&, KeyboardAcceleratorInvokedEventArgs const& args)
        { args.Handled(invoke(actionId)); });
      return accelerator;
    }

    /// Drops every accelerator @p scope carries, letting a failure reach the caller.
    void clearAccelerators(UIElement const& scope)
    {
      if (!scope)
      {
        return;
      }

      scope.KeyboardAccelerators().Clear();
    }
  } // namespace

  void applyKeymapAccelerators(UIElement const& scope,
                               std::span<KeymapAcceleratorPlan const> const plans,
                               KeymapActionInvoker invoke)
  {
    // Throwing form, not the noexcept one below: a clear that failed here left
    // the previous accelerators installed, and appending on top of them would
    // leave one key answered by two handlers. That is the opposite of the
    // replacement this function promises, so the caller has to hear about it.
    // Nothing has been appended yet, so failing now leaves the scope as it was.
    clearAccelerators(scope);

    if (!scope || !invoke)
    {
      return;
    }

    auto const accelerators = scope.KeyboardAccelerators();

    try
    {
      for (auto const& plan : plans)
      {
        accelerators.Append(makeAccelerator(plan, invoke));
      }
    }
    catch (...)
    {
      // The scope outlives this call and usually outlives the caller too, so a
      // half-installed set would keep answering keys with handlers nobody owns.
      // Leave nothing behind, then let the caller fail. The noexcept form here,
      // because an exception is already in flight.
      clearKeymapAccelerators(scope);
      throw;
    }
  }

  void clearKeymapAccelerators(UIElement const& scope) noexcept
  {
    // Reaching here means the accelerators are being abandoned rather than
    // replaced: an owner is releasing the invoker they hold, or an install is
    // unwinding with an exception already in flight. Either way no live operation
    // depends on the outcome, and the handlers check their owner is alive before
    // running, so recording the failure beats ending the process. An install that
    // needs the clear to have worked calls `clearAccelerators` instead.
    try
    {
      clearAccelerators(scope);
    }
    catch (winrt::hresult_error const& error)
    {
      APP_LOG_WARN("KeymapAccelerators: failed to clear the accelerators: {}", winrt::to_string(error.message()));
    }
    catch (...)
    {
      AO_AUDITED_CATCH(SafeCleanup);
      APP_LOG_WARN("KeymapAccelerators: failed to clear the accelerators");
    }
  }
} // namespace ao::winui
