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


#include <rs/ml/core/MusicLibrary.h>
#include <rs/ml/expr/Expression.h>
#include <QtCore/QSortFilterProxyModel>

class TrackSortFilterProxyModel : public QSortFilterProxyModel
{
  Q_OBJECT

public:
  using QSortFilterProxyModel::QSortFilterProxyModel;
  //TrackSortFilterProxyModel(rs::ml::core::MusicLibrary& ml, QObject *parent = 0);

public slots:
  void onQuickFilterChanged(const QString& filter);

protected:
  bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
  std::optional<rs::ml::expr::Expression> _filter;
  std::string _quick;
};
