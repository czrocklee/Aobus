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

#pragma once

#include <rs/ml/utility/IndentingOStreambuf.H>
#include <boost/format.hpp>
#include <iostream>

namespace rs::ml::utility
{
  class CppCodeWriter
  {
  public:
    using IndentGuard = IndentingOStreambuf::Guard;

    CppCodeWriter(std::ostream& os) : _os{os}, _isb{*os.rdbuf()} { os.rdbuf(&_isb); }

    ~CppCodeWriter() { _os.rdbuf(&_isb.buf()); }

    IndentGuard indent(int level = 1) { return {_isb, level}; }

    template<typename... Args>
    CppCodeWriter& operator()(const char* format, Args&&... args) { _os << (boost::format{format} % ... % args) << '\n'; return *this; }

    std::ostream& os() { return _os; }

  private:
    std::ostream& _os;
    IndentingOStreambuf _isb;
  };

  template<typename T>
  inline CppCodeWriter& operator << (CppCodeWriter& writer, T&& t)
  {
    writer.os() << std::forward<T>(t);
    return writer;
  }

  template<typename T>
  inline CppCodeWriter& operator | (CppCodeWriter& writer, T&& t)
  {
    writer << std::forward<T>(t) << '\n';
    return writer;
  }
}
