# Journal

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module and addon that add a
**retail-style journal** in one window, in the spirit of WoW Forever's, with a tab for each part:

- **Dungeons** and **Raids**: every instance with its bosses, their abilities, the loot they drop
  with drop chances, and the instance's quests with your own progress on them.
- **Mounts**, **Pets** and **Toys**: everything there is to collect, what you already have, and
  where to get the rest: vendors and their prices, drops with their chances, quests,
  achievements, professions, trainers, containers and fishing.
- Tabs from other addons: [mod-beast-collection](https://github.com/buildthehomelab/wow-mod-beast-collection)'s
  hunter **Beasts** field guide and [mod-transmog-plus](https://github.com/buildthehomelab/wow-mod-transmog-plus)'s
  **Appearances** show up here when they're installed.

Open it with **Shift-J**, the minimap button, `/journal`, `/mounts`, `/pets` or `/toys`.

Everything comes from your realm's own data, so custom loot, custom mounts and pets,
mod-individual-progression's vanilla raids and other modules' changes show up as they are on your
server.

This module used to be **mod-forever-dungeon-journal** with the **DungeonJournal** addon. See
[Upgrading from mod-forever-dungeon-journal](#upgrading-from-mod-forever-dungeon-journal).

## Patch Notes: Journal

Category: Interface

- The **Dungeon Journal is now the Journal**: one window with tabs along the bottom, as retail's
  Collections window has. Shift-J still opens it.
- **Dungeons** and **Raids** are tabs of their own; everything the dungeon journal showed is still
  there.
- New **Mounts** and **Pets** tabs: every mount and companion pet on the realm, collected ones first,
  with a 3D model you can turn and a Summon button. For the ones you're missing, **where to get
  them**: the vendor (click to flag them on your map), the price in gold, honor, arena points or
  tokens, the boss or creature that drops it with its drop chance (click a boss to open it in the
  Dungeons tab), the quest and who starts it, the achievement, the profession recipe, or the
  trainer for class mounts.
- New **Toys** tab: the fun items (Orb of Deception, Piccolo of the Flaming Fire, the Brazier of
  Dancing Flames...), which ones are in your bags or bank, a Use button and where to get the rest.
- Filters: collected or not, usable by your character, mount type, and by source (vendor, drop,
  quest, achievement, profession, world event...). A search box on each tab.
- On progression realms, mounts, pets and toys your stage hasn't opened yet carry a lock and the
  stage they open at.
- The old DungeonJournal addon turns itself off the first time the Journal loads.

## What players get

The window has a tab along the bottom for each part of the journal. Tabs other addons add
(below) come after the journal's own.

### Dungeons and Raids

**Home**: a tier menu (Classic, Burning Crusade, Wrath of the Lich King) and a tile per instance
with its Dungeon Finder art. Instances your progression stage hasn't
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

### Mounts, Pets and Toys

On the left, the list: collected ones first in their quality colour, then the rest greyed out,
with a lock on those your progression stage hasn't opened. A search box, a Filter menu and the
collected count sit above it. On the right, the one you picked:
- its **model** (drag to turn it; toys show their icon and what using them does), whether it's for
  one faction or class, a flying mount or not, and a **Summon**, **Dismount** or **Use** button
  when you have it;
- **Where to get it**: every source the realm has, each on its own line:

| Source | Shows | Click |
| --- | --- | --- |
| Vendor | who, where, price (gold, honor, arena points, tokens), the world event they come with | flag on the map |
| Boss, drop, chest | who or what, where, the drop chance | a boss opens in the Dungeons tab, others flag the map |
| Pickpocket, fishing, container | who, the zone, or the container item, with the chance | |
| Quest | the quest, who starts it, where, its level and faction | flag the quest giver |
| Achievement | the achievement and what it asks | opens it in the achievement window |
| Profession | the profession, the skill it takes and the recipe | |
| Trainer | the class's trainers (class mounts), level and cost | flag the nearest trainer listed |
| World drop | drops from a shared world drop table | |

Up to eight sources of each kind are listed, most likely or cheapest first; a last line counts the
rest.

## How it works

The addon asks the server through addon whispers to itself (prefix `JNL`, like
[mod-retail-professions](https://github.com/buildthehomelab/wow-mod-retail-professions)' `RPR`).
At startup the module builds an index:
- **instances** from `LFGDungeons.dbc` and `Map.dbc`: one per map with the Dungeon Finder's wings,
  difficulties from `MapDifficulty.dbc`, entrances from `areatrigger_teleport`;
- **bosses** from `DungeonEncounter.dbc` and `instance_encounters` (each encounter goes to the wing
  its `lastEncounterDungeon` closes), plus rares spawned in the instance;
- **abilities** from the bosses' C++ scripts (`mod_forever_journal_script_spell`, see below),
  SmartAI scripts (following timed action lists, respecting difficulty flags), creature spells,
  and what bosses are seen casting (`mod_forever_journal_learned`). Spells with SpellDifficulty
  versions show the version of the chosen difficulty;
- **loot** from `creature_loot_template`, `gameobject_loot_template` and
  `reference_loot_template`, with chances worked out the way `LootTemplate::Process` rolls
  (groups, equal-chance entries, references and their multipliers, the realm's drop rates).
  Shared world drop tables are left out;
- **quests** sorted under the instance's zone, or asking for creatures or quest drops found only
  there; status per player from the core's own checks, XP including other modules' changes
  (such as mod-forever-dungeon-xp);
- **mounts** from the Mounts skill line (`SkillLineAbility.dbc` 777) plus any mount spell an item,
  trainer or quest teaches; **pets** from the Companion Pets skill line (778) plus any companion
  an item teaches, so custom mounts and pets count too; **toys** from `mod_forever_journal_toy`;
- their **sources** from `npc_vendor` (with the vendor lists it references, `ItemExtendedCost.dbc`
  and `game_event_creature` for event vendors), the creature, gameobject, pickpocketing, fishing,
  item and reference loot tables (chances worked out as above), quest rewards and their
  `creature_queststarter` / `gameobject_queststarter`, `achievement_reward`, profession spells
  that create the item, `trainer_spell`, and notes in `mod_forever_journal_collection_note`.
  Mounts, pets and toys with no source at all (promotions, test items) are left out.

Per player, the server adds lockouts and killed bosses (the instance save's encounter mask),
entry requirements (`dungeon_access_requirements`), the progression stage and the toys in the
bags and bank. The client itself knows which mounts and pets a character has.

With mod-individual-progression each source carries the stage that opens it: Outland (outside
the blood elf and draenei zones) and Northrend spots, the dungeon journal's instance locks, and
`conditions` on vendors and quests that ask for its state quests. A mount, pet or toy is locked
while every source is.

The addon keeps instance lists, boss lists, loot and the mount, pet and toy lists across sessions
until the server's data stamp changes; abilities and sources for the session; status, quests and
toys are asked fresh.

### Filling the gaps

`data/sql/db-world/updates/mod_forever_journal_2026_10_06_00.sql` creates the module's tables and
seeds them:
- `mod_forever_journal_boss_extra`: loot chests (Cache of the Firelord, Four Horsemen Chest,
  Ulduar's caches with their hard modes, ICC's Gunship Armory...) and encounters fought as several
  creatures (Four Horsemen, Iron Council, Twin Emperors, Opera Event...).
- `mod_forever_journal_variant` and `mod_forever_journal_variant_swap`: extra difficulty tabs.
  The seed adds mod-individual-progression's 40 player Onyxia (creature 301000) and Naxxramas
  (difficulty 2), only when `IndividualProgression.Enable = 1`.

`data/sql/db-world/updates/mod_forever_journal_2026_10_14_00.sql` adds:
- `mod_forever_journal_toy`: the Toys tab's items. The seed is retail's toy box (`Toy.db2`) as far
  as 3.3.5 has the items: 69 of them. Add a row for any other item.
- `mod_forever_journal_collection_note`: a line of text shown with a mount, pet or toy's sources
  (`Kind` M, P or T; `ID` the spell for mounts and pets, the item for toys), for sources the data
  can't tell, such as a reward your realm hands out by hand.

### Abilities from C++ scripts

Spells cast from C++ boss scripts aren't in the database, so
`tools/extract_script_spells.py` reads them from the script sources and writes
`data/sql/db-world/updates/mod_forever_journal_script_spells.sql`. Run it again when the core or a
module with boss scripts changes:

```
python3 tools/extract_script_spells.py \
    --out data/sql/db-world/updates/mod_forever_journal_script_spells.sql \
    azerothcore-wotlk/src/server/scripts/{EasternKingdoms,Kalimdor,Outland,Northrend,World} \
    mod-individual-progression/src
```

The checked-in file was made from mod-playerbots/azerothcore-wotlk f19a187 and
mod-individual-progression 723c510.

## Requirements

- [AzerothCore](https://www.azerothcore.org/) wotlk (master) and a WoW 3.3.5a (12340) client.
- The bundled `Journal` addon in `Interface/AddOns`. The module answers the addon, so it
  does nothing without it.
- [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression) is
  optional. With it the journal shows era locks and the 40 player Onyxia and Naxxramas tabs.
- No client patch. [Portalkeeper](https://github.com/Hisha/Portalkeeper) is optional
  (`sql/portalkeeper_addon.sql` makes the addon required for players).

## Installation

Server:
```
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-forever-journal.git mod-forever-journal
```
Re-run CMake, rebuild, and copy `conf/mod_forever_journal.conf.dist` next to your
`worldserver.conf`. The SQL in `data/sql/db-world` is applied by the database updater.

Client: copy `addon/Journal` into `Interface/AddOns`. On realms using Portalkeeper,
`sql/portalkeeper_addon.sql` (run by hand against `acore_world`) makes it a required addon.

To remove the module, take it out of the build and run
`data/sql/uninstall/mod_forever_journal_uninstall_world.sql`.

## Configuration

See `conf/mod_forever_journal.conf.dist`: master switch, rares, learning abilities, era locks,
the world drop threshold, showing world drops or grey and white items, the quest cap, the
collections (`Journal.Collections.Enable`) and listing unobtainable mounts, pets and toys
(`Journal.Collections.ShowUnobtainable`).

## Upgrading from mod-forever-dungeon-journal

- Server: the module folder is now `mod-forever-journal` (AzerothCore names the loader after the
  folder, so a clone under the old name won't link). Clone it again under the new name, or rename
  the folder, then re-run CMake and rebuild. The conf file is now `mod_forever_journal.conf` and
  its keys start with `Journal.` instead of `DungeonJournal.`.
- Database: the update `mod_forever_journal_2026_10_14_00.sql` moves the learned boss abilities to
  the new table names and drops the old `mod_forever_dungeon_journal_*` tables.
- Client: the addon folder is now `Journal`. The first time it loads it turns the old
  `DungeonJournal` addon off and moves Shift-J over; delete `Interface/AddOns/DungeonJournal` when
  you like. On Portalkeeper realms, replace the `DungeonJournal` row with the one in
  `sql/portalkeeper_addon.sql`.

## Tabs from other addons

Any addon can put a tab of its own in the journal window. Add `## OptionalDeps: Journal` to its
.toc so the journal loads first, then:

```lua
if Journal and Journal.RegisterTab then
    Journal.RegisterTab({
        key = "beasts",          -- unique; Journal.OpenTab("beasts") opens the journal on it
        name = "Beasts",         -- the tab's text, and the window title while it's shown
        order = 60,              -- place among the tabs (the journal's own use 10-50)
        icon = "Interface\\Icons\\Ability_Hunter_BeastTaming",  -- the window's portrait
        build = function (panel) end,   -- once, the first time the tab is shown
        onShow = function (panel) end,  -- every time it's shown (optional)
        onHide = function (panel) end,  -- (optional)
    })
end
```

`panel` fills the window's content area (about 870 by 530). `Journal.API` is the version of this
interface (1). `Journal.IsTabShown(key)` tells whether the journal is open on a tab.

## Commands

| Command | |
| --- | --- |
| `/journal` or `/dj` (Shift-J) | open or close the journal |
| `/mounts`, `/pets`, `/toys` | open or close the journal on that tab (key bindings too) |
| `/journal <tab>` | open a tab by its key: `dungeons`, `raids`, `mounts`, `beasts`... |
| `/journal <name>` | search dungeons, bosses and loot |
| `/journal minimap` | show or hide the minimap button |
| `/journal reset` | move the window back to the middle |
| `/journal refresh` | fetch the journal data again |

## Development

`lua tools/addon_smoke_test.lua addon/Journal` loads the addon against a stubbed 3.3.5a API
and a fake server and walks the home page, era locks, an instance, a boss's abilities, loot and
its filters, quests, search, the 40 player tab, opening inside an instance, the Mounts, Pets and
Toys tabs (sources, map flags, boss links, Summon, filters) and a tab registered by another addon.
`DRAGON=1` runs it with a DragonUI stand-in.

## Troubleshooting

- **The journal is empty or out of date:** type `/journal refresh` to fetch the data again. The addon
  keeps its lists until the server's data stamp changes.
- **The window is off screen:** `/journal reset` moves it back to the middle. `/journal minimap` shows or
  hides the minimap button.
- **Shift-J does nothing:** check that the addon is in `Interface/AddOns/Journal`. `/journal`
  and the minimap button open the journal too. If Shift-J was bound to something else before, the
  journal leaves it alone: bind "Toggle the Journal" under Key Bindings, Journal.
- **A mount or pet is missing:** the tab lists only what something on the realm sells, drops or
  gives. `Journal.Collections.ShowUnobtainable = 1` lists the rest too. A spell the client doesn't
  have (a custom mount without its client patch) can't be shown.
- **A boss with a C++ script shows no abilities:** run `tools/extract_script_spells.py` against
  your core and modules to regenerate the script spell SQL. Abilities are also learned as bosses
  are seen casting them.
- **No 40 player tabs or era locks:** they need mod-individual-progression with
  `IndividualProgression.Enable = 1`.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT, see [LICENSE](LICENSE).
