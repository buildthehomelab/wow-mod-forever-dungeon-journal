-- mod-forever-journal: removes the module's tables from acore_world. Run by hand after taking the
-- module out of the build.
DROP TABLE IF EXISTS `mod_forever_journal_boss_extra`;
DROP TABLE IF EXISTS `mod_forever_journal_variant`;
DROP TABLE IF EXISTS `mod_forever_journal_variant_swap`;
DROP TABLE IF EXISTS `mod_forever_journal_learned`;
DROP TABLE IF EXISTS `mod_forever_journal_script_spell`;
DROP TABLE IF EXISTS `mod_forever_journal_toy`;
DROP TABLE IF EXISTS `mod_forever_journal_collection_note`;
DROP TABLE IF EXISTS `mod_forever_journal_script_enrage`;
DROP TABLE IF EXISTS `mod_forever_journal_ability`;
DROP TABLE IF EXISTS `mod_forever_journal_boss_reward`;
DROP TABLE IF EXISTS `mod_forever_journal_lore`;
DROP TABLE IF EXISTS `mod_forever_journal_lore_trigger`;
-- The Directory's Town Guide spell and gossip menu text.
DELETE FROM `spell_dbc` WHERE `ID` = 90050;
DELETE FROM `npc_text` WHERE `ID` BETWEEN 9500500 AND 9500502;
