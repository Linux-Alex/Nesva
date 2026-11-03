// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "entra_help_dialog.hpp"

#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>

namespace nesva::ui {

EntraHelpDialog::EntraHelpDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Register the Microsoft 365 app"));
    setModal(true);
    resize(720, 640);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);

    auto* browser = new QTextBrowser(this);
    browser->setOpenExternalLinks(true);
    browser->setHtml(QString::fromUtf8(R"HTML(
<h3>Register the app in Microsoft Entra ID</h3>
<p>Nesva signs in as a <b>public client</b>: no client secret is created or stored. The whole
registration is per-user; no administrator is required unless your tenant forbids user consent.</p>
<ol>
  <li>Open <b>portal.azure.com</b> &rarr; <b>Microsoft Entra ID</b> &rarr; <b>App registrations</b>
      &rarr; <b>New registration</b>.</li>
  <li>Name: <code>Nesva</code>. Supported account types:
      <b>Accounts in this organizational directory only</b>. Leave the redirect URI empty for now
      and register.</li>
  <li><b>Authentication</b> &rarr; <b>Add a platform</b> &rarr; <b>Mobile and desktop
      applications</b> &rarr; tick <b>http://localhost</b>.<br>
      Do <b>not</b> add it under <i>Web</i> and do not configure a <i>Single-page application</i>.</li>
  <li><b>Authentication</b> &rarr; <b>Advanced settings</b> &rarr; <b>Allow public client flows</b>
      = <b>Yes</b> &rarr; <b>Save</b>.<br>
      This step is mandatory: without it the token request fails with
      <code>AADSTS7000218</code> ("must contain client_assertion or client_secret").</li>
  <li><b>API permissions</b> &rarr; <b>Add a permission</b> &rarr; <b>Microsoft Graph</b> &rarr;
      <b>Delegated permissions</b> &rarr; add <b>Mail.Read</b> and <b>User.Read</b>.
      <code>openid</code>, <code>profile</code> and <code>offline_access</code> are requested at
      sign-in and need no entry here.</li>
  <li>Copy the <b>Application (client) ID</b> and the <b>Directory (tenant) ID</b> from the
      overview into the fields on this page, then press <b>Connect account...</b>.</li>
</ol>

<h3>Optional: restrict it to one mailbox</h3>
<p><b>Enterprise applications</b> &rarr; <b>Nesva</b> &rarr; <b>Properties</b> &rarr;
<b>Assignment required: Yes</b>, then assign the account.</p>

<h3>Troubleshooting</h3>
<ul>
  <li><b>AADSTS7000218</b> - <i>Allow public client flows</i> is not set to Yes, or a client secret
      was configured. Remove the secret; Nesva never sends one.</li>
  <li><b>AADSTS50011</b> (redirect mismatch) - add <code>http://localhost</code> under
      <i>Mobile and desktop applications</i>.</li>
  <li><b>AADSTS65001</b> (consent) - grant consent for the app, or have an administrator do it.</li>
  <li><b>AADSTS50105</b> - the account is not assigned to the application.</li>
  <li><b>AADSTS9002326</b> - the app is registered as a single-page application; move the loopback
      redirect to <i>Mobile and desktop applications</i>.</li>
</ul>

<p><b>Where secrets live:</b> the refresh token goes to the Windows Credential Manager (Linux: the
keyring) and the access token stays in memory. Nothing secret is written to the database, the
configuration or a log.</p>
)HTML"));
    layout->addWidget(browser, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

} // namespace nesva::ui
