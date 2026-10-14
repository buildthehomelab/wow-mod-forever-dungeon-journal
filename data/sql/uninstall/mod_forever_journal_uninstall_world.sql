-- mod-forever-journal: removes the module's tables from acore_world. Run by hand after taking the
-- module out of the build.
DROP TABLE IF EXISTS `mod_forever_journal_boss_extra`;
DROP TABLE IF EXISTS `mod_forever_journal_variant`;
DROP TABLE IF EXISTS `mod_forever_journal_variant_swap`;
DROP TABLE IF EXISTS `mod_forever_journal_learned`;
DROP TABLE IF EXISTS `mod_forever_journal_script_spell`;
DROP TABLE IF EXISTS `mod_forever_journal_toy`;
DROP TABLE IF EXISTS `mod_forever_journal_collection_note`;
