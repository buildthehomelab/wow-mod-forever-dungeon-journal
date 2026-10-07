-- mod-forever-dungeon-journal: tables and seed data.
--
-- The journal builds itself from the game data (DungeonEncounter.dbc, LFGDungeons.dbc,
-- instance_encounters, creature and loot templates). These tables only fill the gaps:
--   * mod_forever_dungeon_journal_boss_extra   encounters whose loot sits in a chest, or that are fought
--                                      as several creatures (the Four Horsemen, the Iron Council)
--   * mod_forever_dungeon_journal_variant      extra difficulty tabs, such as mod-individual-progression's
--                                      40 player Onyxia and Naxxramas
--   * mod_forever_dungeon_journal_learned      spells bosses were seen casting (written by the module)

CREATE TABLE IF NOT EXISTS `mod_forever_dungeon_journal_boss_extra` (
  `MapID` SMALLINT UNSIGNED NOT NULL,
  `Bit` TINYINT UNSIGNED NOT NULL COMMENT 'DungeonEncounter.dbc bit of the encounter',
  `Kind` TINYINT UNSIGNED NOT NULL COMMENT '0 = creature fought in the encounter, 1 = chest with its loot, 2 = creature shown instead of the credit creature',
  `Difficulty` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT 'map difficulty 0-3; 255 = every difficulty',
  `Entry` INT UNSIGNED NOT NULL COMMENT 'creature_template or gameobject_template entry',
  `Flags` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = hard mode loot',
  `Comment` VARCHAR(100) NOT NULL DEFAULT '',
  PRIMARY KEY (`MapID`, `Bit`, `Kind`, `Difficulty`, `Entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-dungeon-journal: extra creatures and loot chests per encounter';

CREATE TABLE IF NOT EXISTS `mod_forever_dungeon_journal_variant` (
  `MapID` SMALLINT UNSIGNED NOT NULL,
  `Variant` TINYINT UNSIGNED NOT NULL COMMENT '100 and up; 0-3 are the map difficulties',
  `Difficulty` TINYINT UNSIGNED NOT NULL COMMENT 'map difficulty the variant is played on',
  `Expansion` TINYINT UNSIGNED NOT NULL COMMENT 'journal tier it is listed under: 0 Classic, 1 TBC, 2 WotLK',
  `MaxPlayers` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `MinLevel` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `RequiredState` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'mod-individual-progression state that unlocks it',
  `RequiresIP` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1 = only when IndividualProgression.Enable = 1',
  `HidesDifficulty` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1 = the map difficulty itself gets no tab of its own',
  `Label` VARCHAR(40) NOT NULL,
  PRIMARY KEY (`MapID`, `Variant`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-dungeon-journal: extra difficulty tabs';

CREATE TABLE IF NOT EXISTS `mod_forever_dungeon_journal_variant_swap` (
  `MapID` SMALLINT UNSIGNED NOT NULL,
  `Variant` TINYINT UNSIGNED NOT NULL,
  `FromEntry` INT UNSIGNED NOT NULL,
  `ToEntry` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`MapID`, `Variant`, `FromEntry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-dungeon-journal: creatures a variant replaces';

CREATE TABLE IF NOT EXISTS `mod_forever_dungeon_journal_learned` (
  `Entry` INT UNSIGNED NOT NULL COMMENT 'base creature entry (difficulty entries are folded into it)',
  `Spell` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`Entry`, `Spell`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-dungeon-journal: spells bosses were seen casting';

DELETE FROM `mod_forever_dungeon_journal_boss_extra`;
INSERT INTO `mod_forever_dungeon_journal_boss_extra` (`MapID`, `Bit`, `Kind`, `Difficulty`, `Entry`, `Flags`, `Comment`) VALUES
-- Classic
(230, 16, 1, 255, 169243, 0, 'BRD: Chest of The Seven'),
(409, 8, 1, 255, 179703, 0, 'MC: Cache of the Firelord'),
(309, 4, 0, 255, 15082, 0, 'ZG Edge of Madness: Gri''lek'),
(309, 4, 0, 255, 15083, 0, 'ZG Edge of Madness: Hazza''rah'),
(309, 4, 0, 255, 15084, 0, 'ZG Edge of Madness: Renataki'),
(309, 4, 0, 255, 15085, 0, 'ZG Edge of Madness: Wushoolay'),
(531, 1, 0, 255, 15511, 0, 'AQ40 Silithid Royalty: Lord Kri'),
(531, 1, 0, 255, 15543, 0, 'AQ40 Silithid Royalty: Princess Yauj'),
(531, 6, 0, 255, 15276, 0, 'AQ40 Twin Emperors: Emperor Vek''lor'),
-- Naxxramas (10, 25 and mod-individual-progression's 40 on difficulty 2)
(533, 8, 0, 255, 16063, 0, 'Naxx Four Horsemen: Sir Zeliek'),
(533, 8, 0, 255, 16064, 0, 'Naxx Four Horsemen: Thane Korth''azz'),
(533, 8, 0, 255, 16065, 0, 'Naxx Four Horsemen: Lady Blaumeux'),
(533, 8, 0, 255, 30549, 0, 'Naxx Four Horsemen: Baron Rivendare'),
(533, 8, 1, 0, 181366, 0, 'Naxx: Four Horsemen Chest (10)'),
(533, 8, 1, 1, 193426, 0, 'Naxx: Four Horsemen Chest (25)'),
(533, 8, 1, 2, 361000, 0, 'Naxx: Four Horsemen Chest (40, individual progression)'),
-- The Burning Crusade
(543, 2, 0, 255, 17536, 0, 'Ramparts: Nazan'),
(543, 2, 1, 0, 185168, 0, 'Ramparts: Reinforced Fel Iron Chest'),
(543, 2, 1, 1, 185169, 0, 'Ramparts: Reinforced Fel Iron Chest (heroic)'),
(532, 3, 2, 255, 18168, 0, 'Kara Opera: The Crone'),
(532, 3, 0, 255, 17521, 0, 'Kara Opera: The Big Bad Wolf'),
(532, 3, 0, 255, 17533, 0, 'Kara Opera: Romulo'),
(532, 3, 0, 255, 17534, 0, 'Kara Opera: Julianne'),
(532, 8, 1, 255, 185119, 0, 'Kara: Dust Covered Chest (Chess Event)'),
(564, 7, 2, 255, 22949, 0, 'BT Illidari Council: Gathios the Shatterer'),
(564, 7, 0, 255, 22950, 0, 'BT Illidari Council: High Nethermancer Zerevor'),
(564, 7, 0, 255, 22951, 0, 'BT Illidari Council: Lady Malande'),
(564, 7, 0, 255, 22952, 0, 'BT Illidari Council: Veras Darkshadow'),
(580, 3, 0, 255, 25166, 0, 'Sunwell Eredar Twins: Grand Warlock Alythess'),
-- Wrath of the Lich King
(578, 3, 1, 0, 191349, 0, 'Oculus: Cache of Eregos'),
(578, 3, 1, 1, 193603, 0, 'Oculus: Cache of Eregos (heroic)'),
(595, 3, 2, 255, 26533, 0, 'CoS: Mal''Ganis'),
(595, 3, 1, 0, 190663, 0, 'CoS: Dark Runed Chest'),
(595, 3, 1, 1, 193597, 0, 'CoS: Dark Runed Chest (heroic)'),
(599, 2, 1, 0, 190586, 0, 'HoS: Tribunal Chest'),
(599, 2, 1, 1, 193996, 0, 'HoS: Tribunal Chest (heroic)'),
(600, 3, 2, 255, 26632, 0, 'DTK: The Prophet Tharon''ja'),
(616, 0, 1, 0, 193905, 0, 'EoE: Alexstrasza''s Gift (10)'),
(616, 0, 1, 1, 193967, 0, 'EoE: Alexstrasza''s Gift (25)'),
(616, 0, 1, 0, 194158, 0, 'EoE: Heart of Magic (10)'),
(616, 0, 1, 1, 194159, 0, 'EoE: Heart of Magic (25)'),
(603, 4, 2, 255, 32867, 0, 'Ulduar Iron Council: Steelbreaker'),
(603, 4, 0, 255, 32927, 0, 'Ulduar Iron Council: Runemaster Molgeim'),
(603, 4, 0, 255, 32857, 0, 'Ulduar Iron Council: Stormcaller Brundir'),
(603, 5, 1, 0, 195046, 0, 'Ulduar: Cache of Living Stone (10)'),
(603, 5, 1, 1, 195047, 0, 'Ulduar: Cache of Living Stone (25)'),
(603, 7, 2, 255, 32845, 0, 'Ulduar: Hodir'),
(603, 7, 1, 0, 194307, 0, 'Ulduar: Cache of Winter (10)'),
(603, 7, 1, 1, 194308, 0, 'Ulduar: Cache of Winter (25)'),
(603, 7, 1, 0, 194200, 1, 'Ulduar: Rare Cache of Winter (10)'),
(603, 7, 1, 1, 194201, 1, 'Ulduar: Rare Cache of Winter (25)'),
(603, 8, 2, 255, 32865, 0, 'Ulduar: Thorim'),
(603, 8, 1, 0, 194312, 0, 'Ulduar: Cache of Storms (10)'),
(603, 8, 1, 1, 194314, 0, 'Ulduar: Cache of Storms (25)'),
(603, 8, 1, 0, 194313, 1, 'Ulduar: Cache of Storms, hard mode (10)'),
(603, 8, 1, 1, 194315, 1, 'Ulduar: Cache of Storms, hard mode (25)'),
(603, 9, 2, 255, 32906, 0, 'Ulduar: Freya'),
(603, 9, 1, 0, 194324, 0, 'Ulduar: Freya''s Gift (10)'),
(603, 9, 1, 1, 194328, 0, 'Ulduar: Freya''s Gift (25)'),
(603, 9, 1, 0, 194327, 1, 'Ulduar: Freya''s Gift, three elders (10)'),
(603, 9, 1, 1, 194331, 1, 'Ulduar: Freya''s Gift, three elders (25)'),
(603, 10, 2, 255, 33350, 0, 'Ulduar: Mimiron'),
(603, 10, 1, 0, 194789, 0, 'Ulduar: Cache of Innovation (10)'),
(603, 10, 1, 1, 194956, 0, 'Ulduar: Cache of Innovation (25)'),
(603, 10, 1, 0, 194957, 1, 'Ulduar: Cache of Innovation, hard mode (10)'),
(603, 10, 1, 1, 194958, 1, 'Ulduar: Cache of Innovation, hard mode (25)'),
(603, 13, 2, 255, 32871, 0, 'Ulduar: Algalon the Observer'),
(603, 13, 1, 0, 194821, 0, 'Ulduar: Gift of the Observer (10)'),
(603, 13, 1, 1, 194822, 0, 'Ulduar: Gift of the Observer (25)'),
(649, 0, 0, 255, 34796, 0, 'ToC Northrend Beasts: Gormok the Impaler'),
(649, 0, 0, 255, 35144, 0, 'ToC Northrend Beasts: Acidmaw'),
(649, 0, 0, 255, 34799, 0, 'ToC Northrend Beasts: Dreadscale'),
(649, 2, 1, 0, 195631, 0, 'ToC: Champions'' Cache (10)'),
(649, 2, 1, 1, 195632, 0, 'ToC: Champions'' Cache (25)'),
(649, 2, 1, 2, 195633, 0, 'ToC: Champions'' Cache (10 heroic)'),
(649, 2, 1, 3, 195635, 0, 'ToC: Champions'' Cache (25 heroic)'),
(649, 3, 0, 255, 34497, 0, 'ToC Val''kyr Twins: Fjola Lightbane'),
(650, 0, 1, 0, 195709, 0, 'ToC5: Champion''s Cache'),
(650, 0, 1, 1, 195710, 0, 'ToC5: Champion''s Cache (heroic)'),
(650, 1, 2, 255, 35119, 0, 'ToC5 Argent Champion: Eadric the Pure'),
(650, 1, 0, 255, 34928, 0, 'ToC5 Argent Champion: Argent Confessor Paletress'),
(650, 1, 1, 0, 195374, 0, 'ToC5: Eadric''s Cache'),
(650, 1, 1, 1, 195375, 0, 'ToC5: Eadric''s Cache (heroic)'),
(650, 1, 1, 0, 195323, 0, 'ToC5: Confessor''s Cache'),
(650, 1, 1, 1, 195324, 0, 'ToC5: Confessor''s Cache (heroic)'),
(650, 2, 2, 255, 35451, 0, 'ToC5: The Black Knight'),
(668, 2, 2, 255, 36954, 0, 'HoR: The Lich King'),
(668, 2, 1, 0, 201710, 0, 'HoR: The Captain''s Chest'),
(668, 2, 1, 1, 202336, 0, 'HoR: The Captain''s Chest (heroic)'),
(631, 2, 2, 255, 36939, 0, 'ICC Gunship: High Overlord Saurfang (fought by the Alliance)'),
(631, 2, 0, 255, 36948, 0, 'ICC Gunship: Muradin Bronzebeard (fought by the Horde)'),
(631, 2, 1, 0, 201873, 0, 'ICC: Gunship Armory (10)'),
(631, 2, 1, 1, 201874, 0, 'ICC: Gunship Armory (25)'),
(631, 2, 1, 2, 201872, 0, 'ICC: Gunship Armory (10 heroic)'),
(631, 2, 1, 3, 201875, 0, 'ICC: Gunship Armory (25 heroic)'),
(631, 3, 1, 0, 202239, 0, 'ICC: Deathbringer''s Cache (10)'),
(631, 3, 1, 1, 202240, 0, 'ICC: Deathbringer''s Cache (25)'),
(631, 3, 1, 2, 202238, 0, 'ICC: Deathbringer''s Cache (10 heroic)'),
(631, 3, 1, 3, 202241, 0, 'ICC: Deathbringer''s Cache (25 heroic)'),
(631, 7, 0, 255, 37972, 0, 'ICC Blood Council: Prince Keleseth'),
(631, 7, 0, 255, 37973, 0, 'ICC Blood Council: Prince Taldaram'),
(631, 9, 2, 255, 36789, 0, 'ICC: Valithria Dreamwalker'),
(631, 9, 1, 0, 201959, 0, 'ICC: Cache of the Dreamwalker (10)'),
(631, 9, 1, 1, 202339, 0, 'ICC: Cache of the Dreamwalker (25)'),
(631, 9, 1, 2, 202338, 0, 'ICC: Cache of the Dreamwalker (10 heroic)'),
(631, 9, 1, 3, 202340, 0, 'ICC: Cache of the Dreamwalker (25 heroic)');

-- mod-individual-progression's vanilla raids. Onyxia below level 71 is a separate creature
-- (301000); 40 player Naxxramas is played on difficulty 2 with its own creature entries.
DELETE FROM `mod_forever_dungeon_journal_variant` WHERE `MapID` IN (249, 533);
INSERT INTO `mod_forever_dungeon_journal_variant` (`MapID`, `Variant`, `Difficulty`, `Expansion`, `MaxPlayers`, `MinLevel`, `RequiredState`, `RequiresIP`, `HidesDifficulty`, `Label`) VALUES
(249, 100, 0, 0, 40, 50, 0, 1, 0, '40 Player'),
(533, 101, 2, 0, 40, 60, 6, 1, 1, '40 Player');

DELETE FROM `mod_forever_dungeon_journal_variant_swap` WHERE `MapID` IN (249, 533);
INSERT INTO `mod_forever_dungeon_journal_variant_swap` (`MapID`, `Variant`, `FromEntry`, `ToEntry`) VALUES
(249, 100, 10184, 301000);
