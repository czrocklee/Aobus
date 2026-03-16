// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <exception>
#include <format>
#include <string>

namespace rs::core
{
  class Exception : public std::exception
  {
  public:
    Exception(std::string const& what, char const* file, int line) : _file{file}, _line{line}, _what{what} {}

    [[nodiscard]] char const* file() const { return _file; }

    [[nodiscard]] int line() const { return _line; }

    [[nodiscard]] char const* what() const noexcept override { return _what.c_str(); }

  private:
    char const* _file;
    int _line;
    std::string _what;
  };
}

#define RS_THROW(Expression, what) throw Expression(what, __FILE__, __LINE__);
#define RS_THROW_FORMAT(Expression, f, ...) throw Expression(std::format(f, __VA_ARGS__), __FILE__, __LINE__);