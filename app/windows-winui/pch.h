// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// The product PCH intentionally aggregates framework and generated-XAML dependencies.
// NOLINTBEGIN(misc-include-cleaner)
#include <systemmediatransportcontrolsinterop.h>
#include <unknwn.h>
#include <windows.h>

#undef GetCurrentTime

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Microsoft.Windows.Storage.Pickers.h>
#include <winrt/Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>

// The XAML type provider constructs these implementation types directly.
// Header-main analysis keeps the framework prefix but parses implementation
// headers as their own main files, not first through this aggregate.
#ifndef AOBUS_LINT_HEADER_MAIN
#include "MainWindow.xaml.h"
#include "playback/AobusSoulControl.h"
#include "track/TrackCellItem.h"
#include "track/TrackRowItem.h"
#endif
// NOLINTEND(misc-include-cleaner)
