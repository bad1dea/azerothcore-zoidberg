-- mod-idlebot — failure/session metadata for blocked-state semantics and dashboard event aging.

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'soak_run_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `soak_run_id` VARCHAR(32) DEFAULT NULL AFTER `decision_mode`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'bot_session_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `bot_session_id` VARCHAR(64) DEFAULT NULL AFTER `soak_run_id`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'reset_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `reset_id` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `bot_session_id`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'blocked_reason'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `blocked_reason` VARCHAR(128) DEFAULT NULL AFTER `step_state`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'blocked_since'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `blocked_since` TIMESTAMP NULL DEFAULT NULL AFTER `blocked_reason`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'last_failure_code'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `last_failure_code` VARCHAR(64) DEFAULT NULL AFTER `blocked_since`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'requires_user_action'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `requires_user_action` TINYINT(1) NOT NULL DEFAULT 0 AFTER `last_failure_code`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_events' AND COLUMN_NAME = 'event_code'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_events` ADD COLUMN `event_code` VARCHAR(64) DEFAULT NULL AFTER `event_type`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_events' AND COLUMN_NAME = 'soak_run_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_events` ADD COLUMN `soak_run_id` VARCHAR(32) DEFAULT NULL AFTER `event_code`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_events' AND COLUMN_NAME = 'bot_session_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_events` ADD COLUMN `bot_session_id` VARCHAR(64) DEFAULT NULL AFTER `soak_run_id`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_events' AND COLUMN_NAME = 'reset_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_events` ADD COLUMN `reset_id` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `bot_session_id`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_live_state' AND COLUMN_NAME = 'soak_run_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_live_state` ADD COLUMN `soak_run_id` VARCHAR(32) DEFAULT NULL AFTER `guide_id`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_live_state' AND COLUMN_NAME = 'bot_session_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_live_state` ADD COLUMN `bot_session_id` VARCHAR(64) DEFAULT NULL AFTER `soak_run_id`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_live_state' AND COLUMN_NAME = 'reset_id'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_live_state` ADD COLUMN `reset_id` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `bot_session_id`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_live_state' AND COLUMN_NAME = 'blocked_reason'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_live_state` ADD COLUMN `blocked_reason` VARCHAR(128) DEFAULT NULL AFTER `step_name`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_live_state' AND COLUMN_NAME = 'blocked_since'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_live_state` ADD COLUMN `blocked_since` DATETIME DEFAULT NULL AFTER `blocked_reason`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_live_state' AND COLUMN_NAME = 'last_failure_code'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_live_state` ADD COLUMN `last_failure_code` VARCHAR(64) DEFAULT NULL AFTER `blocked_since`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;

SET @stmt := (
  SELECT IF(
    EXISTS (
      SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS
      WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_live_state' AND COLUMN_NAME = 'requires_user_action'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_live_state` ADD COLUMN `requires_user_action` TINYINT(1) DEFAULT 0 AFTER `last_failure_code`'
  )
);
PREPARE s FROM @stmt; EXECUTE s; DEALLOCATE PREPARE s;
