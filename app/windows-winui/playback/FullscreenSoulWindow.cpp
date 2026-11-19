// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "playback/FullscreenSoulWindow.h"

// Select native IUnknown support before loading the C++/WinRT projections.
#include <unknwn.h>
#undef GetCurrentTime

#include "platform/AppWindowActivity.h"
#include "platform/WinRtStringResources.h"
#include "playback/AobusSoulControl.h"
#include <ao/uimodel/playback/transport/TransportViewModel.h>
#include <ao/utility/ScopedRegistration.h>

#include <winrt/Aobus.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using winrt::Microsoft::UI::Windowing::AppWindow;
    using winrt::Microsoft::UI::Windowing::OverlappedPresenter;
    using winrt::Microsoft::UI::Xaml::Window;
    using winrt::Microsoft::UI::Xaml::WindowEventArgs;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Windows::Foundation::IInspectable;
    using SoulControl = winrt::Aobus::implementation::AobusSoulControl;

    constexpr double kFullscreenSoulPadding = 48.0;
  } // namespace

  // Each opening has its own state. An in-flight call can pin it across re-entry without
  // retaining the frame or playback service. Delegates refer weakly only to this opening.
  class FullscreenSoulWindow::State final
  {
  public:
    void open(rt::PlaybackService& playback, std::weak_ptr<State> const& weakStatePtr);
    void activate();
    void retire(bool closeNative) noexcept;
    bool isClosed() const noexcept { return _phase == Phase::Closed; }

  private:
    enum class Phase : std::uint8_t
    {
      Opening,
      Ready,
      Activating,
      Closed,
    };

    void updateActivity();

    Phase _phase = Phase::Opening;
    Window _window{nullptr};
    AppWindow _appWindow{nullptr};
    winrt::Aobus::AobusSoulControl _control{nullptr};
    Window::Closed_revoker _closedRevoker{};
    utility::ScopedRegistration _activitySub;
  };

  void FullscreenSoulWindow::State::open(rt::PlaybackService& playback, std::weak_ptr<State> const& weakStatePtr)
  {
    auto const window = Window{};

    if (isClosed())
    {
      window.Close();
      return;
    }

    _window = window;
    auto closedRevoker = window.Closed(winrt::auto_revoke,
                                       [weakStatePtr](IInspectable const&, WindowEventArgs const&) noexcept
                                       {
                                         if (auto const statePtr = weakStatePtr.lock(); statePtr)
                                         {
                                           statePtr->retire(false);
                                         }
                                       });

    if (isClosed())
    {
      return;
    }

    _closedRevoker = std::move(closedRevoker);
    window.Title(resourceHstring(L"SoulWindowTitle"));
    auto const appWindow = window.AppWindow();
    auto const control = winrt::Aobus::AobusSoulControl{};
    auto* const soul = winrt::get_self<SoulControl>(control);
    soul->setTransportIcon(uimodel::TransportIcon::None);
    soul->setWindowActivity(false, false);

    auto const root = Grid{};
    root.Padding({
      .Left = kFullscreenSoulPadding,
      .Top = kFullscreenSoulPadding,
      .Right = kFullscreenSoulPadding,
      .Bottom = kFullscreenSoulPadding,
    });
    root.Children().Append(control);

    if (isClosed())
    {
      return;
    }

    // Bind before attachment can deliver Loaded; the detached control cannot start rendering.
    soul->bind(playback);

    if (isClosed())
    {
      soul->unbind();
      return;
    }

    _control = control;
    _appWindow = appWindow;
    window.Content(root);

    if (isClosed())
    {
      return;
    }

    auto const onActivityChanged = [weakStatePtr]
    {
      if (auto const statePtr = weakStatePtr.lock(); statePtr)
      {
        statePtr->updateActivity();
      }
    };
    auto activitySub = observeWindowActivityChanges(appWindow, onActivityChanged);

    if (isClosed())
    {
      return;
    }

    _activitySub = std::move(activitySub);

    if (auto const presenter = appWindow.Presenter().try_as<OverlappedPresenter>(); presenter && !isClosed())
    {
      presenter.Maximize();
    }

    if (!isClosed())
    {
      _phase = Phase::Ready;
      activate();
    }
  }

  void FullscreenSoulWindow::State::activate()
  {
    // Activation may deliver another show() or Closed synchronously.
    if (_phase != Phase::Ready)
    {
      return;
    }

    _phase = Phase::Activating;
    auto const window = _window;
    window.Activate();
    updateActivity();

    if (!isClosed())
    {
      _phase = Phase::Ready;
    }
  }

  void FullscreenSoulWindow::State::retire(bool const closeNative) noexcept
  {
    if (isClosed())
    {
      return;
    }

    _phase = Phase::Closed;
    auto const window = std::exchange(_window, nullptr);

    if (auto const control = std::exchange(_control, nullptr); control)
    {
      winrt::get_self<SoulControl>(control)->unbind();
    }

    _activitySub.reset();
    _closedRevoker.revoke();
    _appWindow = nullptr;

    if (closeNative && window)
    {
      window.Close();
    }
  }

  void FullscreenSoulWindow::State::updateActivity()
  {
    if (isClosed())
    {
      return;
    }

    auto const appWindow = _appWindow;
    auto const visible = appWindow.IsVisible();

    if (auto const minimized = isWindowMinimized(appWindow); !isClosed())
    {
      auto const control = _control;
      winrt::get_self<SoulControl>(control)->setWindowActivity(visible, minimized);
    }
  }

  FullscreenSoulWindow::FullscreenSoulWindow(rt::PlaybackService& playback) noexcept
    : _playback{playback}
  {
  }

  FullscreenSoulWindow::~FullscreenSoulWindow() noexcept
  {
    if (auto const statePtr = std::move(_statePtr); statePtr)
    {
      statePtr->retire(true);
    }
  }

  void FullscreenSoulWindow::show()
  {
    auto statePtr = _statePtr;
    bool const needsWindow = !statePtr || statePtr->isClosed();

    if (needsWindow)
    {
      statePtr = std::make_shared<State>();
      _statePtr = statePtr;
    }

    try
    {
      if (needsWindow)
      {
        statePtr->open(_playback, statePtr);
      }
      else
      {
        statePtr->activate();
      }
    }
    catch (...)
    {
      // Never retire a replacement that was opened during a native call above.
      statePtr->retire(true);
      throw;
    }
  }
} // namespace ao::winui
