-- mod-forever-journal: the Lore tab's entries and what unlocks them. The lore itself is in the
-- mod_forever_journal_lore_<zone>.sql files (generated from data/lore/*.json by
-- tools/build_lore_sql.py).

CREATE TABLE IF NOT EXISTS `mod_forever_journal_lore` (
  `ID` INT UNSIGNED NOT NULL COMMENT 'zone id * 1000 + n',
  `Zone` INT UNSIGNED NOT NULL COMMENT 'AreaTable zone it is filed under',
  `Kind` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '0 zone, 1 place, 2 person, 3 history',
  `SortOrder` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `Faction` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 both, 1 Alliance, 2 Horde',
  `Races` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'race mask, 0 = every race',
  `Classes` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'class mask, 0 = every class',
  `Era` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'mod-individual-progression stage it opens at',
  `Title` VARCHAR(80) NOT NULL,
  `Body` TEXT NOT NULL,
  `Sources` TEXT NOT NULL COMMENT 'what it was written from; never sent to players',
  PRIMARY KEY (`ID`),
  KEY `idx_zone` (`Zone`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: lore entries';

CREATE TABLE IF NOT EXISTS `mod_forever_journal_lore_trigger` (
  `Entry` INT UNSIGNED NOT NULL,
  `Type` TINYINT UNSIGNED NOT NULL COMMENT '1 area, 2 talk to creature, 3 kill creature, 4 accept quest, 5 turn in quest',
  `Value` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'area id (0: by Area name), creature entry or quest id',
  `Area` VARCHAR(80) NOT NULL DEFAULT '' COMMENT 'area name inside the entry''s zone, when Value is 0',
  PRIMARY KEY (`Entry`, `Type`, `Value`, `Area`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: what unlocks each lore entry';
