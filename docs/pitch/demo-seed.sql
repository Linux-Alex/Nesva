PRAGMA foreign_keys = ON;
BEGIN;
DELETE FROM job_runs; DELETE FROM backup_sources; DELETE FROM backup_exclusions; DELETE FROM job_schedules; DELETE FROM backup_jobs; DELETE FROM storage_providers; DELETE FROM mail_accounts;

INSERT INTO storage_providers (id, name, provider_type, rclone_remote, base_path, status, status_detail, last_checked_at, created_at, updated_at)
VALUES (1, 'iCloud Nesva', 'rclone', 'icloud:', 'Nesva/Archive', 'ok', 'probe ok', datetime('now','-2 hours'), datetime('now','-40 days'), datetime('now','-2 hours'));
INSERT INTO storage_providers (id, name, provider_type, rclone_remote, base_path, status, status_detail, last_checked_at, created_at, updated_at)
VALUES (2, 'Backblaze B2', 'rclone', 'b2kraken26:', 'Nesva/Archive', 'ok', 'probe ok', datetime('now','-3 hours'), datetime('now','-35 days'), datetime('now','-3 hours'));
INSERT INTO storage_providers (id, name, provider_type, rclone_remote, base_path, status, status_detail, last_checked_at, created_at, updated_at)
VALUES (3, 'Zunanji disk Seagate', 'local', NULL, '/media/demo/Seagate/Nesva', 'ok', 'writable', datetime('now','-1 hours'), datetime('now','-30 days'), datetime('now','-1 hours'));

INSERT INTO backup_jobs (id, name, mode, storage_provider_id, destination_path, enabled, bandwidth_limit_kib, retention_policy_json, verification_options_json, created_at, updated_at, source_kind)
VALUES (1, 'Dokumenti in Namizje', 'archive', 1, NULL, 1, NULL, '{}', '{"verify":true}', datetime('now','-40 days'), datetime('now','-1 day'), 'filesystem');
INSERT INTO backup_sources (job_id, path, is_primary, created_at, label) VALUES (1, '/home/demo/Documents', 1, datetime('now','-40 days'), 'Dokumenti');
INSERT INTO backup_sources (job_id, path, is_primary, created_at, label) VALUES (1, '/home/demo/Desktop', 1, datetime('now','-40 days'), 'Namizje');
INSERT INTO backup_exclusions (job_id, pattern, match_kind, created_at) VALUES (1, '*.tmp', 'glob', datetime('now','-40 days'));
INSERT INTO backup_exclusions (job_id, pattern, match_kind, created_at) VALUES (1, 'Thumbs.db', 'glob', datetime('now','-40 days'));

INSERT INTO backup_jobs (id, name, mode, storage_provider_id, destination_path, enabled, bandwidth_limit_kib, retention_policy_json, verification_options_json, created_at, updated_at, source_kind)
VALUES (2, 'Fotografije (tedensko)', 'archive', 2, NULL, 1, 2048, '{}', '{"verify":true}', datetime('now','-30 days'), datetime('now','-2 days'), 'filesystem');
INSERT INTO backup_sources (job_id, path, is_primary, created_at, label) VALUES (2, '/home/demo/Pictures', 1, datetime('now','-30 days'), 'Fotografije');

INSERT INTO backup_jobs (id, name, mode, storage_provider_id, destination_path, enabled, bandwidth_limit_kib, retention_policy_json, verification_options_json, created_at, updated_at, source_kind)
VALUES (3, 'Varnostna kopija Dokumentov', 'backup', 2, 'Nesva/Restic/dokumenti', 1, NULL, '{"keep_daily":7,"keep_weekly":4,"keep_monthly":12}', '{}', datetime('now','-28 days'), datetime('now','-1 day'), 'filesystem');
INSERT INTO backup_sources (job_id, path, is_primary, created_at, label) VALUES (3, '/home/demo/Documents', 1, datetime('now','-28 days'), 'Dokumenti');

INSERT INTO backup_jobs (id, name, mode, storage_provider_id, destination_path, enabled, bandwidth_limit_kib, retention_policy_json, verification_options_json, created_at, updated_at, source_kind)
VALUES (4, 'Pošta v arhiv', 'archive', 2, 'Mail', 1, NULL, '{}', '{}', datetime('now','-20 days'), datetime('now','-6 hours'), 'mail');

INSERT INTO job_schedules (job_id, trigger_kind, time_of_day, timezone, retry_max_attempts, retry_backoff_seconds, catch_up_missed, next_run_at, last_run_at, created_at, updated_at)
VALUES (1, 'daily', '03:00', 'Europe/Ljubljana', 3, 900, 1, datetime('now','+1 day','+3 hours'), datetime('now','-21 hours'), datetime('now','-40 days'), datetime('now','-1 day'));
INSERT INTO job_schedules (job_id, trigger_kind, time_of_day, timezone, retry_max_attempts, retry_backoff_seconds, catch_up_missed, next_run_at, last_run_at, created_at, updated_at)
VALUES (2, 'weekly', '04:30', 'Europe/Ljubljana', 3, 900, 1, datetime('now','+5 days'), datetime('now','-2 days'), datetime('now','-30 days'), datetime('now','-2 days'));
INSERT INTO job_schedules (job_id, trigger_kind, time_of_day, timezone, retry_max_attempts, retry_backoff_seconds, catch_up_missed, next_run_at, last_run_at, created_at, updated_at)
VALUES (3, 'daily', '02:00', 'Europe/Ljubljana', 3, 900, 1, datetime('now','+1 day','+2 hours'), datetime('now','-22 hours'), datetime('now','-28 days'), datetime('now','-1 day'));
INSERT INTO job_schedules (job_id, trigger_kind, time_of_day, timezone, retry_max_attempts, retry_backoff_seconds, catch_up_missed, next_run_at, last_run_at, created_at, updated_at)
VALUES (4, 'daily', '03:30', 'Europe/Ljubljana', 3, 900, 1, datetime('now','+1 day','+4 hours'), datetime('now','-20 hours'), datetime('now','-20 days'), datetime('now','-6 hours'));

INSERT INTO job_runs (job_id, run_number, trigger_kind, status, started_at, finished_at, files_total, files_transferred, files_skipped, files_failed, bytes_total, bytes_transferred, created_at, owner_host)
VALUES (1, 12, 'schedule', 'succeeded', datetime('now','-21 hours'), datetime('now','-20 hours','-38 minutes'), 14238, 312, 13926, 0, 73400320, 5242880, datetime('now','-21 hours'), 'demo-pc');
INSERT INTO job_runs (job_id, run_number, trigger_kind, status, started_at, finished_at, files_total, files_transferred, files_skipped, files_failed, bytes_total, bytes_transferred, created_at, owner_host)
VALUES (3, 9, 'schedule', 'succeeded', datetime('now','-22 hours'), datetime('now','-21 hours','-20 minutes'), 9874, 9874, 0, 0, 3145728000, 2415919104, datetime('now','-22 hours'), 'demo-pc');
INSERT INTO job_runs (job_id, run_number, trigger_kind, status, started_at, finished_at, files_total, files_transferred, files_skipped, files_failed, bytes_total, bytes_transferred, created_at, owner_host)
VALUES (4, 6, 'schedule', 'succeeded', datetime('now','-20 hours'), datetime('now','-19 hours','-25 minutes'), 4218, 214, 4004, 0, 1288490188, 88080384, datetime('now','-20 hours'), 'demo-pc');
INSERT INTO job_runs (job_id, run_number, trigger_kind, status, started_at, finished_at, files_total, files_transferred, files_skipped, files_failed, bytes_total, bytes_transferred, created_at, owner_host)
VALUES (2, 4, 'schedule', 'succeeded', datetime('now','-2 days'), datetime('now','-2 days','+2 hours'), 18320, 18320, 0, 0, 53687091200, 53687091200, datetime('now','-2 days'), 'demo-pc');

INSERT INTO settings (key, value, value_type, updated_at) VALUES ('mail.destination', '/home/demo/Arhiv/Posta', 'string', datetime('now','-20 days'))
  ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;
INSERT INTO settings (key, value, value_type, updated_at) VALUES ('ui.theme', 'system', 'string', datetime('now'))
  ON CONFLICT(key) DO UPDATE SET value = excluded.value;
COMMIT;