# Nesva presentation & tutorial

Two documents, one per language:

| File | Language | Pages |
| --- | --- | --- |
| `Nesva-0.6.6-SL.pdf` | Slovenian | 8 |
| `Nesva-0.6.6-EN.pdf` | English | 9 |

Each one is a pitch followed by a complete tutorial: what Nesva is and what archive mode and
backup mode promise, installation on Windows and Linux, every GUI page, the external tools
(Rclone, Restic), storage and remotes, archive and versioned backup jobs, Microsoft 365 mail,
schedules and the background agent, restore, maintenance, updates, security, the whole command
line, and troubleshooting.

The HTML next to each PDF is the source. Rebuild after editing:

```bash
google-chrome --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
  --print-to-pdf=Nesva-0.6.6-SL.pdf Nesva-0.6.6-SL.html
google-chrome --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
  --print-to-pdf=Nesva-0.6.6-EN.pdf Nesva-0.6.6-EN.html
```

A4 page size, print colours, margins and page breaks are in the embedded stylesheet, so any
Chromium-based browser produces the same document.
