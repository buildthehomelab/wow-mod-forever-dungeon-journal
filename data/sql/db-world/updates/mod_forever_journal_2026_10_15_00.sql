-- mod-forever-journal: the Town Guide spell and the text at the top of the Directory's gossip menu
-- (taken over from mod-npc-finder, with the same ids; a realm that had it gets the same rows again).
--
-- Spell 90050 "Town Guide" is new: it isn't in the stock client or server data. The client gets it
-- from a realm patch (tools/build_town_guide_patch.py adds the same row to Spell.dbc); this row gives
-- the server the same spell. It's a copy of "Dummy Spell" (18282): instant, targets yourself, no cost,
-- a Dummy effect that does nothing, the normal global cooldown, and allowed while mounted, sitting
-- or stealthed. Casting it opens the Directory tab, or the gossip menu without the addon. Generated
-- by `python3 tools/build_town_guide_patch.py --from-mpq patch-P.MPQ --sql`; keep the two in step.
--
-- npc_text 9500500-9500502 head the gossip menu pages (main menu, results, nothing found).
--
-- The spell id must match Journal.Directory.SpellId in mod_forever_journal.conf, and the npc_text
-- ids NPC_TEXT_* in src/JournalDirectory.cpp.
--
-- Idempotent: safe to run again.

DELETE FROM `spell_dbc` WHERE `ID` = 90050;
INSERT INTO `spell_dbc` VALUES (90050, 0, 0, 0, 150994944, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 101, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 3, 0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 0.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2578, 0, 0, 'Town Guide', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', 16712190, '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', 16712188, 'Ask where to find trainers, flight masters, the bank and other townsfolk. Search by name, title or zone and the place is flagged on your map.', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', 16712188, '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', 16712188, 0, 133, 1500, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.0, 0.0, 0.0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0.0, 0.0, 0.0, 0, 0);

DELETE FROM `npc_text` WHERE `ID` BETWEEN 9500500 AND 9500502;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `Probability0`) VALUES
(9500500, 'Where are you headed, $n? Search for anyone by name, title or zone, or pick what you need below.', 'Where are you headed, $n? Search for anyone by name, title or zone, or pick what you need below.', 1),
(9500501, 'Pick a place to flag it on your map. The nearest are at the top.', 'Pick a place to flag it on your map. The nearest are at the top.', 1),
(9500502, 'No one like that could be found. Try part of a name, a title such as "alchemy trainer", or a zone.', 'No one like that could be found. Try part of a name, a title such as "alchemy trainer", or a zone.', 1);
