// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#import <AppKit/AppKit.h>

#include <filesystem>

namespace ao::appkit
{
  class LibrarySession;
}

namespace ao::appkit::test
{
  void exercisePlaybackPresentation(LibrarySession& session,
                                    NSWindow* window,
                                    NSView* root,
                                    std::filesystem::path const& stateRoot);
}
