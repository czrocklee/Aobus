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


#include <rs/ml/core/TrackList.H>
#include <QAbstractTableModel>

class TableModel : public QAbstractTableModel, private rs::ml::core::TrackList::Observer
{
  using TrackId = rs::ml::core::TrackId;
  using TrackT = rs::ml::core::TrackT;
  Q_OBJECT

public:
  TableModel(QObject *parent = 0);
  TableModel(rs::ml::core::TrackList& list, QObject *parent = 0);

  int rowCount(const QModelIndex &parent) const override;
  int columnCount(const QModelIndex &parent) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
  bool insertRows(int position, int rows, const QModelIndex &index = QModelIndex()) override;
  bool removeRows(int position, int rows, const QModelIndex &index = QModelIndex()) override;

  QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;

//    QList<Contact> getContacts() const;
public slots:
  void onQuickFilterChanged(const QString& filter);

private:
  void onCreate(rs::ml::core::TrackId, const rs::ml::core::TrackT&, std::size_t index) override;  
  void onModify(rs::ml::core::TrackId, const rs::ml::core::TrackT&, std::size_t index) override;
  void onRemove(rs::ml::core::TrackId, const rs::ml::core::TrackT&, std::size_t index) override;

private:
  rs::ml::core::TrackList& _list;
};
