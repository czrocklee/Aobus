// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "input/KeymapAccelerators.h"

#include "pch.h"
#include <ao/rt/Log.h>
#include <ao/winui/input/KeyChordAccelerator.h>
#include <ao/winui/input/KeymapAcceleratorPlan.h>

#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.System.h>

#include <span>
#include <string>
#include <utility>

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
  } // namespace

  void applyKeymapAccelerators(UIElement const& scope,
                               std::span<KeymapAcceleratorPlan const> const plans,
                               KeymapActionInvoker invoke)
  {
    clearKeymapAccelerators(scope);

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
      // Leave nothing behind, then let the caller fail.
      clearKeymapAccelerators(scope);
      throw;
    }
  }

  void clearKeymapAccelerators(UIElement const& scope) noexcept
  {
    if (!scope)
    {
      return;
    }

    // Every caller is a teardown path, one of them `noexcept`. A shell that
    // cannot clear its accelerators is already being torn down, and the
    // handlers check their owner is alive before running, so the honest answer
    // is to record it and continue rather than end the process.
    try
    {
      scope.KeyboardAccelerators().Clear();
    }
    catch (winrt::hresult_error const& error)
    {
      APP_LOG_WARN("KeymapAccelerators: failed to clear the accelerators: {}", winrt::to_string(error.message()));
    }
    catch (...)
    {
      APP_LOG_WARN("KeymapAccelerators: failed to clear the accelerators");
    }
  }
} // namespace ao::winui
