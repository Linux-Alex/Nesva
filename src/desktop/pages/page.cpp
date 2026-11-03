// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "page.hpp"

#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
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

    // The body is scrollable from the start: a page whose content is taller
    // than the window must scroll, not compress its cards until the buttons and
    // labels are unreadable. Content is resized to at least the viewport, so a
    // table-only page still fills the page.
    auto* content = new QWidget(this);
    content->setObjectName(QStringLiteral("pageBodyContent"));
    bodyLayout_ = new QVBoxLayout(content);
    bodyLayout_->setContentsMargins(0, 8, 0, 0);
    bodyLayout_->setSpacing(12);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setObjectName(QStringLiteral("pageBodyScroll"));
    scrollArea_->setWidget(content);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->viewport()->setAutoFillBackground(false);

    rootLayout->addWidget(scrollArea_, 1);
}

QString Page::pageTitle() const {
    return titleLabel_->text();
}

void Page::setSubtitle(const QString& subtitle) {
    subtitleLabel_->setText(subtitle);
}

void Page::scrollToBottom() {
    if (scrollArea_ == nullptr) {
        return;
    }
    QScrollBar* bar = scrollArea_->verticalScrollBar();
    bar->setValue(bar->maximum());
}

} // namespace nesva::ui
