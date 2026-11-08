# Nesva presentation & tutorial (bilingual)

`Nesva-0.6.6-SL-EN.pdf` is a 16-page document: a pitch plus a complete tutorial, in Slovenian
(pages 2-8) followed by English (pages 9-16). It covers what Nesva is, installation on Windows and
Linux, every GUI page, external tools (Rclone, Restic), storage, archive and backup jobs,
Microsoft 365 mail, schedules and the background agent, restore, maintenance, updates, security and
the whole command line.

Rebuild after editing `Nesva-0.6.6-SL-EN.html`:

```bash
google-chrome --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
  --print-to-pdf=Nesva-0.6.6-SL-EN.pdf Nesva-0.6.6-SL-EN.html
```

The HTML is the source: A4 page size, print colours, margins and page breaks are all in the
embedded stylesheet, so any Chromium-based browser produces the same document.
