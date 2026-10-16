-- mod-forever-journal: the lore each character has discovered.

CREATE TABLE IF NOT EXISTS `mod_forever_journal_lore_known` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'character',
  `entry` INT UNSIGNED NOT NULL COMMENT 'mod_forever_journal_lore.ID',
  `time` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time found; 0 when credited from what was explored or done before',
  PRIMARY KEY (`guid`, `entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: lore discovered per character';

-- mod-forever-journal: the books each character has read (the Library). A book is its first page.

CREATE TABLE IF NOT EXISTS `mod_forever_journal_book_read` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'character',
  `book` INT UNSIGNED NOT NULL COMMENT 'page_text.ID of the first page',
  `zone` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'zone it was read in',
  `time` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time read',
  PRIMARY KEY (`guid`, `book`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: books read per character';
