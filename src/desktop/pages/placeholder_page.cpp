// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "placeholder_page.hpp"

#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <utility>

namespace nesva::ui {

PlaceholderPage::PlaceholderPage(QString title, QString subtitle, QString headline,
                                 QStringList planned, QString availability, QWidget* parent)
    : Page(std::move(title), std::move(subtitle), parent) {
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("statCard"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 16, 18, 18);
    cardLayout->setSpacing(10);

    auto* headlineLabel = new QLabel(std::move(headline), card);
    headlineLabel->setWordWrap(true);
    headlineLabel->setTextFormat(Qt::PlainText);
    cardLayout->addWidget(headlineLabel);

    if (!planned.isEmpty()) {
        QStringList bullets;
        bullets.reserve(planned.size());
        for (const QString& item : planned) {
            bullets.append(QStringLiteral("\u2022 %1").arg(item));
        }
        auto* listLabel = new QLabel(bullets.join(QLatin1Char('\n')), card);
        listLabel->setWordWrap(true);
        listLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cardLayout->addWidget(listLabel);
    }

    auto* availabilityLabel = new QLabel(std::move(availability), card);
    availabilityLabel->setObjectName(QStringLiteral("pageSubtitle"));
    availabilityLabel->setWordWrap(true);
    cardLayout->addWidget(availabilityLabel);

    body()->addWidget(card);
    body()->addStretch(1);
}

} // namespace nesva::ui
