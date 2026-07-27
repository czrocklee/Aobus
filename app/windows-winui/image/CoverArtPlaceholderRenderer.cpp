// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "image/CoverArtPlaceholderRenderer.h"

#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ao::winui
{
  namespace
  {
    constexpr auto kRegularMonogramScale = 0.58;
    constexpr auto kCompactMonogramScale = 0.46;

    winrt::Windows::UI::Color color(uimodel::CoverArtPlaceholderRgb const value) noexcept
    {
      return {.A = 255, .R = value.red, .G = value.green, .B = value.blue};
    }

    std::uint8_t hexChannel(std::string_view const value, std::size_t const offset) noexcept
    {
      auto nibble = [](char const character) noexcept -> std::uint8_t
      {
        if (character >= '0' && character <= '9')
        {
          return static_cast<std::uint8_t>(character - '0');
        }
        if (character >= 'A' && character <= 'F')
        {
          return static_cast<std::uint8_t>(character - 'A' + 10);
        }
        if (character >= 'a' && character <= 'f')
        {
          return static_cast<std::uint8_t>(character - 'a' + 10);
        }
        return 0;
      };
      return static_cast<std::uint8_t>((nibble(value[offset]) << 4U) | nibble(value[offset + 1]));
    }

    winrt::Windows::UI::Color themeColor(std::string_view const value) noexcept
    {
      if (value.size() != 7 || value.front() != '#')
      {
        return {.A = 255, .R = 0x06, .G = 0xB6, .B = 0xD4};
      }
      return {.A = 255, .R = hexChannel(value, 1), .G = hexChannel(value, 3), .B = hexChannel(value, 5)};
    }

    winrt::Windows::UI::Color mutedLabelColor(winrt::Windows::UI::Color const accent) noexcept
    {
      constexpr auto mixChannel = [](std::uint8_t const neutral, std::uint8_t const value) noexcept
      {
        return static_cast<std::uint8_t>(
          (static_cast<std::uint16_t>(neutral) * 7U + static_cast<std::uint16_t>(value) * 3U) / 10U);
      };
      return {
        .A = 245,
        .R = mixChannel(42, accent.R),
        .G = mixChannel(49, accent.G),
        .B = mixChannel(60, accent.B),
      };
    }

    double squareSide(winrt::Microsoft::UI::Xaml::FrameworkElement const& element) noexcept
    {
      auto const sideFrom = [](double const width, double const height) noexcept
      {
        return std::isfinite(width) && width > 0.0 && std::isfinite(height) && height > 0.0 ? std::min(width, height)
                                                                                            : 0.0;
      };

      auto current = element;
      while (current)
      {
        if (auto const actual = sideFrom(current.ActualWidth(), current.ActualHeight()); actual > 0.0)
        {
          return actual;
        }
        if (auto const declared = sideFrom(current.Width(), current.Height()); declared > 0.0)
        {
          return declared;
        }
        current = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(current)
                    .try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
      }
      return 48.0;
    }

    std::wstring_view assetPath(uimodel::CoverArtPlaceholderStyle const style) noexcept
    {
      using Style = uimodel::CoverArtPlaceholderStyle;
      switch (style)
      {
        case Style::Note: return L"ms-appx:///Assets/NoCover/note.svg";
        case Style::Vinyl: return L"ms-appx:///Assets/NoCover/vinyl.svg";
        case Style::Equalizer: return L"ms-appx:///Assets/NoCover/equalizer.svg";
        case Style::Soul: return L"ms-appx:///Assets/Brand/SoulMark.svg";
        case Style::Monogram: return {};
      }
      return L"ms-appx:///Assets/NoCover/note.svg";
    }
  } // namespace

  void renderCoverArtPlaceholder(winrt::Microsoft::UI::Xaml::Controls::Grid const& root,
                                 uimodel::CoverArtPlaceholderPresentation const& presentation,
                                 std::string_view const themeAccent)
  {
    using namespace winrt;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    using namespace Microsoft::UI::Xaml::Media;

    root.Background(nullptr);
    root.Children().Clear();
    auto const logicalSize = squareSide(root);

    if (presentation.style == uimodel::CoverArtPlaceholderStyle::Monogram)
    {
      auto text = TextBlock{};
      text.Text(to_hstring(presentation.monogram));
      auto const scale = presentation.monogramSize == uimodel::CoverArtPlaceholderMonogramSize::Compact
                           ? kCompactMonogramScale
                           : kRegularMonogramScale;
      text.FontSize(std::max(10.0, logicalSize * scale));
      text.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
      text.Foreground(SolidColorBrush{color(presentation.monogramColor)});
      text.HorizontalAlignment(HorizontalAlignment::Center);
      text.VerticalAlignment(VerticalAlignment::Center);
      root.Children().Append(text);
      return;
    }

    auto image = Image{};
    image.Source(Microsoft::UI::Xaml::Media::Imaging::BitmapImage{
      winrt::Windows::Foundation::Uri{hstring{assetPath(presentation.style)}}});
    image.Stretch(Stretch::Uniform);
    image.Margin(presentation.style == uimodel::CoverArtPlaceholderStyle::Note ? Thickness{4.0, 4.0, 4.0, 4.0}
                                                                               : Thickness{});
    image.Opacity(presentation.style == uimodel::CoverArtPlaceholderStyle::Soul ? 0.22 : 1.0);
    root.Children().Append(image);

    if (presentation.style != uimodel::CoverArtPlaceholderStyle::Vinyl)
    {
      return;
    }

    auto const accentSize = std::max(12.0, logicalSize * 228.0 / 256.0);
    auto accent = Shapes::Ellipse{};
    accent.Width(accentSize);
    accent.Height(accentSize);
    accent.Stroke(SolidColorBrush{themeColor(themeAccent)});
    accent.StrokeThickness(std::max(1.0, logicalSize * 1.5 / 256.0));
    accent.Opacity(0.36);
    accent.HorizontalAlignment(HorizontalAlignment::Center);
    accent.VerticalAlignment(VerticalAlignment::Center);
    root.Children().Append(accent);

    auto const labelSize = std::max(8.0, logicalSize / 3.0);
    auto const labelRadius = labelSize / 2.0;
    auto const spindleRadius = std::max(0.75, logicalSize * 3.0 / 256.0);
    auto labelGeometry = GeometryGroup{};
    labelGeometry.FillRule(FillRule::EvenOdd);
    auto outerLabel = EllipseGeometry{};
    outerLabel.Center({static_cast<float>(labelRadius), static_cast<float>(labelRadius)});
    outerLabel.RadiusX(labelRadius);
    outerLabel.RadiusY(labelRadius);
    labelGeometry.Children().Append(outerLabel);
    auto spindleHole = EllipseGeometry{};
    spindleHole.Center({static_cast<float>(labelRadius), static_cast<float>(labelRadius)});
    spindleHole.RadiusX(spindleRadius);
    spindleHole.RadiusY(spindleRadius);
    labelGeometry.Children().Append(spindleHole);

    auto label = Shapes::Path{};
    label.Data(labelGeometry);
    label.Fill(SolidColorBrush{mutedLabelColor(themeColor(themeAccent))});
    label.Width(labelSize);
    label.Height(labelSize);
    label.Stretch(Stretch::None);
    label.HorizontalAlignment(HorizontalAlignment::Center);
    label.VerticalAlignment(VerticalAlignment::Center);
    root.Children().Append(label);
  }
} // namespace ao::winui
