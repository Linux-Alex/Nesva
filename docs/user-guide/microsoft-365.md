# Archiving Microsoft 365 mail

Nesva can archive the mail of a Microsoft 365 mailbox to local `.eml` files. This is
**archive** semantics: an ordinary, readable copy of each message. It is not a versioned or
immutable backup - if you want that, add the mail archive folder as a source of a
[Restic backup job](../architecture/decisions/0012-restic-integration-and-credentials.md) after
the first sync.

Nesva reads mail with the delegated scope `Mail.Read`. It never sends, never deletes and never
modifies anything in the mailbox.

---

## 1. Create an app registration

Nesva is a **public client**: it has no client secret and never asks for one, because a secret
shipped inside a desktop application is not a secret. Sign-in uses the authorization code flow
with PKCE over a loopback redirect, which is the flow Microsoft recommends for desktop apps.

You can register the app yourself. It is a user-scoped app (never a tenant-wide grant) and needs
no administrator if your tenant allows user consent for `Mail.Read`.

1. Open <https://portal.azure.com> and go to **Microsoft Entra ID -> App registrations -> New
   registration**.
2. Name: `Nesva`. Supported account types: **Accounts in this organizational directory only**.
3. Leave **Redirect URI** empty for now and register.
4. Open **Authentication -> Add a platform -> Mobile and desktop applications** and tick
   **`http://localhost`**. Do not add it under *Web* and do not configure a *Single-page
   application*.
5. Open **Authentication -> Advanced settings -> Allow public client flows** and set it to
   **Yes**, then **Save**.
6. Open **API permissions -> Add a permission -> Microsoft Graph -> Delegated permissions** and
   add **`Mail.Read`** and **`User.Read`**. `offline_access`, `openid` and `profile` are
   requested at sign-in and need no entry here.
7. Note the **Application (client) ID** and the **Directory (tenant) ID** from the overview.

> Step 5 is not optional. Without it Entra treats the registration as a confidential client and
> the token request fails with `AADSTS7000218`. See [the troubleshooting table](#troubleshooting).

If you want the app to be usable by one account only, open **Enterprise applications -> Nesva ->
Properties -> Assignment required: Yes** and assign that account.

---

## 2. Connect the account

```bash
nesva-cli mail connect \
  --account you@example.com \
  --tenant <directory-id> \
  --client <application-client-id>
```

The command prints a sign-in URL (and opens your browser). Sign in, and the browser shows
"Nesva is connected". The refresh token is written to the OS keyring - Linux: the Secret Service
(`org.freedesktop.secrets`), Windows: Credential Manager - under
`nesva/msgraph/<tenant>/<upn>`. The access token lives in memory only and is discarded when the
process exits. Nothing secret is written to the database, the configuration or the logs.

```bash
nesva-cli mail accounts      # account id, status, folder count
nesva-cli mail folders 1     # discovered folders, and whether each is enabled
```

Unrelated, system folders (Sync issues, Conversation history, ...) are stored but not enabled by
default:

```bash
nesva-cli mail folders 1 --enable inbox --enable archive --disable drafts
```

---

## 3. Sync

```bash
nesva-cli mail sync 1 --to ~/Archive/Mail
```

Messages land at `<folder>/<YYYY>/<MM>/<subject>-<id12>.eml`, written atomically through a
temporary file and recorded with their SHA-256. Each run:

- uses the per-folder **delta** feed, so only new and changed messages are downloaded;
- resumes from a cursor that is advanced page by page, so an interrupted run does not start over;
- skips a message whose `lastModifiedDateTime` is unchanged since the last download;
- keeps the local file when a message disappears from the mailbox, and marks the row `deleted`,
  so archiving never destroys evidence.

Useful flags: `--full` (ignore the cursor), `--folder <remote-id>`,
`--max-messages <n>` (bound a first run), `--dry-run` (list, download nothing).

---

## 4. Scheduling

Mail archival is a normal Nesva job, so it uses the same scheduler as file archiving and Restic
backups. Scheduled mail jobs are driven by `nesva-agent`; the desktop scheduling UI for mail
accounts arrives with the Phase 5 UI work. Until then, run `mail sync` from cron or a systemd
timer if you need it automated, and keep the same `--to` directory.

---

## 5. Troubleshooting

Every failure Nesva reports for the token endpoint leads with the fix and keeps Microsoft's raw
text (with the trace id) after it.

| Code | Meaning | Fix |
| --- | --- | --- |
| `AADSTS7000218` | The app registration is treated as a confidential client. | `Authentication -> Allow public client flows: Yes`, and keep the loopback redirect under *Mobile and desktop applications*. Never add a secret. |
| `AADSTS50011` | Redirect URI mismatch. | Add `http://localhost` under *Mobile and desktop applications*. Nesva retries once with the port-less form automatically. |
| `AADSTS900971` | No reply address registered. | Same as above: register `http://localhost`. |
| `AADSTS700016` | Client id unknown in this tenant. | Check `--client` and `--tenant`. |
| `AADSTS65001` | Consent missing for `Mail.Read`. | Grant consent, or have an administrator consent for the app. |
| `AADSTS50105` | The account is not assigned to the app. | Assign the account, or turn *Assignment required* off. |
| `AADSTS70008` / `AADSTS54005` | Authorization code expired or already redeemed. | Run the connect command again; a code is single-use. |
| `AADSTS9002326` | The app is configured as a single-page application. | Register the loopback redirect under *Mobile and desktop applications*. |

Other things worth knowing:

- A browser that resolves `localhost` to IPv6 works: Nesva listens on both `127.0.0.1` and
  `::1` on the same ephemeral port.
- If the keyring is unavailable, `mail connect` fails instead of falling back to a plaintext
  file. Start a Secret Service provider (kwalletd, gnome-keyring) and retry.
- To revoke access, delete the app's consent in
  <https://myapps.microsoft.com> and remove the keyring entry; no message data is affected.
