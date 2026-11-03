// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QWidget>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace nesva::ui {

/// Common page chrome: title, subtitle and a body layout.
///
/// The body lives inside a vertical scroll area, so a page with more cards than
/// the window is tall scrolls instead of squeezing every card below its natural
/// size. Pages whose body is a single table still let the table fill the
/// viewport: the scroll area resizes its content to at least the viewport, and
/// the table's own scrolling takes over beyond that.
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

    /// Scrolls to the widget that was just added, for pages that build their
    /// content incrementally and want to reveal the newest part.
    void scrollToBottom();

private:
    QLabel* titleLabel_ = nullptr;
    QLabel* subtitleLabel_ = nullptr;
    QVBoxLayout* bodyLayout_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
};

} // namespace nesva::ui
