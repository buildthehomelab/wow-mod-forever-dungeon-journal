-- Makes the DungeonJournal addon required for every player through Portalkeeper
-- (mod-realm-config). Run by hand against acore_world once the repo is public on GitHub. It is
-- kept out of data/sql on purpose: the table belongs to mod-realm-config, and a failing
-- statement there would stop the database import at startup.

DELETE FROM `mod_realm_config_addon` WHERE `addon_key` = 'DungeonJournal';
INSERT INTO `mod_realm_config_addon`
    (`addon_key`, `name`, `requirement`, `source_type`, `source_url`, `source_ref`, `install_directory`, `sort_order`, `enabled`)
VALUES
    ('DungeonJournal', 'DungeonJournal', 'Required', 'GitHub', 'https://github.com/buildthehomelab/wow-mod-forever-dungeon-journal', 'main', 'DungeonJournal', 62, 1);
