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

#include <rs/ml/query/Expression.h>
#include <rs/ml/Protocol.capnp.h>

namespace rs::ml::query
{
  class JitFilter
  {
  public:
    explicit JitFilter(RootExpression&& root);

    bool operator()(const Track::Reader& track) const;

    void dump() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
  };
}
