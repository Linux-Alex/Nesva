# 0013. Microsoft 365 archival: OAuth PKCE, Graph delta sync and MIME export

- Status: Accepted
- Date: 2026-11-02

## Context

Phase 5 archives Microsoft 365 mail as individual .eml files. Requirements: delegated OAuth with
PKCE, least-privilege permissions, secure token storage, incremental synchronisation that is
restartable without duplication, throttling handling, a deleted/moved message policy, and never
deleting mail in Microsoft 365.

## Decision

**Public client, PKCE, loopback redirect**

- Authorization-code flow with PKCE (S256). No client secret: the app registration is a public
  desktop client, so there is nothing to leak.
- The redirect is a loopback listener (RFC 8252) bound to ephemeral ports on 127.0.0.1 and ::1
  (browsers resolve "localhost" to either family). The redirect URI is the bare
  `http://localhost:<port>/` form, which matches the registered `http://localhost` exactly;
  Microsoft ignores the port for loopback URIs.
- CSRF: a random `state` is verified before the code is accepted, and AAD error responses are
  surfaced as errors instead of leaving the process waiting.
- If a tenant rejects the loopback URI with `AADSTS50011`, the token request is retried once with
  the bare `http://localhost`.

**Tokens**

- The refresh token lives in the OS keyring (Secret Service / Credential Manager) under
  `nesva/msgraph/<tenant>/<upn>`. Access tokens stay in memory only. Neither is written to
  SQLite, to a log or to a command line.
- A rotated refresh token (some tenants rotate) is persisted immediately.

**Graph usage**

- Read-only endpoints: `/me`, `/me/mailFolders` (+ `/childFolders`), the per-folder
  `/messages/delta` feed, and `/me/messages/{id}/$value` with `Accept: message/rfc822`.
  Nothing in the codebase can modify, move or delete mail.
- Throttling: 429 honours `Retry-After` (capped), 5xx retries with backoff, 401 refreshes the
  access token once and then fails clearly.

**Incremental export**

- Each folder stores its delta cursor in `mail_folders.delta_link`; the exporter persists the
  `@odata.nextLink` after every page and the `@odata.deltaLink` at the end, so an interrupted run
  resumes from the last page instead of starting over.
- A message is re-downloaded only when `lastModifiedDateTime` differs from the recorded value;
  otherwise it counts as skipped. That is what makes repeated runs cheap and duplicate-free.
- `@removed` entries mark the local row `skipped` and keep the .eml that already exists locally:
  Nesva never deletes an archive because Microsoft 365 deleted a message.
- Files land at `<folder>/<YYYY>/<MM>/<sanitised-subject>-<12 hex of message key>.eml`, written
  through `QSaveFile` (atomic), and their SHA-256 is recorded in `mail_messages`.

**Identity**

- The stable identity is `internetMessageId` with the remote id as fallback; both are stored, and
  the message key determines the file name, so a re-export overwrites the same file rather than
  creating duplicates.

## Consequences

- The user authenticates locally with Microsoft: Nesva never sees the password.
- Revocation is immediate: remove the user assignment or delete the registration; the next run
  fails with "authorization required" instead of silently succeeding.
- Tenants that block user consent need one admin consent for this app; with assignment required
  only the assigned user can obtain tokens.
- Windows token storage uses the Credential Manager backend, which is written but not yet
  exercised on a Windows machine.

## Alternatives considered

- **Client credentials / application permissions**: would act without a user and could reach any
  mailbox; rejected on least-privilege grounds.
- **Device code flow**: simpler to implement, but requires a browserless UX and grants the same
  scopes; the loopback flow is the standard for desktop apps with a browser available.
- **Storing the refresh token in SQLite**: forbidden by the design rules (ADR 0008).
- **Re-downloading the mailbox on every run**: wasteful and would re-write thousands of files.
