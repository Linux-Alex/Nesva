// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

namespace nesva::ui {

/// In-application instructions for registering the Entra ID application Nesva
/// signs in with. Self-contained on purpose: the steps are needed exactly when
/// the user is in Settings, and on Windows the repository documentation is not
/// beside the executable.
class EntraHelpDialog : public QDialog {
    Q_OBJECT

public:
    explicit EntraHelpDialog(QWidget* parent = nullptr);
};

} // namespace nesva::ui
