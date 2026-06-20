-- mod-idlebot — persist organic training/spec markers across restarts.
SET @sql := IF(
    EXISTS(
        SELECT 1
        FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = 'idlebot_bots'
          AND COLUMN_NAME = 'last_trained_level'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `last_trained_level` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `death_count_current_step`'
);
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @sql := IF(
    EXISTS(
        SELECT 1
        FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = 'idlebot_bots'
          AND COLUMN_NAME = 'last_specced_level'
    ),
    'SELECT 1',
    'ALTER TABLE `idlebot_bots` ADD COLUMN `last_specced_level` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `last_trained_level`'
);
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
