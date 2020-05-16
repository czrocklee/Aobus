/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once;

#include <exception>
#include <string>
#include <fmt/format.h>
#include <fmt/ostream.h>

namespace rs::common
{
  class Exception : public std::exception
  {
  public:
    Exception(const std::string& what, const char* file, int line) : _what{what}, _file{file}, _line{line} {}

    const char* file() const { return _file; }

    int line() const { return _line; }

    const char* what() const throw() override { return _what.c_str(); }

  private:
    const char* _file;
    int _line;
    std::string _what;
  };
}

#define RS_THROW(Expression, what) throw Expression(what, __FILE__, __LINE__);
#define RS_THROW_FORMAT(Expression, f, ...) throw Expression(::fmt::format(f, __VA_ARGS__), __FILE__, __LINE__);