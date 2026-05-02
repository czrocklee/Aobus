// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstdint>
#include <exception>
#include <format>
#include <string>

namespace ao
{
  class Exception : public std::exception
  {
  public:
    Exception(std::string what, char const* file, std::int32_t line)
      : _what{std::move(what)}, _file{file}, _line{line}
    {
    }

    char const* file() const { return _file; }

    std::int32_t line() const { return _line; }

    char const* what() const noexcept override { return _what.c_str(); }

  private:
    std::string _what;
    char const* _file;
    std::int32_t _line;
  };
}

#define AO_THROW(Expression, what) throw Expression{what, __FILE__, __LINE__};
#define AO_THROW_FORMAT(Expression, f, ...) throw Expression{std::format(f, __VA_ARGS__), __FILE__, __LINE__};
