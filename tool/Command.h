// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ostream>
#include <system_error>
#include <vector>

class Command
{
public:
  virtual ~Command() {};

  virtual void execute(int argc, char const* argv[], std::ostream& os) = 0;
};
