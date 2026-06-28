// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <string_view>

namespace include_convention_fixture
{
  using Handler = void (*)(std::string_view);

  void handleGenerated(std::string_view /*value*/)
  {
  }

// NEGATIVE
#include "tag/flac/VorbisCommentDispatch.h"
}

int main()
{
  return 0;
}
