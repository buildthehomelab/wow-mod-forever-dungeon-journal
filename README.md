# Dungeon Journal

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module and addon that add a
**retail-style dungeon journal**, in the spirit of WoW Forever's: every dungeon and raid with its
bosses, their abilities, the loot they drop with drop chances, and the instance's quests with your
own progress on them. Open it with **Shift-J**, the minimap button or `/dj`.

Everything comes from your realm's own data, so custom loot, mod-individual-progression's vanilla
raids and other modules' changes show up as they are on your server.

## Patch Notes: Dungeon Journal

Category: Interface

- New **Dungeon Journal** (Shift-J, `/dj` or the minimap book). Browse every dungeon and raid by
  expansion, with Dungeon Finder art, level ranges and wings.
- **Bosses**: each boss with its 3D model, level, health per difficulty, and a check when you've
  killed it this lockout. Rare spawns are listed too.
- **Abilities**: what each boss and its adds do, with the spell's description, cast time and tags
  such as Interruptible, Magic, Curse, Area and Crowd control. The journal learns abilities it
  didn't know as bosses are fought.
- **Loot**: every drop with its drop chance, filtered to your class (or any class) and slot. Hover
  to compare with your gear (Shift), Shift-click to link, Ctrl-click to try it on. Boss chests
  (Four Horsemen, Hodir, Gunship...) and hard-mode loot are included.
- **Quests**: the instance's quests with your status (to pick up, in progress, done, level too low,
  needs an earlier quest), who gives them and where, XP, money and rewards. Click a quest for its
  objectives and a map flag on its quest giver.
- **Your instance**: lockouts and resets per difficulty, entry requirements (keys, attunements),
  a map flag on the entrance, and, on progression realms, which instances your stage has opened.
- **Search** (top right) finds dungeons, bosses and loot: type an item to see who drops it.
- Opening the journal inside a dungeon jumps straight to it.

## What players get

**Home**: a tier menu (Classic, Burning Crusade, Wrath of the Lich King), Dungeons and Raids tabs,
and a tile per instance with its Dungeon Finder art. Instances your progression stage hasn't
opened carry a lock and the stage they open at.

**An instance**: its art, levels and entrance zone over the boss list. Multi-wing dungeons
(Scarlet Monastery, Dire Maul, Maraudon, Stratholme, Blackrock Depths) are split into their wings;
rares come last. On the right, the difficulty menu and four tabs:
- **Overview**: with no boss picked, the instance's difficulties with your lockout on each, what
  it takes to enter, the entrance (with a map flag), wings and a summary of its quests. With a boss
  picked, its model (drag to turn it), level, health and the creatures fought in the encounter.
- **Abilities**: per creature, each ability with icon, cast time, tags and description.
- **Loot**: drops with chances, class and slot filters. With no boss picked, every boss's loot.
- **Quests**: as above.

**Difficulty tabs** are the map's own (Normal and Heroic, 10 and 25 player, heroic raids). With
mod-individual-progression, **40 player Onyxia and Naxxramas** get tabs of their own under the
classic raids, while the 10 and 25 player versions stay under Wrath.

## How it works

The addon asks the server through addon whispers to itself (prefix `DJN`, like
[mod-retail-professions](https://github.com/buildthehomelab/wow-mod-retail-professions)' `RPR`).
At startup the module builds an index:
- **instances** from `LFGDungeons.dbc` and `Map.dbc`: one per map with the Dungeon Finder's wings,
  difficulties from `MapDifficulty.dbc`, entrances from `areatrigger_teleport`;
- **bosses** from `DungeonEncounter.dbc` and `instance_encounters` (each encounter goes to the wing
  its `lastEncounterDungeon` closes), plus rares spawned in the instance;
- **abilities** from the bosses' C++ scripts (`mod_dungeon_journal_script_spell`, see below),
  SmartAI scripts (following timed action lists, respecting difficulty flags), creature spells,
  and what bosses are seen casting (`mod_dungeon_journal_learned`). Spells with SpellDifficulty
  versions show the version of the chosen difficulty;
- **loot** from `creature_loot_template`, `gameobject_loot_template` and
  `reference_loot_template`, with chances worked out the way `LootTemplate::Process` rolls
  (groups, equal-chance entries, references and their multipliers, the realm's drop rates).
  Shared world drop tables are left out;
- **quests** sorted under the instance's zone, or asking for creatures or quest drops found only
  there; status per player from the core's own checks, XP including other modules' changes
  (such as mod-forever-dungeon-xp).

Per player, the server adds lockouts and killed bosses (the instance save's encounter mask),
entry requirements (`dungeon_access_requirements`) and the progression stage.

The addon keeps instance lists, boss lists and loot across sessions until the server's data stamp
changes; abilities for the session; status and quests are asked fresh.

### Filling the gaps

`data/sql/db-world/updates/mod_dungeon_journal_2026_10_06_00.sql` creates the module's tables and
seeds them:
- `mod_dungeon_journal_boss_extra`: loot chests (Cache of the Firelord, Four Horsemen Chest,
  Ulduar's caches with their hard modes, ICC's Gunship Armory...) and encounters fought as several
  creatures (Four Horsemen, Iron Council, Twin Emperors, Opera Event...).
- `mod_dungeon_journal_variant` and `mod_dungeon_journal_variant_swap`: extra difficulty tabs.
  The seed adds mod-individual-progression's 40 player Onyxia (creature 301000) and Naxxramas
  (difficulty 2), only when `IndividualProgression.Enable = 1`.

### Abilities from C++ scripts

Spells cast from C++ boss scripts aren't in the database, so
`tools/extract_script_spells.py` reads them from the script sources and writes
`data/sql/db-world/updates/mod_dungeon_journal_script_spells.sql`. Run it again when the core or a
module with boss scripts changes:

```
python3 tools/extract_script_spells.py \
    --out data/sql/db-world/updates/mod_dungeon_journal_script_spells.sql \
    azerothcore-wotlk/src/server/scripts/{EasternKingdoms,Kalimdor,Outland,Northrend,World} \
    mod-individual-progression/src
```

The checked-in file was made from mod-playerbots/azerothcore-wotlk f19a187 and
mod-individual-progression 723c510.

## Install

Server:
```
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-dungeon-journal.git mod-dungeon-journal
```
Re-run CMake, rebuild, and copy `conf/mod_dungeon_journal.conf.dist` next to your
`worldserver.conf`. The SQL in `data/sql/db-world` is applied by the database updater.

Client: copy `addon/DungeonJournal` into `Interface/AddOns`. On realms using Portalkeeper,
`sql/portalkeeper_addon.sql` (run by hand against `acore_world`) makes it a required addon.

To remove the module, take it out of the build and run
`data/sql/uninstall/mod_dungeon_journal_uninstall_world.sql`.

## Configuration

See `conf/mod_dungeon_journal.conf.dist`: master switch, rares, learning abilities, era locks,
the world drop threshold, showing world drops or grey and white items, and the quest cap.

## Commands

| Command | |
| --- | --- |
| `/dj` (Shift-J) | open or close the journal |
| `/dj <name>` | search |
| `/dj minimap` | show or hide the minimap button |
| `/dj reset` | move the window back to the middle |
| `/dj refresh` | fetch the journal data again |

## Development

`lua tools/addon_smoke_test.lua addon/DungeonJournal` loads the addon against a stubbed 3.3.5a API
and a fake server and walks the home page, era locks, an instance, a boss's abilities, loot and
its filters, quests, search, the 40 player tab and opening inside an instance.
`DRAGON=1` runs it with a DragonUI stand-in.

## License

MIT
