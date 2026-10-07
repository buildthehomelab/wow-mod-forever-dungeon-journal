/*
 * mod-forever-dungeon-journal
 *
 * Server half of a retail-style dungeon journal (the DungeonJournal addon). Everything the
 * journal shows comes from this realm's own data, so custom loot, mod-individual-progression's
 * vanilla raids and other modules' changes show up as they are:
 *
 *   instances  LFGDungeons.dbc and Map.dbc: wings, level ranges, difficulties, entrances
 *   bosses     DungeonEncounter.dbc and instance_encounters, plus the dungeon's rares
 *   abilities  C++ boss scripts (tools/extract_script_spells.py), SmartAI, creature spells and
 *              what bosses are seen casting
 *   loot       creature, gameobject (boss chests) and reference loot templates, with drop chances
 *   quests     quests sorted under the dungeon or asking for its creatures and drops, with the
 *              player's own status and rewards
 *
 * Per player it adds what only the server knows: lockouts, bosses killed this lockout, entry
 * requirements and, with mod-individual-progression, which instances the character's era has
 * unlocked. It never changes how anything plays.
 *
 * Released under the MIT License.
 */

#ifndef MOD_DUNGEON_JOURNAL_H
#define MOD_DUNGEON_JOURNAL_H

#include "Define.h"
#include <string>
#include <string_view>
#include <vector>

class Player;
class Unit;
class SpellInfo;

namespace DungeonJournal
{
    // Addon message prefix; the client sends "DJN\t<command>" as a whisper to itself.
    constexpr char const* PREFIX = "DJN";

    // Bumped when a message changes shape. The addon refuses the server data on a mismatch.
    constexpr uint32 PROTOCOL_VERSION = 1;

    // Leaves room for the prefix and tab inside the client's 255-byte chat message limit.
    constexpr std::size_t MAX_PAYLOAD = 240;

    // Capability bits in the HELLO answer.
    enum HelloFlags : uint32
    {
        HELLO_JOURNAL     = 0x01, // D, B, A, L, Q, QD, S, H, F
        HELLO_MAP_PINS    = 0x02, // M flags an entrance or a quest giver on the map
        HELLO_PROGRESSION = 0x04, // mod-individual-progression is on: eras lock instances
        HELLO_LEARNING    = 0x08, // boss abilities grow as bosses are seen casting
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
    };

    // Loot row flags.
    enum LootFlags : uint32
    {
        LOOT_QUEST        = 0x01, // only drops while you have the quest
        LOOT_HARD_MODE    = 0x02, // hard mode only (loot mode, or a hard mode chest)
        LOOT_CONDITION    = 0x04, // has extra drop conditions
        LOOT_CHEST        = 0x08, // comes from the encounter's chest
        LOOT_BOP          = 0x10, // binds when picked up
        LOOT_SHARED       = 0x20, // a shared table (many creatures drop it)
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
    };

    Config& GetConfig();

    // ---- DungeonJournal.cpp: transport -----------------------------------------------------------

    void Send(Player* player, std::string const& payload);

    // Sends "<header>:<row>;<row>;..." in as many messages as it takes, never splitting a row.
    void SendRows(Player* player, std::string const& header, std::vector<std::string> const& rows);

    void SendError(Player* player, std::string const& req, std::string_view what);

    bool ParseUInt(std::string_view text, uint32& out);

    // Names travel inside ','/';'/':' separated fields: those, '|' and '%' become %XX, and an
    // empty string becomes "-" (the addon's row parser drops empty values).
    std::string Escape(std::string_view text, std::size_t maxLength = 80);

    // mod-individual-progression's state (0-18), or 18 when it isn't running, for game masters
    // and for accounts it leaves alone.
    uint8 ProgressionState(Player* player);

    // ---- DungeonJournalIndex.cpp: instances, bosses, quests -------------------------------------

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
    }

    // ---- DungeonJournalLoot.cpp: loot tables and drop chances -----------------------------------

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

        enum class Store : uint8 { Creature, GameObject };

        // Loads the loot tables of the given creature and gameobject loot ids (and every
        // reference they use).
        void Load(std::vector<uint32> const& creatureLoot, std::vector<uint32> const& objectLoot);

        // What one kill (or one opened chest) of a loot id drops, most likely first.
        std::vector<Drop> const& For(Store store, uint32 lootId);

        // Folds the data into a hash, for the journal stamp.
        uint32 Hash(uint32 seed);
    }

    // ---- DungeonJournalAbilities.cpp: what a creature casts -------------------------------------

    namespace Abilities
    {
        struct Ability
        {
            uint32 spell = 0;
            uint32 tags = 0;     // AbilityTags
            uint32 castMs = 0;
        };

        // Reads the script table, the learned spells and the SmartAI scripts of these creatures.
        void Load(std::vector<uint32> const& baseEntries);

        // Spells the creature (a base entry) uses on a map difficulty, in a sensible order.
        std::vector<Ability> For(uint32 baseEntry, uint8 difficulty);

        // Creatures the creature summons, from its script and SmartAI.
        std::vector<uint32> const& Summons(uint32 baseEntry);

        // A creature was seen casting a spell; true when it was new.
        bool Learn(uint32 baseEntry, uint32 spell);

        // Tags the client shows beside an ability; 0 for spells the journal shouldn't list.
        bool IsListable(SpellInfo const* spell);
        uint32 TagsOf(SpellInfo const* spell);

        uint32 Hash(uint32 seed);
    }
}

#endif
