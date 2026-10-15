-- mod-forever-journal: abilities by hand, the difficulty abilities were learned on, enrage timers
-- and what bosses give besides their loot.

-- Spells learned from bosses keep the map difficulty they were seen on; rows learned before stay
-- for every difficulty (255).
SET @has_difficulty := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_forever_journal_learned' AND COLUMN_NAME = 'Difficulty');
SET @alter := IF(@has_difficulty = 0,
    'ALTER TABLE `mod_forever_journal_learned` ADD COLUMN `Difficulty` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT ''map difficulty 0-3 it was seen on; 255 = every difficulty'' AFTER `Spell`, DROP PRIMARY KEY, ADD PRIMARY KEY (`Entry`, `Spell`, `Difficulty`)',
    'DO 0');
PREPARE journal_alter FROM @alter;
EXECUTE journal_alter;
DEALLOCATE PREPARE journal_alter;

-- Changes to the automatic ability list, applied after it. A row for a spell the creature isn't
-- known to cast adds it (on every difficulty). Entry 0 changes the spell on every creature.
CREATE TABLE IF NOT EXISTS `mod_forever_journal_ability` (
  `Entry` INT UNSIGNED NOT NULL COMMENT 'base creature entry; 0 = every creature',
  `Spell` INT UNSIGNED NOT NULL COMMENT 'the spell (any difficulty version of it)',
  `Hide` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = never listed',
  `Sort` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = where the automatic list puts it; 1 and up = before the rest, lowest first',
  `Flags` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 tank, 2 healer, 4 damage dealer, 8 important, 16 deadly',
  `Note` VARCHAR(200) NOT NULL DEFAULT '' COMMENT 'shown under the description',
  `Comment` VARCHAR(100) NOT NULL DEFAULT '',
  PRIMARY KEY (`Entry`, `Spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: boss abilities by hand';

-- Lines on a boss's overview for what it gives besides its loot, such as currency put straight
-- in the bags. By encounter (MapID and Bit) or by creature (MapID and Entry, for rares).
CREATE TABLE IF NOT EXISTS `mod_forever_journal_boss_reward` (
  `MapID` SMALLINT UNSIGNED NOT NULL,
  `Bit` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT 'DungeonEncounter.dbc bit of the encounter; 255 = by Entry',
  `Entry` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'creature_template entry; 0 = by Bit',
  `Difficulty` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT 'map difficulty 0-3; 255 = every difficulty',
  `Sort` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `Text` VARCHAR(200) NOT NULL,
  PRIMARY KEY (`MapID`, `Bit`, `Entry`, `Difficulty`, `Sort`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: what bosses give besides their loot';

-- Filled by mod_forever_journal_script_spells.sql; created here too so the table exists
-- whichever runs first.
CREATE TABLE IF NOT EXISTS `mod_forever_journal_script_enrage` (
  `ScriptName` VARCHAR(64) NOT NULL,
  `Spell` INT UNSIGNED NOT NULL,
  `Seconds` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`ScriptName`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: enrage timers from C++ scripts';
