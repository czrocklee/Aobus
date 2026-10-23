// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "XamlMetaDataProvider.h"
#include "playback/AobusSoulControl.h"
#include <ao/Contract.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <utility>

namespace
{
  using namespace winrt::Microsoft::UI::Xaml;
  using ao::uimodel::AobusSoulMotionMode;
  using ao::uimodel::AobusSoulViewState;
  using ao::uimodel::SoulAura;

  constexpr auto kTargets = std::to_array<AobusSoulViewState>({
    {.aura = SoulAura::Radiant, .motionMode = AobusSoulMotionMode::Frozen},
    {.aura = SoulAura::Dormant, .motionMode = AobusSoulMotionMode::Dormant},
  });

  // C++/WinRT derives its allocation wrapper from the application implementation.
  class AobusSoulProbeApplication : public ApplicationT<AobusSoulProbeApplication, Markup::IXamlMetadataProvider>
  {
  public:
    explicit AobusSoulProbeApplication(bool& completed)
      : _completed{completed}
    {
      UnhandledException(
        [](auto const&, UnhandledExceptionEventArgs const& arguments)
        {
          AO_FATAL("WinUI Soul probe XAML exception {:#x}: {}",
                   static_cast<std::uint32_t>(arguments.Exception().value),
                   winrt::to_string(arguments.Message()));
        });
      initializeComponent();
    }

    void OnLaunched(LaunchActivatedEventArgs const& /*arguments*/)
    {
      _window = Window{};
      _window.Title(L"Aobus Soul unload/reload probe");
      _root = Controls::Grid{};
      _soulPtr = winrt::make_self<winrt::Aobus::implementation::AobusSoulControl>();
      _soulPtr->Width(65.0);
      _soulPtr->Height(65.0);
      _soulPtr->Loaded([this](auto const&, auto const&) { enqueue([this] { handleLoaded(); }); });
      _soulPtr->Unloaded([this](auto const&, auto const&) { handleUnloaded(); });
      _root.Children().Append(_soulPtr.as<UIElement>());
      _window.Content(_root);
      _timeout = _window.DispatcherQueue().CreateTimer();
      _timeout.Interval(std::chrono::seconds{20});
      _timeout.IsRepeating(false);
      _timeout.Tick([](auto const&, auto const&) { AO_FATAL("WinUI Soul unload/reload probe timed out"); });
      _timeout.Start();
      _window.Activate();
    }

    // The IXamlMetadataProvider boundary requires these exact names for C++/WinRT consume/produce dispatch.
    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
      return appProvider()->GetXamlType(type);
    }

    Markup::IXamlType GetXamlType(winrt::hstring const& fullName) { return appProvider()->GetXamlType(fullName); }

    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return appProvider()->GetXmlnsDefinitions(); }

  private:
    void initializeComponent()
    {
      if (_contentLoaded)
      {
        return;
      }

      _contentLoaded = true;

      auto const resourceLocator = winrt::Windows::Foundation::Uri{L"ms-appx:///App.xaml"};
      Application::LoadComponent(*this, resourceLocator);
    }

    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> appProvider()
    {
      if (!_appProviderPtr)
      {
        _appProviderPtr = winrt::make_self<winrt::Aobus::implementation::XamlMetaDataProvider>();
      }

      return _appProviderPtr;
    }

    template<typename Callback>
    void enqueue(Callback callback)
    {
      auto const admitted = _window.DispatcherQueue().TryEnqueue(std::move(callback));
      AO_INVARIANT(admitted, "The live Soul probe dispatcher must admit its next native lifecycle step");
    }

    void handleLoaded()
    {
      AO_INVARIANT(_soulPtr->IsLoaded(), "The probe must run inside a real loaded XAML tree");

      if (_reloading)
      {
        AO_INVARIANT(!_soulPtr->needsFrames() && _soulPtr->visualFrame() == _held,
                     "Reloading a settled Soul must not replay its pause coast or aura fade");
        ++_caseIndex;
        _reloading = false;

        if (_caseIndex == kTargets.size())
        {
          _completed = true;
          _timeout.Stop();
          _window.Close();
          Exit();
          return;
        }
      }

      _soulPtr->presentState({.aura = SoulAura::Radiant, .motionMode = AobusSoulMotionMode::Animating});
      // Settle the playing state without waiting for real time, then restore
      // frame admission so the next target starts a genuine pending transition.
      _soulPtr->setWindowActivity(false, false);
      _soulPtr->setWindowActivity(true, false);
      _soulPtr->presentState(kTargets[_caseIndex]);
      AO_INVARIANT(_soulPtr->needsFrames(), "The unload must interrupt a pending pause coast or aura fade");
      _root.Children().Clear();
    }

    void handleUnloaded()
    {
      if (_completed)
      {
        return;
      }

      AO_INVARIANT(!_soulPtr->IsLoaded() && !_soulPtr->needsFrames(),
                   "A real XAML Unloaded event must settle pending Soul transitions");
      auto const settled = _soulPtr->visualFrame();
      AO_INVARIANT(settled == ao::uimodel::aobusSoulVisualFrame(
                                ao::uimodel::aobusSoulAuraRgb(kTargets[_caseIndex].aura), settled.motion),
                   "Unload settlement must present the target aura immediately");
      _held = settled;
      _reloading = true;
      enqueue([this] { _root.Children().Append(_soulPtr.as<UIElement>()); });
    }

    // main owns the result until Application::Start returns after native shutdown.
    bool& _completed;
    bool _contentLoaded = false;
    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> _appProviderPtr{nullptr};
    Window _window{nullptr};
    Controls::Grid _root{nullptr};
    winrt::com_ptr<winrt::Aobus::implementation::AobusSoulControl> _soulPtr;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _timeout{nullptr};
    ao::uimodel::AobusSoulVisualFrame _held{};
    std::size_t _caseIndex = 0;
    bool _reloading = false;
  };
} // namespace

int main()
{
  try
  {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    bool completed = false;
    Application::Start([&completed](auto const&) { std::ignore = winrt::make<AobusSoulProbeApplication>(completed); });
    return completed ? 0 : 1;
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << "WinUI Soul probe startup failed: " << winrt::to_string(error.message()) << '\n';
    return 2;
  }
}
