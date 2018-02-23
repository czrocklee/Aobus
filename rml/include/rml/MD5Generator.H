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

#include <openssl/md5.h>
#include <istream>

namespace rml
{
  std::array<std::uint8_t, 16> calculateMD5(std::istream& is)
  {
    constexpr std::size_t BufferSize = 1024 * 1024;
    std::array<std::uint8_t, 16> digest;
    std::streamsize bytes = 0;
    MD5_CTX mdContext;
    MD5_Init (&mdContext);

    do
    {
      char buffer[BufferSize];
      bytes = is.readsome(buffer, BufferSize);
      MD5_Update (&mdContext, buffer, bytes);
    }
    while (is && bytes > 0);

    MD5_Final (digest.data(), &mdContext);
    return digest;
  }
}



