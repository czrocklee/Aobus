// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#import <AppKit/AppKit.h>

#include <cstdint>
#include <filesystem>

namespace ao::appkit
{
  class LibrarySession;
}

namespace ao::appkit::test
{
  std::int32_t runQualityPresentationScenario(std::filesystem::path const& musicRoot,
                                              std::filesystem::path const& stateRoot);

  void exercisePlaybackPresentation(LibrarySession& session,
                                    NSWindow* window,
                                    NSView* root,
                                    std::filesystem::path const& stateRoot);
}
