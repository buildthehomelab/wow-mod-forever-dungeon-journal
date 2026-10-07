-- mod-dungeon-journal: removes the module's tables from acore_world. Run by hand after taking the
-- module out of the build.
DROP TABLE IF EXISTS `mod_dungeon_journal_boss_extra`;
DROP TABLE IF EXISTS `mod_dungeon_journal_variant`;
DROP TABLE IF EXISTS `mod_dungeon_journal_variant_swap`;
DROP TABLE IF EXISTS `mod_dungeon_journal_learned`;
DROP TABLE IF EXISTS `mod_dungeon_journal_script_spell`;
