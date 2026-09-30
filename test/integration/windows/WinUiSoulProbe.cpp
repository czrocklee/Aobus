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
  bool completed = false;

  // C++/WinRT derives its allocation wrapper from the application implementation.
  class AobusSoulProbeApplication : public ApplicationT<AobusSoulProbeApplication, Markup::IXamlMetadataProvider>
  {
  public:
    AobusSoulProbeApplication()
    {
      UnhandledException(
        [](auto const&, UnhandledExceptionEventArgs const& arguments)
        {
          AO_FATAL("WinUI Soul probe XAML exception {:#x}: {}",
                   static_cast<std::uint32_t>(arguments.Exception().value),
                   winrt::to_string(arguments.Message()));
        });
      InitializeComponent();
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

    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type)
    {
      return AppProvider()->GetXamlType(type);
    }

    Markup::IXamlType GetXamlType(winrt::hstring const& fullName) { return AppProvider()->GetXamlType(fullName); }

    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return AppProvider()->GetXmlnsDefinitions(); }

  private:
    void InitializeComponent()
    {
      if (_contentLoaded)
      {
        return;
      }
      _contentLoaded = true;

      winrt::Windows::Foundation::Uri resourceLocator{L"ms-appx:///App.xaml"};
      Application::LoadComponent(*this, resourceLocator);
    }

    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> AppProvider()
    {
      if (!_appProvider)
      {
        _appProvider = winrt::make_self<winrt::Aobus::implementation::XamlMetaDataProvider>();
      }
      return _appProvider;
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
          completed = true;
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
      if (completed)
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

    bool _contentLoaded = false;
    winrt::com_ptr<winrt::Aobus::implementation::XamlMetaDataProvider> _appProvider{nullptr};
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
    Application::Start([](auto const&) { std::ignore = winrt::make<AobusSoulProbeApplication>(); });
    return completed ? 0 : 1;
  }
  catch (winrt::hresult_error const& error)
  {
    std::cerr << "WinUI Soul probe startup failed: " << winrt::to_string(error.message()) << '\n';
    return 2;
  }
}
