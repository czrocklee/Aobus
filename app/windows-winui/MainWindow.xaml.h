#pragma once

#include "MainWindow.g.h"

namespace winrt::Aobus::implementation
{
  struct MainWindow : MainWindowT<MainWindow>
  {
    MainWindow();

    void OnVerifyClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
  };
}

namespace winrt::Aobus::factory_implementation
{
  struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
  {};
}
