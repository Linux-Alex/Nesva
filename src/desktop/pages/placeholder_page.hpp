// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QStringList>

#include "page.hpp"

namespace nesva::ui {

/// Page used for capabilities that are planned but not part of the current
/// delivery phase. It states exactly what exists today and what will arrive,
/// instead of presenting inactive controls that pretend to work.
class PlaceholderPage : public Page {
    Q_OBJECT

public:
    PlaceholderPage(QString title, QString subtitle, QString headline, QStringList planned,
                    QString availability, QWidget* parent = nullptr);
};

} // namespace nesva::ui
