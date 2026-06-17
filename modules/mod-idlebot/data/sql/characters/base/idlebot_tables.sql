-- mod-idlebot — base schema (characters DB)
--
-- LAYOUT: this lives at modules/mod-idlebot/data/sql/characters/base/ to match
-- the playerbots-style layout that the zoidberg `ac-module-sql` one-shot loops
-- over (data/sql/world|characters/{base,updates}). Files under base/ are applied
-- to acore_characters. Confirm the applier picks this path up on first boot.
--
-- TODO(verify): the zoidberg ac-module-sql script applies module SQL idempotently
-- using DROP TABLE IF EXISTS + static INSERTs. These tables use CREATE TABLE IF
-- NOT EXISTS instead (safe to re-run, but does NOT reset rows). If the applier
-- specifically expects DROP-based idempotency, adjust accordingly — check the
-- inline script in compose/zoidberg before relying on either behaviour.
--
-- Runtime / per-character-state tables live in the CHARACTERS database because
-- they describe live bot instances, goals, progress, and event history tied to
-- specific characters on this realm.
--
-- MySQL/MariaDB compatible (MySQL 8.4 on zoidberg). Safe to re-run (IF NOT EXISTS).

-- --------------------------------------------------------------------------
-- Registered bots this module drives.
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_bots` (
    `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_name`      VARCHAR(64)  NOT NULL,
    `character_guid` INT UNSIGNED DEFAULT NULL,   -- resolved when online
    `active`        TINYINT(1)   NOT NULL DEFAULT 0,
    `decision_mode` VARCHAR(16)  NOT NULL DEFAULT 'strict',
    -- guide progress (Priority 3): resume mid-guide across restarts.
    `guide_id`      VARCHAR(96)  DEFAULT NULL,
    `step_index`    INT UNSIGNED NOT NULL DEFAULT 0,
    `step_state`    VARCHAR(32)  NOT NULL DEFAULT 'idle',   -- idle/running/blocked
    -- death handling (Priority 2): persisted counters.
    `death_count_total`        INT UNSIGNED NOT NULL DEFAULT 0,
    `death_count_current_step` INT UNSIGNED NOT NULL DEFAULT 0,
    `created_at`    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at`    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_bot_name` (`bot_name`),
    KEY `idx_character_guid` (`character_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- --------------------------------------------------------------------------
-- A high-level goal assigned to a bot (e.g. level 1-6 via a guide).
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_goals` (
    `id`           INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_id`       INT UNSIGNED NOT NULL,
    `guide_id`     VARCHAR(96)  NOT NULL,
    `target_level` TINYINT UNSIGNED DEFAULT NULL,
    `zone`         VARCHAR(64)  DEFAULT NULL,
    `status`       VARCHAR(16)  NOT NULL DEFAULT 'pending',  -- pending/active/paused/complete/failed
    `current_step` INT UNSIGNED NOT NULL DEFAULT 0,
    `created_at`   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_bot_id` (`bot_id`),
    KEY `idx_status` (`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- --------------------------------------------------------------------------
-- Concrete executable steps derived from a guide for a specific goal run.
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_goal_steps` (
    `id`         INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `goal_id`    INT UNSIGNED NOT NULL,
    `step_index` INT UNSIGNED NOT NULL,
    `step_type`  VARCHAR(32)  NOT NULL,
    `payload`    TEXT         DEFAULT NULL,   -- JSON: targets/coords/conditions
    `status`     VARCHAR(16)  NOT NULL DEFAULT 'pending',
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_goal_step` (`goal_id`, `step_index`),
    KEY `idx_goal_id` (`goal_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- --------------------------------------------------------------------------
-- Append-only event log (telemetry).
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_events` (
    `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_id`     INT UNSIGNED NOT NULL,
    `event_type` VARCHAR(32)  NOT NULL,       -- death/stuck/quest_accept/level_up/...
    `detail`     TEXT         DEFAULT NULL,
    `created_at` TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_bot_time` (`bot_id`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- --------------------------------------------------------------------------
-- Periodic state snapshots so goals resume after a worldserver restart.
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_state_snapshots` (
    `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_id`     INT UNSIGNED NOT NULL,
    `snapshot`   MEDIUMTEXT   NOT NULL,        -- JSON blob of resumable state
    `created_at` TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_bot_time` (`bot_id`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- --------------------------------------------------------------------------
-- Failure tracking feeding the decision engine.
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_failures` (
    `id`           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_id`       INT UNSIGNED NOT NULL,
    `failure_type` VARCHAR(32)  NOT NULL,      -- death/stuck/no_progress/pathing/interact
    `step_index`   INT UNSIGNED DEFAULT NULL,
    `area`         VARCHAR(64)  DEFAULT NULL,
    `count`        INT UNSIGNED NOT NULL DEFAULT 1,
    `last_seen`    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_bot_type` (`bot_id`, `failure_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- --------------------------------------------------------------------------
-- Quests the bot has decided are broken/blocked, to avoid retrying forever.
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_blocked_quests` (
    `id`         INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_id`     INT UNSIGNED NOT NULL,
    `quest_id`   INT UNSIGNED NOT NULL,
    `reason`     VARCHAR(128) DEFAULT NULL,
    `created_at` TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_bot_quest` (`bot_id`, `quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- --------------------------------------------------------------------------
-- Social interaction cooldowns (M8+).
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_social_cooldowns` (
    `id`          INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_id`      INT UNSIGNED NOT NULL,
    `target_guid` INT UNSIGNED NOT NULL,
    `action`      VARCHAR(16)  NOT NULL,       -- invite/whisper/emote
    `expires_at`  TIMESTAMP    NOT NULL,
    PRIMARY KEY (`id`),
    KEY `idx_bot_target` (`bot_id`, `target_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- --------------------------------------------------------------------------
-- Decision audit trail.
-- --------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `idlebot_decision_history` (
    `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_id`     INT UNSIGNED NOT NULL,
    `step_index` INT UNSIGNED DEFAULT NULL,
    `action`     VARCHAR(48)  NOT NULL,
    `score`      FLOAT        DEFAULT NULL,
    `reason`     VARCHAR(255) DEFAULT NULL,
    `created_at` TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_bot_time` (`bot_id`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
