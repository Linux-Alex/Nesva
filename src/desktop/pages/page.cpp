// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "page.hpp"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <utility>

namespace nesva::ui {

Page::Page(QString title, QString subtitle, QWidget* parent) : QWidget(parent) {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 20, 24, 20);
    rootLayout->setSpacing(12);

    titleLabel_ = new QLabel(std::move(title), this);
    titleLabel_->setObjectName(QStringLiteral("pageTitle"));

    subtitleLabel_ = new QLabel(std::move(subtitle), this);
    subtitleLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    subtitleLabel_->setWordWrap(true);

    rootLayout->addWidget(titleLabel_);
    rootLayout->addWidget(subtitleLabel_);

    bodyLayout_ = new QVBoxLayout();
    bodyLayout_->setContentsMargins(0, 8, 0, 0);
    bodyLayout_->setSpacing(12);
    rootLayout->addLayout(bodyLayout_, 1);
}

QString Page::pageTitle() const {
    return titleLabel_->text();
}

void Page::setSubtitle(const QString& subtitle) {
    subtitleLabel_->setText(subtitle);
}

} // namespace nesva::ui
