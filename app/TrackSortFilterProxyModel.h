// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <QtCore/QSortFilterProxyModel>
#include <rs/core/MusicLibrary.h>
#include <rs/expr/Expression.h>

class TrackSortFilterProxyModel : public QSortFilterProxyModel
{
  Q_OBJECT

public:
  using QSortFilterProxyModel::QSortFilterProxyModel;
  // TrackSortFilterProxyModel(rs::core::MusicLibrary& ml, QObject *parent = 0);

public slots:
  void onQuickFilterChanged(QString const& filter);

protected:
  bool filterAcceptsRow(int sourceRow, QModelIndex const& sourceParent) const override;

private:
  std::optional<rs::expr::Expression> _filter;
  std::string _quick;
};
