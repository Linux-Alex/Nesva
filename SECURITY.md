# Security policy

Nesva handles backups of personal and business data, so security issues are treated as priority
work.

## Reporting a vulnerability

Do **not** open a public issue. Send a private report to the maintainers (for this repository:
the address configured in `CPACK_PACKAGE_CONTACT`, `security@nesva.invalid`) including:

- affected component and version (`nesva-cli status` output helps);
- reproduction steps or a proof of concept;
- impact: data loss, credential exposure, privilege escalation, remote code execution;
- whether the issue is already public.

You will get an acknowledgement within 5 working days and a remediation plan with a timeline.
Please allow time for a fix before public disclosure.

## Supported versions

Only the latest release line receives security fixes. Pre-release builds from `main` are not
supported for production use.

## Security design in this project

- **No credentials in the repository.** Nothing sensitive is committed; `.gitignore` excludes
  databases, secret files and environment files.
- **No credentials in logs.** Every record passes through the redactor (registered secrets,
  `key=value` assignments, Bearer/Basic credentials, PEM private keys, AWS/GitHub/JWT token
  shapes, sensitive JSON keys) before any sink writes it.
- **Secrets at rest** are references into Windows Credential Manager / Linux Secret Service;
  SQLite stores only the reference and the backend name.
- **Subprocesses** are started with an argument array through `QProcess` - no shell, so no
  command injection through filenames or settings.
- **No untrusted code execution.** Binaries are detected, never downloaded and executed at
  runtime. No scripts are executed automatically.
- **Destructive actions** are off by default (`allow_remote_deletions = 0`) and require explicit
  confirmation.
- **Telemetry**: none. Nesva does not transmit anything unless the user configures a destination
  and runs a job.
- **No custom cryptography** is implemented; encryption is delegated to Restic.
- **Least privilege**: the desktop app and the planned agent run as the invoking user; no
  elevation is required or requested.

## Dependency policy

- Qt, SQLite, Rclone and Restic are reviewed for licence compatibility (Apache-2.0) and known
  vulnerabilities before a release.
- Dependabot/dependency review runs on pull requests; findings must be triaged before release.
- Bundled binaries (if introduced in Phase 6) require pinned versions and published checksums.

## Known limitations (Phase 1)

- No job execution, no network access, no credential storage: the attack surface is limited to
  local file parsing and SQLite.
- Windows VSS is not implemented, so file consistency during a copy is not guaranteed (see
  ADR 0007). This is documented in the user-facing documentation rather than hidden.
