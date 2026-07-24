#include "MainWindow.xaml.h"

#include "pch.h"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

namespace winrt::Aobus::implementation
{
  MainWindow::MainWindow()
  {
    InitializeComponent();
    Title(L"Aobus");
  }

  void MainWindow::OnVerifyClicked(Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&)
  {
    StatusText().Text(L"WinUI event binding is working.");
  }
}
