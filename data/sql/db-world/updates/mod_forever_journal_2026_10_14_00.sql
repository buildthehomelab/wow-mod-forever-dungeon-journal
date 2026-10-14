-- mod-forever-journal: the module was mod-forever-dungeon-journal. Its spells learned from bosses
-- move to the new table (created by mod_forever_journal_2026_10_06_00.sql, which runs first);
-- the old tables go. Then the collections' tables: toys and notes.

SET @old_learned := (SELECT COUNT(*) FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_forever_dungeon_journal_learned');
SET @copy := IF(@old_learned > 0,
    'INSERT IGNORE INTO `mod_forever_journal_learned` (`Entry`, `Spell`) SELECT `Entry`, `Spell` FROM `mod_forever_dungeon_journal_learned`',
    'DO 0');
PREPARE journal_copy FROM @copy;
EXECUTE journal_copy;
DEALLOCATE PREPARE journal_copy;

DROP TABLE IF EXISTS `mod_forever_dungeon_journal_boss_extra`;
DROP TABLE IF EXISTS `mod_forever_dungeon_journal_variant`;
DROP TABLE IF EXISTS `mod_forever_dungeon_journal_variant_swap`;
DROP TABLE IF EXISTS `mod_forever_dungeon_journal_learned`;
DROP TABLE IF EXISTS `mod_forever_dungeon_journal_script_spell`;

-- Toys: the items retail's toy box holds that exist in 3.3.5 (retail Toy.db2 against the
-- 3.3.5 item table). Add a row to list another item in the Toys tab.
CREATE TABLE IF NOT EXISTS `mod_forever_journal_toy` (
  `ItemID` INT UNSIGNED NOT NULL,
  `Comment` VARCHAR(100) NOT NULL DEFAULT '',
  PRIMARY KEY (`ItemID`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: items in the Toys tab';

-- Notes shown with a mount, pet or toy's sources, for what the data can't tell (a promotion, a
-- custom reward). Kind M mount (ID = spell), P pet (ID = spell), T toy (ID = item).
CREATE TABLE IF NOT EXISTS `mod_forever_journal_collection_note` (
  `Kind` CHAR(1) NOT NULL,
  `ID` INT UNSIGNED NOT NULL,
  `Sort` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `Note` VARCHAR(200) NOT NULL,
  PRIMARY KEY (`Kind`, `ID`, `Sort`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: notes on where a collectible comes from';

DELETE FROM `mod_forever_journal_toy`;
INSERT INTO `mod_forever_journal_toy` (`ItemID`, `Comment`) VALUES
(1973, 'Orb of Deception'),
(13379, 'Piccolo of the Flaming Fire'),
(17712, 'Winter Veil Disguise Kit'),
(17716, 'Snowmaster 9000'),
(18660, 'World Enlarger'),
(18984, 'Dimensional Ripper - Everlook'),
(18986, 'Ultrasafe Transporter: Gadgetzan'),
(21540, 'Elune''s Lantern'),
(23767, 'Crashin'' Thrashin'' Robot'),
(30542, 'Dimensional Ripper - Area 52'),
(30544, 'Ultrasafe Transporter: Toshley''s Station'),
(30690, 'Power Converter'),
(32542, 'Imp in a Ball'),
(32566, 'Picnic Basket'),
(32782, 'Time-Lost Figurine'),
(33079, 'Murloc Costume'),
(33219, 'Goblin Gumbo Kettle'),
(33223, 'Fishing Chair'),
(33927, 'Brewfest Pony Keg'),
(34480, 'Romantic Picnic Basket'),
(34499, 'Paper Flying Machine Kit'),
(34686, 'Brazier of Dancing Flames'),
(35227, 'Goblin Weather Machine - Prototype 01-B'),
(35275, 'Orb of the Sin''dorei'),
(36862, 'Worn Troll Dice'),
(36863, 'Decahedral Dwarven Dice'),
(37254, 'Super Simian Sphere'),
(37460, 'Rope Pet Leash'),
(37710, 'Crashin'' Thrashin'' Racer Controller'),
(38301, 'D.I.S.C.O.'),
(38578, 'The Flag of Ownership'),
(40727, 'Gnomish Gravity Well'),
(40768, 'MOLL-E'),
(40895, 'Gnomish X-Ray Specs'),
(43499, 'Iron Boot Flask'),
(43824, 'The Schools of Arcane Magic - Mastery'),
(44430, 'Titanium Seal of Dalaran'),
(44606, 'Toy Train Set'),
(44719, 'Frenzyheart Brew'),
(44820, 'Red Ribbon Pet Leash'),
(45011, 'Stormwind Banner'),
(45013, 'Thunder Bluff Banner'),
(45014, 'Orgrimmar Banner'),
(45015, 'Sen''jin Banner'),
(45016, 'Undercity Banner'),
(45017, 'Silvermoon City Banner'),
(45018, 'Ironforge Banner'),
(45019, 'Gnomeregan Banner'),
(45020, 'Exodar Banner'),
(45021, 'Darnassus Banner'),
(45057, 'Wind-Up Train Wrecker'),
(45063, 'Foam Sword Rack'),
(45984, 'Unusual Compass'),
(46709, 'MiniZep Controller'),
(46780, 'Ogre Pinata'),
(46843, 'Argent Crusader''s Banner'),
(48933, 'Wormhole Generator: Northrend'),
(49703, 'Perpetual Purple Firework'),
(49704, 'Carved Ogre Idol'),
(50471, 'The Heartbreaker'),
(52201, 'Muradin''s Favor'),
(52253, 'Sylvanas''s Music Box'),
(54212, 'Instant Statue Pedestal'),
(54343, 'Blue Crashin'' Thrashin'' Racer Controller'),
(54437, 'Tiny Green Ragdoll'),
(54438, 'Tiny Blue Ragdoll'),
(54452, 'Ethereal Portal'),
(54651, 'Gnomeregan Pride'),
(54653, 'Darkspear Pride');
