// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/LayoutHost.h"

#include "pch.h"
#include <ao/Error.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <expected>
#include <memory>
#include <utility>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::UIElement;
  } // namespace

  LayoutHost::LayoutHost(winrt::Microsoft::UI::Xaml::Controls::Border host)
    : _host{std::move(host)}
  {
  }

  LayoutHost::~LayoutHost()
  {
    retire();
  }

  std::shared_ptr<uimodel::ShellGenerationGate> LayoutHost::stage()
  {
    return _sequence.stage();
  }

  Result<> LayoutHost::publish(ShellGeneration candidate)
  {
    if (candidate.rootPtr == nullptr || candidate.gatePtr == nullptr)
    {
      return makeError(Error::Code::InvalidState, "A Windows shell candidate must carry a root and a generation gate");
    }

    if (!_host)
    {
      return makeError(Error::Code::InvalidState, "The Windows layout host has no native surface");
    }

    auto const previousChild = _host.Child();
    auto const candidateElement = candidate.rootPtr->element();

    auto publishedRes =
      _sequence.publish(candidate.gatePtr->id(),
                        [this, candidateElement] -> Result<>
                        {
                          try
                          {
                            _host.Child(candidateElement);
                          }
                          catch (winrt::hresult_error const& error)
                          {
                            return makeError(Error::Code::InitFailed, winrt::to_string(error.message()));
                          }

                          return {};
                        });

    if (!publishedRes)
    {
      try
      {
        _host.Child(previousChild);
      }
      catch (winrt::hresult_error const& error)
      {
        AO_FATAL("LayoutHost failed to restore the previous shell root: {}", winrt::to_string(error.message()));
      }

      return std::unexpected{publishedRes.error()};
    }

    // The retired generation is destroyed only after the candidate is live, so
    // no callback can observe a window with neither generation attached.
    auto const retired = std::move(_active);
    _active = std::move(candidate);
    return {};
  }

  void LayoutHost::discard(ShellGeneration candidate)
  {
    if (candidate.gatePtr != nullptr)
    {
      _sequence.discard(candidate.gatePtr->id());
    }
  }

  void LayoutHost::retire() noexcept
  {
    // Close the generation gate first. Any callback delivered during native
    // detachment must become a no-op before the component tree is released.
    _sequence.retireActive();

    if (_host)
    {
      try
      {
        _host.Child(UIElement{nullptr});
      }
      catch (winrt::hresult_error const& error)
      {
        APP_LOG_WARN("LayoutHost: failed to detach the shell root: {}", winrt::to_string(error.message()));
      }
      catch (...)
      {
        AO_FATAL_EXCEPTION(std::current_exception(), "WinUI shell root detachment");
      }
    }

    // The active generation owns all runtime-borrowing controls. It is released
    // even when the native host rejected the detach above.
    _active = {};
  }
} // namespace ao::winui::layout
