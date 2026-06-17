-- mod-idlebot — add guide-progress + death-handling columns to idlebot_bots.
--
-- For servers where idlebot_bots was already created by an earlier base schema
-- (M2/M4 deploys) without these columns. Fresh installs get them from base/.
--
-- IDEMPOTENT and safe to re-run: each column is added only if absent, using
-- information_schema + a prepared statement. Avoids `ADD COLUMN IF NOT EXISTS`
-- (unsupported on MySQL 8.x) and `DELIMITER`/stored procedures (so it works
-- whether the applier pipes to the mysql CLI or a single-statement driver).
-- MySQL 8.4 / MariaDB compatible.

-- guide_id
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'guide_id');
SET @ddl := IF(@col = 0,
    'ALTER TABLE `idlebot_bots` ADD COLUMN `guide_id` VARCHAR(96) DEFAULT NULL',
    'DO 0');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- step_index
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'step_index');
SET @ddl := IF(@col = 0,
    'ALTER TABLE `idlebot_bots` ADD COLUMN `step_index` INT UNSIGNED NOT NULL DEFAULT 0',
    'DO 0');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- step_state
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'step_state');
SET @ddl := IF(@col = 0,
    'ALTER TABLE `idlebot_bots` ADD COLUMN `step_state` VARCHAR(32) NOT NULL DEFAULT ''idle''',
    'DO 0');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- death_count_total
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'death_count_total');
SET @ddl := IF(@col = 0,
    'ALTER TABLE `idlebot_bots` ADD COLUMN `death_count_total` INT UNSIGNED NOT NULL DEFAULT 0',
    'DO 0');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- death_count_current_step
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'death_count_current_step');
SET @ddl := IF(@col = 0,
    'ALTER TABLE `idlebot_bots` ADD COLUMN `death_count_current_step` INT UNSIGNED NOT NULL DEFAULT 0',
    'DO 0');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- updated_at
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'idlebot_bots' AND COLUMN_NAME = 'updated_at');
SET @ddl := IF(@col = 0,
    'ALTER TABLE `idlebot_bots` ADD COLUMN `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP',
    'DO 0');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
