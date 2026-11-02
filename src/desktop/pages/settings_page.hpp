// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include "page.hpp"

class QCheckBox;
class QComboBox;
class QLabel;

namespace nesva::ui {

class AppContext;

/// Application preferences that are real and persisted in this phase:
/// theme, log verbosity, destructive-action confirmation and path inspection.
class SettingsPage : public Page {
    Q_OBJECT

public:
    explicit SettingsPage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    void buildAppearanceSection();
    void buildLoggingSection();
    void buildSafetySection();
    void buildBehaviourSection();
    void buildAgentSection();
    void refreshAgentStatus();
    void buildLocationsSection();
    void showSaved();

    AppContext& context_;
    QComboBox* themeCombo_ = nullptr;
    QComboBox* logLevelCombo_ = nullptr;
    QCheckBox* confirmDestructive_ = nullptr;
    QCheckBox* minimizeToTrayCheck_ = nullptr;
    QCheckBox* notificationsCheck_ = nullptr;
    QLabel* agentStatusValue_ = nullptr;
    QLabel* savedLabel_ = nullptr;
    QLabel* dataRootValue_ = nullptr;
    QLabel* databaseValue_ = nullptr;
    QLabel* logFileValue_ = nullptr;
};

} // namespace nesva::ui
