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

#include <flatbuffers/reflection.h>
#include <ostream>

namespace rs::ml::gen
{
  using Fields = std::vector<const ::reflection::Field*>;

  class Generator
  {
  public:
    virtual ~Generator() = default;

    virtual std::string name() const = 0;

    virtual std::string outputFile() const = 0;

    virtual void run(std::ostream& os, const Fields& meta, const Fields& prop) = 0;
  };

}
