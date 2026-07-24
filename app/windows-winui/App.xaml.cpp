#include "App.xaml.h"

#include "MainWindow.xaml.h"
#include "pch.h"

namespace winrt::Aobus::implementation
{
  App::App()
  {
    InitializeComponent();
  }

  void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
  {
    window_ = winrt::make<MainWindow>();
    window_.Activate();
  }
}
