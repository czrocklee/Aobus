// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::desktop
{
  inline constexpr std::string_view kLibrarySuccessorOption = "--aobus-successor";
  inline constexpr std::string_view kLibraryRootOption = "--library-root";
  inline constexpr std::string_view kScanAfterOpenOption = "--scan-after-open";

  struct LibrarySuccessorProtocolParse final
  {
    std::optional<LibrarySwitchRequest> optRequest{};
    std::vector<std::string> remainingArguments{};

    friend bool operator==(LibrarySuccessorProtocolParse const&, LibrarySuccessorProtocolParse const&) = default;
  };

  /** Consume the paired private successor arguments and preserve all other arguments in order. */
  Result<LibrarySuccessorProtocolParse> parseLibrarySuccessorProtocol(std::span<std::string_view const> arguments);

  /** Encode one validated successor request without argv[0]. */
  Result<std::vector<std::string>> librarySuccessorArguments(LibrarySwitchRequest const& request);
} // namespace ao::desktop
