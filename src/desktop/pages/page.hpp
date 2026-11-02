// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QWidget>

class QLabel;
class QVBoxLayout;

namespace nesva::ui {

/// Common page chrome: title, subtitle and a body layout.
///
/// Pages are plain widgets hosted by the main window's QStackedWidget; they pull
/// their data from AppContext on refresh() rather than caching stale state.
class Page : public QWidget {
    Q_OBJECT

public:
    Page(QString title, QString subtitle, QWidget* parent = nullptr);

    QString pageTitle() const;
    void setSubtitle(const QString& subtitle);

    /// Reloads data from the database. Called on activation and periodically.
    virtual void refresh() {}

protected:
    QVBoxLayout* body() const { return bodyLayout_; }

private:
    QLabel* titleLabel_ = nullptr;
    QLabel* subtitleLabel_ = nullptr;
    QVBoxLayout* bodyLayout_ = nullptr;
};

} // namespace nesva::ui
