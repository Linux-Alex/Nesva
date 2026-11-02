// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "activity_page.hpp"

#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtCore/QSortFilterProxyModel>
#include <QtWidgets/QTableView>

#include "app_context.hpp"
#include "models/event_list_model.hpp"
#include "nesva/core/paths.hpp"

namespace nesva::ui {

ActivityPage::ActivityPage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Activity"),
           QStringLiteral("Job and application events. The structured JSON log file holds the same "
                          "records with full diagnostic detail."),
           parent),
      context_(context) {
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter events (text, level, category, source)"));
    filterEdit_->setClearButtonEnabled(true);
    toolbar->addWidget(filterEdit_, 1);

    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    auto* openLogButton = new QPushButton(QStringLiteral("Open log folder"), this);
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(openLogButton);
    body()->addLayout(toolbar);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    body()->addWidget(summaryLabel_);

    model_ = new EventListModel(context_.database(), 500, this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(-1);

    view_ = new QTableView(this);
    view_->setModel(proxy_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->setWordWrap(false);
    view_->setSortingEnabled(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setSectionResizeMode(EventListModel::TimeColumn,
                                                     QHeaderView::ResizeToContents);
    view_->horizontalHeader()->setSectionResizeMode(EventListModel::LevelColumn,
                                                     QHeaderView::ResizeToContents);
    view_->horizontalHeader()->setSectionResizeMode(EventListModel::SourceColumn,
                                                     QHeaderView::ResizeToContents);
    view_->horizontalHeader()->setSectionResizeMode(EventListModel::CategoryColumn,
                                                     QHeaderView::ResizeToContents);
    body()->addWidget(view_, 1);

    connect(filterEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        proxy_->setFilterFixedString(text);
        summaryLabel_->setText(QStringLiteral("%1 of %2 events shown")
                                   .arg(proxy_->rowCount())
                                   .arg(model_->rowCount()));
    });
    connect(refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(openLogButton, &QPushButton::clicked, this, [] {
        const QString directory = Paths::logsDirectory();
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
}

void ActivityPage::refresh() {
    const QString error = model_->refresh();
    proxy_->setFilterFixedString(filterEdit_->text());

    if (!error.isEmpty()) {
        summaryLabel_->setText(QStringLiteral("Could not load events: %1").arg(error));
        return;
    }
    summaryLabel_->setText(QStringLiteral("%1 of %2 events shown \u2014 log file: %3")
                               .arg(proxy_->rowCount())
                               .arg(model_->rowCount())
                               .arg(context_.logFilePath()));
}

} // namespace nesva::ui
