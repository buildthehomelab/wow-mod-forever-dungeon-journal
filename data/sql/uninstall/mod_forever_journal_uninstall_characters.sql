-- mod-forever-journal: removes the module's tables from acore_characters. Run by hand after taking
-- the module out of the build.
DROP TABLE IF EXISTS `mod_forever_journal_lore_known`;
DROP TABLE IF EXISTS `mod_forever_journal_book_read`;
