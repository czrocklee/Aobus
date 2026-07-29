// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Output.h"

#include <ostream>
#include <string>

namespace ao::cli::detail
{
  void emitGeneratedDocument(std::ostream& os, std::string const& text)
  {
    os << text;

    if (text.empty() || text.back() != '\n')
    {
      os << '\n';
    }
  }
} // namespace ao::cli::detail
