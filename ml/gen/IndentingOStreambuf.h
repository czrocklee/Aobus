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

#include <iostream>
#include <string>

namespace rs::ml::gen
{
  // https://stackoverflow.com/questions/9599807/how-to-add-indention-to-the-stream-operator

  class IndentingOStreambuf : public std::streambuf
  {
  public:
    explicit IndentingOStreambuf(std::streambuf& buf) : _buf{buf} { }

    std::streambuf& buf() { return _buf; }

    void adjust(int level) { _indent.resize(_indent.size() + 2 * level, ' '); }

    class Guard;

  protected:
    int overflow(int ch) override
    {
      if (_isAtStartOfLine && ch != '\n' && !_indent.empty())
      {
        _buf.sputn(_indent.data(), _indent.size());
      }

      _isAtStartOfLine = (ch == '\n');
      return _buf.sputc(ch);
    }

  private:
    std::streambuf& _buf;
    std::string _indent;
    bool _isAtStartOfLine = true;
  };

  class IndentingOStreambuf::Guard
  {
  public:
    Guard(IndentingOStreambuf& isb, int level) 
      : _isb{isb}, _level{level} 
    { 
      _isb.adjust(_level); 
    } 

    ~Guard() 
    { 
      _isb.adjust(-1 * _level); 
    }

  private:
    IndentingOStreambuf& _isb;
    int _level;
  };
}



