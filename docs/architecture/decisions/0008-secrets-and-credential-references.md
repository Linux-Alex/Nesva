# 0008. Secrets live in the OS keyring; SQLite stores references

- Status: Accepted
- Date: 2025-11-02

## Context

Nesva will hold provider passwords, Rclone-obscured trust tokens, Restic repository passwords and
(later) Microsoft OAuth refresh tokens. Storing them in SQLite or config files in plaintext is
not acceptable, and logging them is categorically forbidden.

## Decision

- A secret is never written to SQLite. `storage_credentials_refs` stores only a
  `keyring_backend` (`windows_credential_manager`, `linux_secret_service`, `macos_keychain`,
  `none`) and a `secret_reference` (service/account identifier).
- Keyring integration is implemented in Phase 2 alongside the first provider that needs a
  secret; until then, the schema can express a reference but nothing can store a secret.
- Every secret read into memory is registered with `Logger::addSecret()` so the redactor censors
  it even if it appears in third-party output (e.g. an Rclone error message).
- Passwords for external tools are passed via environment variables or temporary files with
  restrictive permissions, never as command-line arguments that would land in process lists.
- Microsoft 365 authentication uses the delegated authorisation-code flow with PKCE and a public
  client; refresh tokens go to the keyring, and the user always authenticates locally with Apple
  or Microsoft - never with a password Nesva stores.
- Telemetry: none. No diagnostic export leaves the machine unless the user saves it.

## Consequences

- Users moving a database between machines must re-enter secrets - correct and expected.
- Headless/agent execution needs an unlocked keyring, which is documented (ADR for Phase 3).
- Tests use fake secrets to prove redaction (`tst_redactor`, `tst_logging`) rather than real
  credentials.

## Alternatives considered

- **Encrypting secrets inside SQLite**: requires key management, invites "just use a password"
  shortcuts, and duplicates what the OS keyring already does correctly.
- **Environment-only configuration**: workable for CI but not for a desktop application.
