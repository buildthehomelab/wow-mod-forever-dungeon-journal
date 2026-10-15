/*
 * mod-forever-journal
 *
 * Server half of a retail-style journal (the Journal addon): a dungeon journal plus mount, pet
 * and toy collections. Everything it shows comes from this realm's own data, so custom loot,
 * mod-individual-progression's vanilla raids and other modules' changes show up as they are:
 *
 *   instances  LFGDungeons.dbc and Map.dbc: wings, level ranges, difficulties, entrances
 *   bosses     DungeonEncounter.dbc and instance_encounters, plus the dungeon's rares
 *   abilities  C++ boss scripts (tools/extract_script_spells.py), SmartAI, creature spells and
 *              what bosses are seen casting
 *   loot       creature, gameobject (boss chests) and reference loot templates, with drop chances
 *   quests     quests sorted under the dungeon or asking for its creatures and drops, with the
 *              player's own status and rewards; and every quest of the realm by zone, class,
 *              profession and world event, as the quest log sorts them
 *   mounts,    every mount, companion pet and toy players can get, and where from: vendors,
 *   pets, toys drops, quests, achievements, crafting, trainers, containers and fishing
 *   directory  every friendly trainer, flight master, bank, mailbox and other service on the
 *              continents, nearest first (the Town Guide spell and .find open it)
 *
 * Per player it adds what only the server knows: lockouts, bosses killed this lockout, entry
 * requirements and, with mod-individual-progression, which instances the character's era has
 * unlocked. It never changes how anything plays.
 *
 * Released under the MIT License.
 */

#ifndef MOD_JOURNAL_H
#define MOD_JOURNAL_H

#include "Define.h"
#include <array>
#include <string>
#include <string_view>
#include <vector>

class Player;
class Quest;
class Unit;
class SpellInfo;

namespace Journal
{
    // Addon message prefix; the client sends "JNL\t<command>" as a whisper to itself.
    constexpr char const* PREFIX = "JNL";

    // Bumped when a message changes shape. The addon refuses the server data on a mismatch.
    constexpr uint32 PROTOCOL_VERSION = 2;

    // Leaves room for the prefix and tab inside the client's 255-byte chat message limit.
    constexpr std::size_t MAX_PAYLOAD = 240;

    // Capability bits in the HELLO answer.
    enum HelloFlags : uint32
    {
        HELLO_JOURNAL     = 0x01, // D, B, A, L, Q, QD, S, H, F
        HELLO_MAP_PINS    = 0x02, // M flags an entrance or a quest giver on the map
        HELLO_PROGRESSION = 0x04, // mod-individual-progression is on: eras lock instances
        HELLO_LEARNING    = 0x08, // boss abilities grow as bosses are seen casting
        HELLO_COLLECTIONS = 0x10, // C, W, O: mounts, pets and toys
        HELLO_QUESTS      = 0x20, // G, Z, X: every quest of the realm
        HELLO_DIRECTORY   = 0x40, // N: the town directory, M:N flags a result
    };

    // What the addon can do, sent as HELLO's second field.
    enum AddonFeatureFlags : uint32
    {
        ADDON_OPEN        = 0x01, // opens a tab when the server sends OPEN:0:<tab>[:<argument>]
    };

    // Ability tags (the T field of an A row).
    enum AbilityTags : uint32
    {
        TAG_MAGIC         = 0x0001, // dispel type
        TAG_CURSE         = 0x0002,
        TAG_DISEASE       = 0x0004,
        TAG_POISON        = 0x0008,
        TAG_ENRAGE        = 0x0010,
        TAG_INTERRUPT     = 0x0020, // a cast or channel a kick or silence stops
        TAG_AOE           = 0x0040, // hits an area
        TAG_HEAL          = 0x0080, // heals
        TAG_SUMMON        = 0x0100, // summons creatures
        TAG_CC            = 0x0200, // fear, stun, polymorph, charm, sleep, horror...
        TAG_KNOCKBACK     = 0x0400,
        TAG_BUFF          = 0x0800, // a positive spell on itself or its allies
        TAG_LEARNED       = 0x1000, // only known because the boss was seen casting it
        // From mod_forever_journal_ability's Flags (shifted by ABILITY_FLAG_SHIFT):
        TAG_TANK          = 0x2000, // tanks need to know
        TAG_HEALER        = 0x4000, // healers need to know
        TAG_DAMAGE        = 0x8000, // damage dealers need to know
        TAG_IMPORTANT     = 0x10000,
        TAG_DEADLY        = 0x20000,
    };

    constexpr uint32 ABILITY_FLAG_SHIFT = 13;
    constexpr uint32 TAG_OVERRIDE_MASK = TAG_TANK | TAG_HEALER | TAG_DAMAGE | TAG_IMPORTANT | TAG_DEADLY;

    // Loot row flags.
    enum LootFlags : uint32
    {
        LOOT_QUEST        = 0x01, // only drops while you have the quest
        LOOT_HARD_MODE    = 0x02, // hard mode only (loot mode, or a hard mode chest)
        LOOT_CONDITION    = 0x04, // has extra drop conditions
        LOOT_CHEST        = 0x08, // comes from the encounter's chest
        LOOT_BOP          = 0x10, // binds when picked up
        LOOT_SHARED       = 0x20, // a shared table (many creatures drop it)
        LOOT_TOKEN        = 0x40, // turned in for other items (K lists them)
    };

    // Quest status for the player (the S field of a Q row).
    enum QuestState : uint8
    {
        QSTATE_AVAILABLE  = 0, // can be taken now
        QSTATE_ACTIVE     = 1, // in the quest log
        QSTATE_COMPLETE   = 2, // in the quest log, ready to turn in
        QSTATE_DONE       = 3, // turned in
        QSTATE_LOW_LEVEL  = 4, // level too low
        QSTATE_PREREQ     = 5, // an earlier quest of the chain first
        QSTATE_LOCKED     = 6, // anything else: reputation, skill, exclusive group, daily done...
        QSTATE_ERA        = 7, // the character's mod-individual-progression era hasn't reached it
    };

    struct Config
    {
        bool enabled = true;
        bool rares = true;
        bool learnAbilities = true;
        bool eraLocks = true;
        bool progressionEnabled = false;  // IndividualProgression.Enable
        uint8 zulGurubState = 3;          // IndividualProgression.RequiredZulGurubProgression
        uint8 zulAmanState = 12;          // IndividualProgression.RequiredZulAmanProgression
        uint32 worldDropThreshold = 25;   // a reference shared by more loot tables is a world drop
        bool showWorldDrops = false;
        bool showGrayLoot = false;
        uint32 maxQuestsPerInstance = 60;
        bool collections = true;
        bool showUnobtainable = false;
        bool quests = true;
        bool directory = true;
    };

    Config& GetConfig();

    // ---- Journal.cpp: transport -----------------------------------------------------------------

    void Send(Player* player, std::string const& payload);

    // Sends "<header>:<row>;<row>;..." in as many messages as it takes, never splitting a row.
    void SendRows(Player* player, std::string const& header, std::vector<std::string> const& rows);

    void SendError(Player* player, std::string const& req, std::string_view what);

    // The AddonFeatureFlags the character's addon sent with its last HELLO; 0 before one, and for
    // bots. Safe from map threads.
    uint32 AddonFeatures(Player* player);

    // Turns one feature off (an addon that stopped answering) or back on. Safe from map threads.
    void SetAddonFeature(Player* player, uint32 feature, bool on);

    bool ParseUInt(std::string_view text, uint32& out);

    // A DBC string in the server's DBC locale, or its enUS text when that one is empty.
    char const* DbcString(char const* const (&names)[16]);
    char const* DbcString(std::array<char const*, 16> const& names);

    // ASCII lower case, for searches.
    std::string Lower(std::string_view text);

    // Names travel inside ','/';'/':' separated fields: those, '|' and '%' become %XX, and an
    // empty string becomes "-" (the addon's row parser drops empty values).
    std::string Escape(std::string_view text, std::size_t maxLength = 80);

    // mod-individual-progression's state (0-18), or 18 when it isn't running, for game masters
    // and for accounts it leaves alone.
    uint8 ProgressionState(Player* player);

    // ---- JournalIndex.cpp: instances, bosses, quests --------------------------------------------

    namespace Index
    {
        // Reads the instances and everything in them. Only from OnStartup: entrances are placed
        // in zones from the map files, which isn't safe once the maps update.
        void Build();

        // Changes whenever the journal data does, so the addon knows when its cache is stale.
        uint32 Stamp();

        void HandleList(Player* player, std::string const& req);
        void HandleBosses(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        void HandleStatus(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        void HandleAbilities(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        void HandleLoot(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        void HandleQuests(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        void HandleQuestDetail(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        void HandleHere(Player* player, std::string const& req);
        void HandleMapPin(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        void HandleSearch(Player* player, std::string const& req, std::vector<std::string_view> const& args);

        // A creature cast a spell: remembered when it is a journal boss or add on its own map.
        void OnCreatureCast(Unit* caster, SpellInfo const* spell);

        // ---- shared with the collections --------------------------------------------------------

        std::string AreaName(uint32 areaId);
        std::string MapName(uint32 mapId);

        // "Zone (x, y), 40 yards away" for a spot.
        std::string WhereText(Player* player, uint32 map, float x, float y, uint32 zone);

        // A red flag on the player's map, when they're on the spot's map.
        void SendPoi(Player* player, uint32 map, float x, float y, std::string const& name);

        // "id*count/id*count", or "-".
        std::string ItemList(uint32 const* ids, uint32 const* counts, std::size_t n);

        // Where a source is in the dungeon journal.
        struct Place
        {
            uint32 instance = 0;  // journal instance id
            uint32 boss = 0;      // boss key, 0 = the instance itself
            uint8 reqState = 0;   // the era state it opens at: the most open of its tabs
            uint8 variant = 255;  // the first difficulty tab it is on, 255 = no particular one
            std::string label;    // those tabs ("Heroic", "25 Player"), empty when it is on all of them
            std::string name;     // the instance's name
        };

        // The journal boss a creature is, and the difficulty tabs on which the boss is fought as
        // exactly this creature entry (a heroic entry is only on the heroic tabs; 40 player
        // Onyxia's own creature only on its tab), or the boss whose chest an object is, and the
        // tabs the chest is opened on.
        bool FindBossOf(uint32 creatureEntry, Place& out);
        bool FindBossOfChest(uint32 objectEntry, Place& out);

        // The journal instance on a map.
        bool FindInstanceOnMap(uint32 map, Place& out);

        // The era state a map opens at, by the map alone: Outland (without the blood elf and
        // draenei zones, which need a spot) and Northrend, and instances by their own expansion
        // and mod-individual-progression's raid stages. 0 without era locks.
        uint8 MapEra(uint32 map);

        // A boss of the journal, or a creature anything summons (spells, SmartAI, boss scripts,
        // summon groups, scripts): a creature that can be met without a spawn of its own.
        bool IsSummonable(uint32 creatureEntry);
    }

    // ---- JournalLoot.cpp: loot tables and drop chances ------------------------------------------

    namespace Loot
    {
        struct Drop
        {
            uint32 item = 0;
            float chance = 0.0f;   // 0-100, the chance one kill drops it
            uint32 flags = 0;      // LootFlags
            uint8 minCount = 1;
            uint8 maxCount = 1;
        };

        enum class Store : uint8 { Creature, GameObject, Item, Fishing, Pickpocket };
        constexpr std::size_t STORE_COUNT = 5;

        // Loot ids to read, per Store.
        using LootIds = std::array<std::vector<uint32>, STORE_COUNT>;

        // Loads the loot tables of the given loot ids (and every reference they use).
        void Load(LootIds const& ids);

        // What one kill (or one opened chest, container, cast of the line or pick of a pocket)
        // of a loot id drops, most likely first.
        std::vector<Drop> const& For(Store store, uint32 lootId);

        // A reference shared by so many tables that it is a world drop table.
        bool IsWorldDropTable(uint32 reference);

        // Folds the data into a hash, for the journal stamp.
        uint32 Hash(uint32 seed);

        // ---- tokens -----------------------------------------------------------------------------

        // Reads the items a token is turned in for: quests that take it (one of it) and reward
        // items, and vendors whose extended cost takes it. After the quests are loaded.
        void BuildTokens();

        bool IsToken(uint32 item);

        // The classes (CLASSMASK bits) an item is for: its AllowableClass, or when it is shown as
        // a token every class one of its items is for; 0 when any class can use it.
        uint32 ClassesOf(uint32 item, bool asToken);

        // K:<req>:<item> -> KR, KD rows, KE: what a token turns into.
        void HandleTokens(Player* player, std::string const& req, std::vector<std::string_view> const& args);
    }

    // ---- JournalAbilities.cpp: what a creature casts --------------------------------------------

    namespace Abilities
    {
        struct Ability
        {
            uint32 spell = 0;
            uint32 tags = 0;     // AbilityTags
            uint32 castMs = 0;
            std::string text;    // written from the spell's effects when the client has no description
            std::string note;    // mod_forever_journal_ability's note
        };

        // Reads the script table, the learned spells and the SmartAI scripts of these creatures.
        void Load(std::vector<uint32> const& baseEntries);

        // Spells the creature (a base entry) uses on a map difficulty, in a sensible order.
        std::vector<Ability> For(uint32 baseEntry, uint8 difficulty);

        // Creatures the creature summons, from its script and SmartAI.
        std::vector<uint32> const& Summons(uint32 baseEntry);

        // The spell a creature casts a fixed time into the fight (its enrage timer), when known, in
        // its version for a map difficulty.
        bool EnrageOf(uint32 baseEntry, uint8 difficulty, uint32& spell, uint32& seconds);

        // A creature was seen casting a spell on a map difficulty; true when it was new there.
        bool Learn(uint32 baseEntry, uint32 spell, uint8 difficulty);

        // Tags the client shows beside an ability; 0 for spells the journal shouldn't list.
        bool IsListable(SpellInfo const* spell);
        uint32 TagsOf(SpellInfo const* spell);

        uint32 Hash(uint32 seed);
    }

    // ---- JournalCollections.cpp: mounts, pets and toys ------------------------------------------

    namespace Collections
    {
        // Before the loot is read: the catalog, and the loot ids that drop anything in it.
        void Prepare(Loot::LootIds& lootIds);

        // After the loot and the dungeon index: every entry's sources.
        void Build();

        uint32 Hash(uint32 seed);

        void HandleCatalog(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        void HandleSources(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        // O:<req> -> OR, OD rows "<item>,<in bags 0/1>", OE: toys in the bags (usable) or bank.
        void HandleOwnedToys(Player* player, std::string const& req);
        // M:<req>:S:<kind>:<id>:<source>: a red flag on a source.
        void HandleMapPin(Player* player, std::string const& req, std::vector<std::string_view> const& args);

        // The era state a spot on a map opens at: Outland and Northrend by zone (the blood elf
        // and draenei zones are classic's), instances by their journal tabs. 0 without era locks.
        uint8 ZoneEra(uint32 map, uint32 zone);
    }

    // ---- JournalQuests.cpp: every quest, as the quest log sorts them ----------------------------

    namespace Quests
    {
        // After the dungeon index: who gives every quest, where, and under which heading.
        void Build();

        // G:<req> -> GR, GD rows, GE: the headings, with the player's counts.
        void HandleCategories(Player* player, std::string const& req);
        // Z:<req>:<category> -> ZR, ZD rows (Q/R), ZE: a heading's quests.
        void HandleCategory(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        // X:<req>:<text> -> XR, XD rows (C/Q/R), XE: quests by name.
        void HandleSearch(Player* player, std::string const& req, std::vector<std::string_view> const& args);

        // The Q and R rows of a quest for the player (see Index::HandleQuests for their fields).
        // The caller has checked the player's race and class.
        void AppendRows(Player* player, Quest const* quest, char giverKind, uint32 giverEntry, std::string_view giver,
            std::string_view where, std::vector<std::string>& rows);

        // A red flag on the quest's giver and "M:<req>:ok:<where>"; false when nobody known gives it.
        bool ShowGiver(Player* player, std::string const& req, uint32 questId);
    }

    // ---- JournalDirectory.cpp: the town directory ----------------------------------------------

    namespace Directory
    {
        // Journal.Directory.*; at startup and on every config reload. The index is only built at
        // startup, so turning the Directory on at a reload waits for the next restart.
        void LoadConfig();

        // Reads every service NPC and object spawn. Only from OnStartup: zones the database
        // doesn't have yet are worked out from the map files.
        void Build();

        // Turned on and built.
        bool Ready();

        // N:<req>:<C|P|S>:<value> -> NR, ND rows, NE; N:<req>:R -> the character's profession skills.
        void HandleList(Player* player, std::string const& req, std::vector<std::string_view> const& args);
        // OPENED:<token>: the addon opened the tab an OPEN asked for.
        void HandleOpenAck(Player* player, std::string const& token);
        // M:<req>:N:<place>:<entry>: a red flag on a result.
        void HandleMapPin(Player* player, std::string const& req, std::vector<std::string_view> const& args);

        // The Town Guide spell, .find and the menu that stands in for the tab without the addon.
        void AddScripts();
    }
}

#endif
