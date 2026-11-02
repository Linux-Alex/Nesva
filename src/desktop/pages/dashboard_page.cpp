// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "dashboard_page.hpp"

#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>
#include <QtWidgets/QVBoxLayout>

#include <utility>

#include "app_context.hpp"
#include "models/event_list_model.hpp"
#include "nesva/core/app_info.hpp"
#include "nesva/core/external_tool.hpp"

namespace nesva::ui {
namespace {

QString formattedTimestamp(const QString& isoTimestamp) {
    if (isoTimestamp.isEmpty()) {
        return QStringLiteral("\u2014");
    }
    QString value = isoTimestamp;
    value.replace(QLatin1Char('T'), QLatin1Char(' '));
    if (value.size() > 19) {
        value = value.left(19);
    }
    return value + QStringLiteral(" UTC");
}

int scalarInt(AppContext& context, const QString& sql, int fallback = 0) {
    Result<QSqlQuery> query = context.database().select(sql);
    if (!query) {
        return fallback;
    }
    QSqlQuery rows = std::move(query).take();
    if (!rows.next() || rows.value(0).isNull()) {
        return fallback;
    }
    return rows.value(0).toInt();
}

QString scalarText(AppContext& context, const QString& sql) {
    Result<QSqlQuery> query = context.database().select(sql);
    if (!query) {
        return QString();
    }
    QSqlQuery rows = std::move(query).take();
    if (!rows.next() || rows.value(0).isNull()) {
        return QString();
    }
    return rows.value(0).toString();
}

QLabel* makeLabel(const QString& text, const char* objectName, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(objectName));
    return label;
}

} // namespace

DashboardPage::DashboardPage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Dashboard"),
           QStringLiteral("Health of the local Nesva installation, its jobs and its storage."),
           parent),
      context_(context) {
    buildUi();
}

void DashboardPage::buildUi() {
    auto* cardsRow = new QHBoxLayout();
    cardsRow->setSpacing(12);

    const QStringList captions = {QStringLiteral("Jobs configured"),
                                  QStringLiteral("Last successful run"),
                                  QStringLiteral("Last failed run"),
                                  QStringLiteral("Next scheduled run"),
                                  QStringLiteral("Storage providers")};
    for (const QString& caption : captions) {
        auto* card = new QFrame(this);
        card->setObjectName(QStringLiteral("statCard"));
        card->setMinimumHeight(86);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 10, 14, 10);
        cardLayout->setSpacing(2);

        StatCard entry;
        entry.value = makeLabel(QStringLiteral("\u2014"), "statCardValue", card);
        entry.caption = makeLabel(caption, "statCardCaption", card);
        cardLayout->addWidget(entry.value);
        cardLayout->addWidget(entry.caption);
        cardsRow->addWidget(card, 1);
        cards_.append(entry);
    }
    body()->addLayout(cardsRow);

    auto* attentionHeader = new QHBoxLayout();
    attentionHeader->setSpacing(8);
    attentionHeader->addWidget(makeLabel(QStringLiteral("Attention"), "pageTitle", this));
    attentionHeader->addStretch(1);
    recheckButton_ = new QPushButton(QStringLiteral("Re-check environment"), this);
    attentionHeader->addWidget(recheckButton_);
    body()->addLayout(attentionHeader);

    auto* attentionCard = new QFrame(this);
    attentionCard->setObjectName(QStringLiteral("statCard"));
    auto* attentionLayout = new QVBoxLayout(attentionCard);
    attentionLayout->setContentsMargins(14, 12, 14, 12);
    attentionLabel_ = new QLabel(attentionCard);
    attentionLabel_->setWordWrap(true);
    attentionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    attentionLayout->addWidget(attentionLabel_);
    body()->addWidget(attentionCard);

    auto* activityHeader = new QHBoxLayout();
    activityHeader->setSpacing(8);
    activityHeader->addWidget(makeLabel(QStringLiteral("Recent activity"), "pageTitle", this));
    activityHeader->addStretch(1);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), this);
    activityHeader->addWidget(refreshButton_);
    body()->addLayout(activityHeader);

    recentModel_ = new EventListModel(context_.database(), 10, this);
    recentView_ = new QTableView(this);
    recentView_->setModel(recentModel_);
    recentView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    recentView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    recentView_->setAlternatingRowColors(true);
    recentView_->setWordWrap(false);
    recentView_->verticalHeader()->setVisible(false);
    recentView_->horizontalHeader()->setStretchLastSection(true);
    recentView_->horizontalHeader()->setSectionResizeMode(EventListModel::TimeColumn,
                                                          QHeaderView::ResizeToContents);
    recentView_->horizontalHeader()->setSectionResizeMode(EventListModel::LevelColumn,
                                                          QHeaderView::ResizeToContents);
    recentView_->horizontalHeader()->setSectionResizeMode(EventListModel::SourceColumn,
                                                          QHeaderView::ResizeToContents);
    recentView_->horizontalHeader()->setSectionResizeMode(EventListModel::CategoryColumn,
                                                          QHeaderView::ResizeToContents);
    recentView_->setMinimumHeight(180);
    body()->addWidget(recentView_, 1);

    connect(refreshButton_, &QPushButton::clicked, this, [this] { refresh(); });
    connect(recheckButton_, &QPushButton::clicked, this, [this] {
        probeTools(true);
        refresh();
    });
}

void DashboardPage::probeTools(bool force) {
    if (toolsProbed_ && !force) {
        return;
    }
    toolsProbed_ = true;
    toolWarnings_.clear();

    const QString rcloneConfigured = [&] {
        const Result<QString> value = context_.settings().getString(
            QString::fromUtf8(config::keys::kRcloneBinaryPath));
        return value && !value.value().isEmpty() ? value.value() : QStringLiteral("rclone");
    }();
    const QString resticConfigured = [&] {
        const Result<QString> value = context_.settings().getString(
            QString::fromUtf8(config::keys::kResticBinaryPath));
        return value && !value.value().isEmpty() ? value.value() : QStringLiteral("restic");
    }();

    const Result<ExternalToolInfo> rclone =
        probeExternalTool(rcloneConfigured, {QStringLiteral("version")});
    if (!rclone) {
        toolWarnings_.append(QStringLiteral(
            "Rclone was not found, so archive uploads to remote storage are unavailable. "
            "Install Rclone or set its path in Settings (planned tool-path setting)."));
    }

    const Result<ExternalToolInfo> restic =
        probeExternalTool(resticConfigured, {QStringLiteral("version")});
    if (!restic) {
        toolWarnings_.append(QStringLiteral(
            "Restic was not found, so encrypted versioned backups are unavailable until it is installed."));
    }
}

QStringList DashboardPage::collectAttention() const {
    QStringList issues;

    if (!context_.database().isOpen()) {
        issues.append(QStringLiteral("The database is not open."));
        return issues;
    }

    const Result<int> schema = context_.database().schemaVersion();
    if (!schema) {
        issues.append(QStringLiteral("The database schema version could not be read."));
    } else if (schema.value() != db::Database::latestSchemaVersion()) {
        issues.append(QStringLiteral("Database schema %1 is not the expected version %2.")
                          .arg(schema.value())
                          .arg(db::Database::latestSchemaVersion()));
    }

    issues.append(toolWarnings_);

    const QString failedJob =
        scalarText(context_, QStringLiteral(
                                  "SELECT j.name || ' failed at ' || COALESCE(r.finished_at, 'unknown') "
                                  "|| COALESCE(': ' || r.error_message, '') "
                                  "FROM job_runs r JOIN backup_jobs j ON j.id = r.job_id "
                                  "WHERE r.status = 'failed' ORDER BY r.id DESC LIMIT 1"));
    if (!failedJob.isEmpty()) {
        issues.append(QStringLiteral("Most recent failure: %1").arg(failedJob));
    }

    const QString authRequired = scalarText(
        context_, QStringLiteral("SELECT name FROM storage_providers WHERE status = 'auth_required' "
                                 "ORDER BY name LIMIT 1"));
    if (!authRequired.isEmpty()) {
        issues.append(QStringLiteral("Storage provider '%1' requires reconnection.").arg(authRequired));
    }

    if (issues.isEmpty()) {
        issues.append(QStringLiteral(
            "No issues detected. Jobs, schedules and storage connections arrive in later phases."));
    }
    return issues;
}

void DashboardPage::refresh() {
    if (!context_.database().isOpen()) {
        return;
    }

    probeTools(false);

    const int jobCount = scalarInt(context_, QStringLiteral("SELECT COUNT(*) FROM backup_jobs"));
    const int storageCount =
        scalarInt(context_, QStringLiteral("SELECT COUNT(*) FROM storage_providers"));
    const QString lastSuccess = scalarText(
        context_,
        QStringLiteral("SELECT MAX(finished_at) FROM job_runs WHERE status = 'succeeded'"));
    const QString lastFailure = scalarText(
        context_,
        QStringLiteral("SELECT MAX(finished_at) FROM job_runs WHERE status = 'failed'"));
    const QString nextRun = scalarText(
        context_, QStringLiteral("SELECT MIN(next_run_at) FROM job_schedules "
                                 "WHERE next_run_at IS NOT NULL"));

    const QString values[] = {QString::number(jobCount), formattedTimestamp(lastSuccess),
                              formattedTimestamp(lastFailure), formattedTimestamp(nextRun),
                              QString::number(storageCount)};
    for (int index = 0; index < cards_.size() && index < 5; ++index) {
        cards_.at(index).value->setText(values[index]);
    }

    QStringList lines;
    for (const QString& issue : collectAttention()) {
        lines.append(QStringLiteral("\u2022 %1").arg(issue));
    }
    attentionLabel_->setText(lines.join(QLatin1Char('\n')));
    attentionLabel_->setToolTip(QStringLiteral("Checked at %1")
                                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate)));

    recentModel_->refresh();
}

} // namespace nesva::ui
