/*
 * mod-forever-journal: the Mounts, Pets and Toys tabs.
 *
 * The catalog, built once at startup:
 *   mounts  spells of the Mounts skill line (777), and any other mount spell an item, a trainer
 *           or a quest teaches (class mounts, custom mounts without a skill line row)
 *   pets    spells of the Companion Pets skill line (778), and any other companion an item teaches
 *   toys    the items in mod_forever_journal_toy (retail's toy box, as far as 3.3.5 has them)
 *
 * Where each entry comes from:
 *   V  vendors (npc_vendor and the vendor lists it references; price, honor, arena points and
 *      items; vendors only spawned by a world event say so)
 *   D  creature drops, G chests, P pickpocketing, F fishing, I containers, with the chance per
 *      kill, chest, pocket, cast or container worked out like the dungeon journal's loot. Drops of
 *      a dungeon journal boss link to it. R: only through a world drop table.
 *   Q  quest rewards (items, choices, reward spells), with who starts the quest
 *   A  achievement rewards (achievement_reward)
 *   C  crafted: a profession spell creates the item
 *   T  trainers (class mounts)
 *   N  notes from mod_forever_journal_collection_note, for what the data can't tell
 * Entries with no source at all (promotions, test spells) are left out unless
 * Journal.Collections.ShowUnobtainable is on.
 *
 * With mod-individual-progression each source carries the era state that opens it: Outland and
 * Northrend spots, the dungeon journal's instance locks, and vendor and quest conditions on
 * mod-individual-progression's state quests. An entry is locked while every source is.
 *
 * The client knows which mounts and pets a character has (GetCompanionInfo); for toys the
 * server says which ones are in the character's bags or bank.
 *
 * Messages (see Journal.cpp for the wire format):
 *   C:<req>:<M|P|T>       -> CR:<req>:<kind>:<entries>, CD rows, CE
 *       <id>,<creature>,<item>,<quality>,<flags>,<class mask>,<source bits>,<era state>
 *       id = the spell for mounts and pets, the item for toys
 *   W:<req>:<kind>:<id>   -> WR:<req>:<kind>:<id>:<sources>, WD rows (below), WE
 *   O:<req>               -> OR:<req>:<toys>, OD rows (item ids), OE   toys the character has
 *   M:<req>:S:<kind>:<id>:<source> -> M:<req>:ok:<where>               a map flag on a source
 *
 * WD rows, each with the source's index <i> second (for M):
 *   V,<i>,<npc>,<copper>,<honor>,<arena>,<items>,<era>,<placed>,<name>,<where>,<event>
 *   D,<i>,<creature>,<chance x100>,<instance>,<boss>,<era>,<placed>,<rare>,<name>,<where>
 *   G,<i>,<object>,<chance x100>,<instance>,<boss>,<era>,<placed>,<name>,<where>
 *   P,<i>,<creature>,<chance x100>,<era>,<placed>,<name>,<where>
 *   F,<i>,<area>,<chance x100>,<era>,<zone>
 *   I,<i>,<container item>,<chance x100>
 *   R,<i>
 *   Q,<i>,<quest>,<level>,<era>,<placed>,<faction 0/1 Alliance/2 Horde>,<title>,<giver>,<where>
 *   A,<i>,<achievement>
 *   C,<i>,<spell>,<skill>,<rank>,<profession>
 *   T,<i>,<class>,<level>,<copper>,<era>,<placed>,<trainers>,<name>,<where>
 *   N,<i>,<text>
 *   +,<i>,<row kind>,<more>      more sources of that kind than were sent
 */

#include "Journal.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Journal::Collections
{
    namespace
    {
        enum Kind : uint8
        {
            KIND_MOUNT = 0,
            KIND_PET   = 1,
            KIND_TOY   = 2,
            KIND_COUNT = 3,
        };

        constexpr char KIND_LETTERS[KIND_COUNT] = { 'M', 'P', 'T' };

        // The <flags> field of a catalog row.
        enum EntryFlags : uint32
        {
            ENTRY_ALLIANCE     = 0x01, // only Alliance characters can use or buy it
            ENTRY_HORDE        = 0x02,
            ENTRY_FLYING       = 0x04,
            ENTRY_CLASS        = 0x08, // a class's own (the class mask says which)
            ENTRY_UNOBTAINABLE = 0x10, // no source found (only with ShowUnobtainable)
        };

        // The <source bits> field: which kinds of source an entry has, for the addon's filters.
        enum SourceBits : uint32
        {
            SRC_VENDOR      = 0x001,
            SRC_DROP        = 0x002, // creatures, chests, pickpocketing, world drops
            SRC_QUEST       = 0x004,
            SRC_ACHIEVEMENT = 0x008,
            SRC_CRAFTED     = 0x010,
            SRC_TRAINER     = 0x020,
            SRC_CONTAINER   = 0x040,
            SRC_FISHING     = 0x080,
            SRC_EVENT       = 0x100, // a world event's vendor
            SRC_NOTE        = 0x200,
        };

        // mod-individual-progression states (IndividualProgression.h)
        constexpr uint8 IP_TBC = 8;
        constexpr uint8 IP_SUNWELL = 12;
        constexpr uint8 IP_WOTLK = 13;
        constexpr uint32 IP_QUEST_BASE = 66000;
        constexpr uint32 IP_STATE_MAX = 18;

        constexpr uint32 RACEMASK_ALLIANCE = 1101; // Human, Dwarf, Night Elf, Gnome, Draenei
        constexpr uint32 RACEMASK_HORDE = 690;     // Orc, Undead, Tauren, Troll, Blood Elf
        constexpr uint32 CLASSMASK_ALL = 0x5FF;

        // conditions
        constexpr uint32 COND_SOURCE_QUEST_AVAILABLE = 19;
        constexpr uint32 COND_SOURCE_NPC_VENDOR = 23;
        constexpr uint32 COND_QUESTREWARDED = 8;

        // Sources of one kind sent per entry; the rest are counted in a '+' row.
        constexpr std::size_t MAX_PER_KIND = 8;

        struct Spot
        {
            bool placed = false;
            uint32 map = 0;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            uint32 zone = 0;
        };

        struct Source
        {
            char kind = 'N';
            uint32 id = 0;
            uint32 a = 0;          // V copper, D/G/P/F/I chance x100, Q level, C skill, T class
            uint32 b = 0;          // V honor, D/G instance, C rank, T level, Q faction
            uint32 c = 0;          // V arena, D/G boss, T copper, D rare
            uint32 d = 0;          // T trainer count
            uint8 reqState = 0;
            std::string items;     // V: extended cost items
            std::string name;
            std::string where;
            std::string extra;     // V: event, Q: who starts it
            uint32 at = 0;         // T: the trainer whose spot is shown
            Spot spot;
            float sort = 0.0f;     // higher first within a kind
        };

        struct Entry
        {
            uint8 kind = KIND_MOUNT;
            uint32 id = 0;         // the spell for mounts and pets, the item for toys
            uint32 spell = 0;
            uint32 creature = 0;
            std::vector<uint32> items; // items that teach it (mounts, pets) or the toy itself
            uint8 quality = 1;
            uint32 flags = 0;
            uint32 classMask = 0;
            uint32 sources = 0;
            uint8 reqState = 0;
            std::vector<Source> list;
            std::map<char, uint32> more; // kind -> sources left out
        };

        struct Ref
        {
            uint8 kind = 0;
            std::size_t index = 0;
        };

        struct Data
        {
            std::array<std::vector<Entry>, KIND_COUNT> entries;
            std::array<std::unordered_map<uint32, std::size_t>, KIND_COUNT> byId;
            std::unordered_map<uint32, std::vector<Ref>> byItem;   // teaching item or toy -> entries
            std::unordered_map<uint32, std::vector<Ref>> bySpell;  // mount or pet spell -> entries
            // loot id -> catalog items its table can give, per loot store
            std::array<std::unordered_map<uint32, std::unordered_set<uint32>>, Loot::STORE_COUNT> lootItems;
            std::unordered_map<uint32, std::vector<std::pair<uint32, uint32>>> trainers; // spell -> (trainer id, row)
            uint32 hash = 0;
        };

        Data sData;

        struct TrainerRow
        {
            uint32 trainer = 0;
            uint32 cls = 0;     // class id for class trainers, 0 otherwise
            uint32 cost = 0;
            uint8 level = 0;
        };

        std::unordered_map<uint32, std::vector<TrainerRow>> sTrainerRows; // learned spell -> rows
        std::unordered_map<uint32, std::vector<uint32>> sQuestsBySpell;   // learned spell -> quests
        std::unordered_map<uint32, std::vector<uint32>> sQuestsByItem;    // item -> quests

        // ---- helpers ----------------------------------------------------------------------------

        uint32 Fnv(uint32 h, uint32 v)
        {
            for (int i = 0; i < 4; ++i)
            {
                h ^= (v >> (i * 8)) & 0xFF;
                h *= 16777619u;
            }
            return h;
        }

        std::string IdList(std::vector<uint32> const& ids, std::size_t from, std::size_t count)
        {
            std::string list;
            for (std::size_t i = from; i < std::min(ids.size(), from + count); ++i)
            {
                if (!list.empty())
                    list += ',';
                list += std::to_string(ids[i]);
            }
            return list;
        }

        // Runs a query once per 500 ids: "{}" in the text takes the id list.
        template <class Fn>
        void QueryIn(std::string_view text, std::vector<uint32> const& ids, Fn&& onRow)
        {
            for (std::size_t i = 0; i < ids.size(); i += 500)
            {
                std::string query = Acore::StringFormat(text, IdList(ids, i, 500));
                if (QueryResult result = WorldDatabase.Query(query))
                    do
                    {
                        onRow(result->Fetch());
                    } while (result->NextRow());
            }
        }

        template <class Set>
        std::vector<uint32> Sorted(Set const& set)
        {
            std::vector<uint32> out(set.begin(), set.end());
            std::sort(out.begin(), out.end());
            return out;
        }

        // The spell a spell teaches when cast (the "learn" spells items and trainers use), or 0.
        uint32 LearnedBy(SpellInfo const* spell)
        {
            if (!spell)
                return 0;
            for (SpellEffectInfo const& eff : spell->Effects)
                if (eff.Effect == SPELL_EFFECT_LEARN_SPELL && eff.TriggerSpell)
                    return eff.TriggerSpell;
            return 0;
        }

        // A spell that puts you on a mount: the creature it shows, flying or not.
        bool IsMountSpell(SpellInfo const* spell, uint32& creature, bool& flying)
        {
            if (!spell)
                return false;
            bool mount = false;
            for (SpellEffectInfo const& eff : spell->Effects)
            {
                if (eff.Effect != SPELL_EFFECT_APPLY_AURA)
                    continue;
                if (eff.ApplyAuraName == SPELL_AURA_MOUNTED)
                {
                    mount = true;
                    if (eff.MiscValue > 0)
                        creature = uint32(eff.MiscValue);
                }
                else if (eff.ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED || eff.ApplyAuraName == SPELL_AURA_FLY)
                    flying = true;
            }
            return mount;
        }

        // A spell that summons a companion (a mini pet): the creature.
        bool IsCompanionSpell(SpellInfo const* spell, uint32& creature)
        {
            if (!spell)
                return false;
            for (SpellEffectInfo const& eff : spell->Effects)
            {
                if (eff.Effect != SPELL_EFFECT_SUMMON || eff.MiscValue <= 0)
                    continue;
                SummonPropertiesEntry const* props = sSummonPropertiesStore.LookupEntry(uint32(eff.MiscValueB));
                if (props && props->Type == SUMMON_TYPE_MINIPET)
                {
                    creature = uint32(eff.MiscValue);
                    return true;
                }
            }
            return false;
        }

        Entry* Find(uint8 kind, uint32 id)
        {
            auto itr = sData.byId[kind].find(id);
            return itr != sData.byId[kind].end() ? &sData.entries[kind][itr->second] : nullptr;
        }

        Entry* Get(Ref const& ref)
        {
            return &sData.entries[ref.kind][ref.index];
        }

        // Adds a mount or pet for a spell, or finds the one there is.
        Entry* AddSpellEntry(uint32 spellId)
        {
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
            if (!spell)
                return nullptr;
            uint32 creature = 0;
            bool flying = false;
            uint8 kind;
            if (IsMountSpell(spell, creature, flying))
                kind = KIND_MOUNT;
            else if (IsCompanionSpell(spell, creature))
                kind = KIND_PET;
            else
                return nullptr;

            if (Entry* existing = Find(kind, spellId))
                return existing;

            Entry e;
            e.kind = kind;
            e.id = spellId;
            e.spell = spellId;
            e.creature = creature;
            if (flying)
                e.flags |= ENTRY_FLYING;
            sData.byId[kind][spellId] = sData.entries[kind].size();
            sData.bySpell[spellId].push_back({ kind, sData.entries[kind].size() });
            sData.entries[kind].push_back(std::move(e));
            return &sData.entries[kind].back();
        }

        void LinkItem(Entry& e, uint8 kind, std::size_t index, uint32 item)
        {
            if (std::find(e.items.begin(), e.items.end(), item) != e.items.end())
                return;
            e.items.push_back(item);
            sData.byItem[item].push_back({ kind, index });
        }

        // ---- the catalog ------------------------------------------------------------------------

        void ReadTrainers()
        {
            std::unordered_map<uint32, uint32> trainerClass;
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(Id AS UNSIGNED), CAST(Type AS UNSIGNED), "
                "CAST(Requirement AS UNSIGNED) FROM trainer"))
                do
                {
                    Field* f = result->Fetch();
                    // Type 0 = class trainer, its requirement the class.
                    trainerClass[uint32(f[0].Get<uint64>())] = f[1].Get<uint64>() == 0 ? uint32(f[2].Get<uint64>()) : 0;
                } while (result->NextRow());

            if (QueryResult result = WorldDatabase.Query("SELECT CAST(TrainerId AS UNSIGNED), CAST(SpellId AS UNSIGNED), "
                "CAST(MoneyCost AS UNSIGNED), CAST(ReqLevel AS UNSIGNED) FROM trainer_spell"))
                do
                {
                    Field* f = result->Fetch();
                    uint32 spellId = uint32(f[1].Get<uint64>());
                    SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
                    if (!spell)
                        continue;
                    TrainerRow row;
                    row.trainer = uint32(f[0].Get<uint64>());
                    row.cls = trainerClass.count(row.trainer) ? trainerClass[row.trainer] : 0;
                    row.cost = uint32(f[2].Get<uint64>());
                    row.level = uint8(f[3].Get<uint64>());
                    uint32 learned = LearnedBy(spell);
                    sTrainerRows[learned ? learned : spellId].push_back(row);
                } while (result->NextRow());
        }

        void ReadQuestRewards()
        {
            for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
            {
                for (uint32 spellId : { quest->GetRewSpell(), uint32(std::max<int32>(0, quest->GetRewSpellCast())) })
                {
                    if (!spellId)
                        continue;
                    uint32 learned = LearnedBy(sSpellMgr->GetSpellInfo(spellId));
                    sQuestsBySpell[learned ? learned : spellId].push_back(id);
                }
                for (uint32 item : quest->RewardItemId)
                    if (item)
                        sQuestsByItem[item].push_back(id);
                for (uint32 item : quest->RewardChoiceItemId)
                    if (item)
                        sQuestsByItem[item].push_back(id);
            }
        }

        void BuildCatalog()
        {
            // The skill lines first: their spells are the mounts and pets, whatever teaches them.
            for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
            {
                SkillLineAbilityEntry const* sla = sSkillLineAbilityStore.LookupEntry(i);
                if (!sla || (sla->SkillLine != SKILL_MOUNTS && sla->SkillLine != SKILL_COMPANIONS))
                    continue;
                AddSpellEntry(sla->Spell);
            }

            // Items that teach a mount or a pet: the spell_2 "learn" trigger, or a use spell that
            // teaches one. Custom mounts and pets without a skill line row join here.
            for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
            {
                for (auto const& s : proto.Spells)
                {
                    if (s.SpellId <= 0)
                        continue;
                    uint32 taught = 0;
                    if (s.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID)
                        taught = uint32(s.SpellId);
                    else if (s.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                        taught = LearnedBy(sSpellMgr->GetSpellInfo(uint32(s.SpellId)));
                    if (!taught)
                        continue;
                    Entry* e = AddSpellEntry(taught);
                    if (!e)
                        continue;
                    LinkItem(*e, e->kind, sData.byId[e->kind][e->id], itemId);
                }
            }

            // Mounts trainers teach (paladins', warlocks') and quests give (death knights').
            for (auto const& [spellId, rows] : sTrainerRows)
            {
                uint32 creature = 0;
                bool flying = false;
                if (IsMountSpell(sSpellMgr->GetSpellInfo(spellId), creature, flying))
                    AddSpellEntry(spellId);
            }
            for (auto const& [spellId, quests] : sQuestsBySpell)
                AddSpellEntry(spellId);

            // Toys.
            if (QueryResult result = WorldDatabase.Query("SELECT ItemID FROM mod_forever_journal_toy ORDER BY ItemID"))
                do
                {
                    uint32 itemId = result->Fetch()[0].Get<uint32>();
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                    if (!proto || Find(KIND_TOY, itemId))
                        continue;
                    Entry e;
                    e.kind = KIND_TOY;
                    e.id = itemId;
                    for (auto const& s : proto->Spells)
                        if (s.SpellId > 0 && s.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                        {
                            e.spell = uint32(s.SpellId);
                            break;
                        }
                    std::size_t index = sData.entries[KIND_TOY].size();
                    sData.byId[KIND_TOY][itemId] = index;
                    sData.entries[KIND_TOY].push_back(std::move(e));
                    LinkItem(sData.entries[KIND_TOY][index], KIND_TOY, index, itemId);
                } while (result->NextRow());

            // Item details: quality, faction and class.
            for (uint8 kind = 0; kind < KIND_COUNT; ++kind)
                for (Entry& e : sData.entries[kind])
                {
                    uint32 races = 0;
                    uint32 classes = 0;
                    bool anyRace = e.items.empty();
                    bool anyClass = e.items.empty();
                    uint8 quality = 0;
                    for (uint32 itemId : e.items)
                    {
                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                        if (!proto)
                            continue;
                        quality = std::max<uint8>(quality, uint8(proto->Quality));
                        uint32 r = proto->AllowableRace & (RACEMASK_ALLIANCE | RACEMASK_HORDE);
                        if (proto->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE))
                            r &= RACEMASK_ALLIANCE;
                        else if (proto->HasFlag2(ITEM_FLAG2_FACTION_HORDE))
                            r &= RACEMASK_HORDE;
                        if (!r || r == (RACEMASK_ALLIANCE | RACEMASK_HORDE))
                            anyRace = true;
                        races |= r;
                        uint32 c = proto->AllowableClass & CLASSMASK_ALL;
                        if (!c || c == CLASSMASK_ALL)
                            anyClass = true;
                        classes |= c;
                    }
                    e.quality = std::max<uint8>(1, quality);
                    if (!anyRace)
                    {
                        if (!(races & RACEMASK_HORDE))
                            e.flags |= ENTRY_ALLIANCE;
                        else if (!(races & RACEMASK_ALLIANCE))
                            e.flags |= ENTRY_HORDE;
                    }
                    if (!anyClass && classes)
                    {
                        e.flags |= ENTRY_CLASS;
                        e.classMask = classes;
                    }
                }
        }

        // ---- loot ids that can give catalog items -----------------------------------------------

        void FindLoot(Loot::LootIds& lootIds)
        {
            std::vector<uint32> items;
            for (auto const& [item, refs] : sData.byItem)
                items.push_back(item);
            std::sort(items.begin(), items.end());
            if (items.empty())
                return;

            // References that reach a catalog item, and which items, all the way up.
            std::unordered_map<uint32, std::unordered_set<uint32>> refItems;
            QueryIn("SELECT CAST(Entry AS UNSIGNED), CAST(Item AS UNSIGNED) FROM reference_loot_template "
                "WHERE Reference = 0 AND Item IN ({})", items, [&](Field* f)
            {
                refItems[uint32(f[0].Get<uint64>())].insert(uint32(f[1].Get<uint64>()));
            });
            std::vector<uint32> frontier = Sorted(std::unordered_set<uint32>([&]
            {
                std::unordered_set<uint32> s;
                for (auto const& [ref, set] : refItems)
                    s.insert(ref);
                return s;
            }()));
            for (uint32 depth = 0; depth < 6 && !frontier.empty(); ++depth)
            {
                std::unordered_set<uint32> next;
                QueryIn("SELECT CAST(Entry AS UNSIGNED), CAST(ABS(Reference) AS UNSIGNED) FROM reference_loot_template "
                    "WHERE ABS(Reference) IN ({})", frontier, [&](Field* f)
                {
                    uint32 parent = uint32(f[0].Get<uint64>());
                    uint32 child = uint32(f[1].Get<uint64>());
                    auto& set = refItems[parent];
                    std::size_t before = set.size();
                    set.insert(refItems[child].begin(), refItems[child].end());
                    if (set.size() != before)
                        next.insert(parent);
                });
                frontier = Sorted(next);
            }
            std::vector<uint32> refs;
            for (auto const& [ref, set] : refItems)
                refs.push_back(ref);
            std::sort(refs.begin(), refs.end());

            static char const* const TABLES[Loot::STORE_COUNT] = { "creature_loot_template", "gameobject_loot_template",
                "item_loot_template", "fishing_loot_template", "pickpocketing_loot_template" };
            for (std::size_t store = 0; store < Loot::STORE_COUNT; ++store)
            {
                auto& found = sData.lootItems[store];
                QueryIn(Acore::StringFormat("SELECT CAST(Entry AS UNSIGNED), CAST(Item AS UNSIGNED) FROM {} "
                    "WHERE Reference = 0 AND Item IN ({{}})", TABLES[store]), items, [&](Field* f)
                {
                    found[uint32(f[0].Get<uint64>())].insert(uint32(f[1].Get<uint64>()));
                });
                QueryIn(Acore::StringFormat("SELECT CAST(Entry AS UNSIGNED), CAST(ABS(Reference) AS UNSIGNED) FROM {} "
                    "WHERE ABS(Reference) IN ({{}})", TABLES[store]), refs, [&](Field* f)
                {
                    auto const& set = refItems[uint32(f[1].Get<uint64>())];
                    found[uint32(f[0].Get<uint64>())].insert(set.begin(), set.end());
                });
                std::unordered_set<uint32> have(lootIds[store].begin(), lootIds[store].end());
                for (auto const& [loot, set] : found)
                    if (have.insert(loot).second)
                        lootIds[store].push_back(loot);
            }
        }

        // ---- places and eras --------------------------------------------------------------------

        uint8 WorldEra(uint32 map, uint32 zone)
        {
            Config const& config = GetConfig();
            if (!config.progressionEnabled || !config.eraLocks)
                return 0;
            if (map == 530)
            {
                switch (zone)
                {
                    case 3430: case 3433: case 3487:  // Eversong Woods, Ghostlands, Silvermoon
                    case 3524: case 3525: case 3557:  // Azuremyst Isle, Bloodmyst Isle, the Exodar
                        return 0;
                    case 4080:                        // Isle of Quel'Danas
                        return IP_SUNWELL;
                    default:
                        return IP_TBC;
                }
            }
            if (map == 571 || map == 609)             // Northrend, Ebon Hold
                return IP_WOTLK;
            Index::Place place;
            if (Index::FindInstanceOnMap(map, place))
                return place.reqState;
            return 0;
        }

        // First spawn of each wanted creature or object, placed in its zone.
        class Spots
        {
        public:
            void WantCreature(uint32 entry) { _creatures.emplace(entry, Spot()); }
            void WantObject(uint32 entry) { _objects.emplace(entry, Spot()); }

            void Resolve()
            {
                for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                    Take(_creatures, data.id, data.mapid, data.posX, data.posY, data.posZ);
                for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                    Take(_objects, data.id, data.mapid, data.posX, data.posY, data.posZ);
                uint32 computed = 0;
                for (auto* set : { &_creatures, &_objects })
                    for (auto& [entry, spot] : *set)
                    {
                        if (!spot.placed)
                            continue;
                        MapEntry const* map = sMapStore.LookupEntry(spot.map);
                        if (!map || map->Instanceable())
                            continue;
                        uint32 area = 0;
                        sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, spot.zone, area, spot.map, spot.x, spot.y, spot.z);
                        if (++computed % 50 == 0)
                            ++World::m_worldLoopCounter;
                    }
            }

            Spot const& Creature(uint32 entry) const { return Lookup(_creatures, entry); }
            Spot const& Object(uint32 entry) const { return Lookup(_objects, entry); }

        private:
            static void Take(std::unordered_map<uint32, Spot>& set, uint32 entry, uint32 map, float x, float y, float z)
            {
                auto itr = set.find(entry);
                if (itr == set.end() || itr->second.placed)
                    return;
                itr->second = { true, map, x, y, z, 0 };
            }

            static Spot const& Lookup(std::unordered_map<uint32, Spot> const& set, uint32 entry)
            {
                static Spot const none;
                auto itr = set.find(entry);
                return itr != set.end() ? itr->second : none;
            }

            std::unordered_map<uint32, Spot> _creatures;
            std::unordered_map<uint32, Spot> _objects;
        };

        // A spot's zone name, or its instance's, or its map's.
        std::string PlaceName(Spot const& spot)
        {
            if (!spot.placed)
                return "";
            if (spot.zone)
                return Index::AreaName(spot.zone);
            Index::Place place;
            if (Index::FindInstanceOnMap(spot.map, place))
                return place.name;
            return Index::MapName(spot.map);
        }

        uint8 SpotEra(Spot const& spot)
        {
            return spot.placed ? WorldEra(spot.map, spot.zone) : 0;
        }

        // Highest mod-individual-progression state quest a condition group asks for, per
        // (source group, source entry).
        std::map<std::pair<uint32, uint32>, uint8> ReadEraConditions(uint32 sourceType)
        {
            std::map<std::pair<uint32, uint32>, uint8> out;
            if (!GetConfig().progressionEnabled || !GetConfig().eraLocks)
                return out;
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(SourceGroup AS UNSIGNED), CAST(SourceEntry AS SIGNED), "
                "CAST(ConditionValue1 AS UNSIGNED) FROM conditions "
                "WHERE SourceTypeOrReferenceId = {} AND ConditionTypeOrReference = {} AND NegativeCondition = 0 "
                "AND ConditionValue1 BETWEEN {} AND {}", sourceType, COND_QUESTREWARDED, IP_QUEST_BASE + 1, IP_QUEST_BASE + IP_STATE_MAX))
                do
                {
                    Field* f = result->Fetch();
                    auto key = std::make_pair(uint32(f[0].Get<uint64>()), uint32(f[1].Get<int64>()));
                    uint8 state = uint8(f[2].Get<uint64>() - IP_QUEST_BASE);
                    out[key] = std::max(out[key], state);
                } while (result->NextRow());
            return out;
        }

        // ---- sources ----------------------------------------------------------------------------

        std::string CreatureName(uint32 entry)
        {
            CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry);
            return t ? t->Name : std::string();
        }

        void AddSource(Ref const& ref, Source source)
        {
            Get(ref)->list.push_back(std::move(source));
        }

        void AddSourceForItem(uint32 item, Source const& source)
        {
            auto itr = sData.byItem.find(item);
            if (itr == sData.byItem.end())
                return;
            for (Ref const& ref : itr->second)
                AddSource(ref, source);
        }

        void BuildVendors(Spots& spots, std::vector<std::pair<uint32, Source>>& pending)
        {
            std::vector<uint32> items;
            for (auto const& [item, refs] : sData.byItem)
                items.push_back(item);
            std::sort(items.begin(), items.end());

            // Vendor lists other vendors reference (npc_vendor rows with a negative item).
            std::unordered_map<uint32, std::vector<uint32>> users;
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(entry AS UNSIGNED), CAST(-item AS UNSIGNED) FROM npc_vendor WHERE item < 0"))
                do
                {
                    Field* f = result->Fetch();
                    users[uint32(f[1].Get<uint64>())].push_back(uint32(f[0].Get<uint64>()));
                } while (result->NextRow());

            auto eras = ReadEraConditions(COND_SOURCE_NPC_VENDOR);

            struct Sale { uint32 vendor; uint32 item; uint32 extendedCost; };
            std::vector<Sale> sales;
            QueryIn("SELECT CAST(entry AS UNSIGNED), CAST(item AS UNSIGNED), CAST(ExtendedCost AS UNSIGNED) FROM npc_vendor "
                "WHERE item IN ({})", items, [&](Field* f)
            {
                uint32 list = uint32(f[0].Get<uint64>());
                uint32 item = uint32(f[1].Get<uint64>());
                uint32 cost = uint32(f[2].Get<uint64>());
                std::vector<uint32> vendors = { list };
                std::unordered_set<uint32> seen = { list };
                for (std::size_t i = 0; i < vendors.size() && i < 64; ++i)
                    if (auto itr = users.find(vendors[i]); itr != users.end())
                        for (uint32 v : itr->second)
                            if (seen.insert(v).second)
                                vendors.push_back(v);
                for (uint32 v : vendors)
                    if (sObjectMgr->GetCreatureTemplate(v))
                        sales.push_back({ v, item, cost });
            });

            // Vendors only spawned by a world event.
            std::unordered_set<uint32> vendorSet;
            for (Sale const& s : sales)
                vendorSet.insert(s.vendor);
            std::vector<uint32> vendors = Sorted(vendorSet);
            std::unordered_map<uint32, std::string> eventNames;
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(eventEntry AS UNSIGNED), description FROM game_event"))
                do
                {
                    Field* f = result->Fetch();
                    eventNames[uint32(f[0].Get<uint64>())] = f[1].Get<std::string>();
                } while (result->NextRow());
            std::unordered_map<uint32, std::pair<uint32, uint32>> spawnCounts; // vendor -> (spawns, event spawns)
            std::unordered_map<uint32, uint32> vendorEvent;
            QueryIn("SELECT CAST(c.id AS UNSIGNED), COUNT(*), CAST(COALESCE(SUM(e.eventEntry > 0), 0) AS UNSIGNED), "
                "CAST(COALESCE(MAX(GREATEST(e.eventEntry, 0)), 0) AS UNSIGNED) "
                "FROM creature c LEFT JOIN game_event_creature e ON e.guid = c.guid WHERE c.id IN ({}) GROUP BY c.id",
                vendors, [&](Field* f)
            {
                uint64 all = f[1].Get<uint64>();
                uint64 onEvent = f[2].Get<uint64>();
                uint64 eventId = f[3].Get<uint64>();
                if (all > 0 && onEvent >= all && eventId)
                    vendorEvent[uint32(f[0].Get<uint64>())] = uint32(eventId);
            });

            for (Sale const& s : sales)
            {
                spots.WantCreature(s.vendor);
                Source src;
                src.kind = 'V';
                src.id = s.vendor;
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(s.item))
                    src.a = uint32(std::max<int32>(0, proto->BuyPrice));
                if (ItemExtendedCostEntry const* cost = s.extendedCost ? sItemExtendedCostStore.LookupEntry(s.extendedCost) : nullptr)
                {
                    src.b = cost->reqhonorpoints;
                    src.c = cost->reqarenapoints;
                    src.items = Index::ItemList(cost->reqitem, cost->reqitemcount, MAX_ITEM_EXTENDED_COST_REQUIREMENTS);
                }
                src.name = CreatureName(s.vendor);
                if (auto itr = vendorEvent.find(s.vendor); itr != vendorEvent.end())
                    if (auto name = eventNames.find(itr->second); name != eventNames.end())
                        src.extra = name->second;
                if (auto itr = eras.find({ s.vendor, s.item }); itr != eras.end())
                    src.reqState = itr->second;
                src.sort = -float(src.a);
                pending.emplace_back(s.item, std::move(src));
            }
        }

        // Every catalog item a loot id's table gives, with its chance.
        template <class Fn>
        void ForEachDrop(Loot::Store store, uint32 lootId, Fn&& fn)
        {
            auto const& wanted = sData.lootItems[std::size_t(store)];
            auto itr = wanted.find(lootId);
            if (itr == wanted.end())
                return;
            std::unordered_set<uint32> left = itr->second;
            for (Loot::Drop const& d : Loot::For(store, lootId))
                if (left.erase(d.item))
                    fn(d.item, d.chance);
            // Reached only through a world drop table, which the loot leaves out.
            for (uint32 item : left)
                fn(item, -1.0f);
        }

        void BuildDrops(Spots& spots, std::vector<std::pair<uint32, Source>>& pending, std::unordered_set<uint32>& worldDrops)
        {
            auto chanceOf = [](float chance) { return uint32(std::lround(std::max(0.0f, chance) * 100.0f)); };

            // Creatures and pickpocketing, by loot id.
            std::unordered_map<uint32, std::vector<uint32>> byLoot, byPocket;
            for (auto const& [entry, t] : *sObjectMgr->GetCreatureTemplates())
            {
                if (t.lootid && sData.lootItems[std::size_t(Loot::Store::Creature)].count(t.lootid))
                    byLoot[t.lootid].push_back(entry);
                if (t.pickpocketLootId && sData.lootItems[std::size_t(Loot::Store::Pickpocket)].count(t.pickpocketLootId))
                    byPocket[t.pickpocketLootId].push_back(entry);
            }
            for (auto const& [loot, entries] : byLoot)
                ForEachDrop(Loot::Store::Creature, loot, [&](uint32 item, float chance)
                {
                    if (chance < 0.0f)
                    {
                        worldDrops.insert(item);
                        return;
                    }
                    for (uint32 entry : entries)
                    {
                        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry);
                        Source src;
                        src.kind = 'D';
                        src.id = entry;
                        src.a = chanceOf(chance);
                        src.name = t->Name;
                        src.c = 0;
                        src.d = (t->rank == CREATURE_ELITE_RARE || t->rank == CREATURE_ELITE_RAREELITE) ? 1 : 0;
                        Index::Place place;
                        if (Index::FindBossOf(entry, place))
                        {
                            src.b = place.instance;
                            src.c = place.boss;
                            src.where = place.name;
                            src.reqState = place.reqState;
                        }
                        spots.WantCreature(entry);
                        src.sort = chance;
                        pending.emplace_back(item, std::move(src));
                    }
                });
            for (auto const& [loot, entries] : byPocket)
                ForEachDrop(Loot::Store::Pickpocket, loot, [&](uint32 item, float chance)
                {
                    if (chance < 0.0f)
                        return;
                    for (uint32 entry : entries)
                    {
                        Source src;
                        src.kind = 'P';
                        src.id = entry;
                        src.a = chanceOf(chance);
                        src.name = CreatureName(entry);
                        spots.WantCreature(entry);
                        src.sort = chance;
                        pending.emplace_back(item, std::move(src));
                    }
                });

            // Chests and other objects.
            std::unordered_map<uint32, std::vector<uint32>> byObjectLoot;
            for (auto const& [entry, t] : *sObjectMgr->GetGameObjectTemplates())
                if (uint32 loot = t.GetLootId(); loot && sData.lootItems[std::size_t(Loot::Store::GameObject)].count(loot))
                    byObjectLoot[loot].push_back(entry);
            for (auto const& [loot, entries] : byObjectLoot)
                ForEachDrop(Loot::Store::GameObject, loot, [&](uint32 item, float chance)
                {
                    if (chance < 0.0f)
                        return;
                    for (uint32 entry : entries)
                    {
                        GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(entry);
                        Source src;
                        src.kind = 'G';
                        src.id = entry;
                        src.a = chanceOf(chance);
                        src.name = t->name;
                        Index::Place place;
                        if (Index::FindBossOfChest(entry, place))
                        {
                            src.b = place.instance;
                            src.c = place.boss;
                            src.where = place.name;
                            src.reqState = place.reqState;
                        }
                        spots.WantObject(entry);
                        src.sort = chance;
                        pending.emplace_back(item, std::move(src));
                    }
                });

            // Containers (the loot id is the container item) and fishing (the loot id is the zone).
            for (auto const& [loot, set] : sData.lootItems[std::size_t(Loot::Store::Item)])
                ForEachDrop(Loot::Store::Item, loot, [&](uint32 item, float chance)
                {
                    if (chance < 0.0f || !sObjectMgr->GetItemTemplate(loot))
                        return;
                    Source src;
                    src.kind = 'I';
                    src.id = loot;
                    src.a = chanceOf(chance);
                    src.sort = chance;
                    pending.emplace_back(item, std::move(src));
                });
            for (auto const& [loot, set] : sData.lootItems[std::size_t(Loot::Store::Fishing)])
                ForEachDrop(Loot::Store::Fishing, loot, [&](uint32 item, float chance)
                {
                    if (chance < 0.0f)
                        return;
                    AreaTableEntry const* area = sAreaTableStore.LookupEntry(loot);
                    Source src;
                    src.kind = 'F';
                    src.id = loot;
                    src.a = chanceOf(chance);
                    src.name = Index::AreaName(loot);
                    if (area)
                        src.reqState = WorldEra(area->mapid, area->zone ? area->zone : area->ID);
                    src.sort = chance;
                    pending.emplace_back(item, std::move(src));
                });
        }

        void BuildQuests(Spots& spots)
        {
            std::unordered_set<uint32> questSet;
            for (auto const& [item, quests] : sQuestsByItem)
                if (sData.byItem.count(item))
                    questSet.insert(quests.begin(), quests.end());
            for (auto const& [spell, quests] : sQuestsBySpell)
                if (sData.bySpell.count(spell))
                    questSet.insert(quests.begin(), quests.end());
            std::vector<uint32> quests = Sorted(questSet);

            // Who starts each quest: a creature or an object, the first found.
            std::unordered_map<uint32, std::pair<char, uint32>> giver;
            QueryIn("SELECT CAST(quest AS UNSIGNED), CAST(id AS UNSIGNED) FROM creature_queststarter WHERE quest IN ({})", quests,
                [&](Field* f) { giver.emplace(uint32(f[0].Get<uint64>()), std::make_pair('C', uint32(f[1].Get<uint64>()))); });
            QueryIn("SELECT CAST(quest AS UNSIGNED), CAST(id AS UNSIGNED) FROM gameobject_queststarter WHERE quest IN ({})", quests,
                [&](Field* f) { giver.emplace(uint32(f[0].Get<uint64>()), std::make_pair('G', uint32(f[1].Get<uint64>()))); });
            for (auto const& [quest, g] : giver)
            {
                if (g.first == 'C')
                    spots.WantCreature(g.second);
                else
                    spots.WantObject(g.second);
            }
            spots.Resolve();

            auto eras = ReadEraConditions(COND_SOURCE_QUEST_AVAILABLE);

            auto make = [&](uint32 questId) -> std::optional<Source>
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(questId);
                if (!q)
                    return std::nullopt;
                Source src;
                src.kind = 'Q';
                src.id = questId;
                src.a = uint32(std::max<int32>(0, q->GetQuestLevel()));
                uint32 races = q->GetAllowableRaces() & (RACEMASK_ALLIANCE | RACEMASK_HORDE);
                if (races && !(races & RACEMASK_HORDE))
                    src.b = 1;
                else if (races && !(races & RACEMASK_ALLIANCE))
                    src.b = 2;
                src.name = q->GetTitle();
                if (auto itr = giver.find(questId); itr != giver.end())
                {
                    if (itr->second.first == 'C')
                    {
                        src.extra = CreatureName(itr->second.second);
                        src.spot = spots.Creature(itr->second.second);
                    }
                    else if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(itr->second.second))
                    {
                        src.extra = t->name;
                        src.spot = spots.Object(itr->second.second);
                    }
                }
                src.where = PlaceName(src.spot);
                src.reqState = SpotEra(src.spot);
                if (auto itr = eras.find({ 0u, questId }); itr != eras.end())
                    src.reqState = std::max(src.reqState, itr->second);
                return src;
            };

            for (auto const& [item, qs] : sQuestsByItem)
                if (sData.byItem.count(item))
                    for (uint32 questId : qs)
                        if (auto src = make(questId))
                            AddSourceForItem(item, *src);
            for (auto const& [spell, qs] : sQuestsBySpell)
                if (auto itr = sData.bySpell.find(spell); itr != sData.bySpell.end())
                    for (uint32 questId : qs)
                        if (auto src = make(questId))
                            for (Ref const& ref : itr->second)
                                AddSource(ref, *src);
        }

        void BuildAchievements()
        {
            std::vector<uint32> items;
            for (auto const& [item, refs] : sData.byItem)
                items.push_back(item);
            std::sort(items.begin(), items.end());
            QueryIn("SELECT CAST(ID AS UNSIGNED), CAST(ItemID AS UNSIGNED) FROM achievement_reward WHERE ItemID IN ({})", items,
                [&](Field* f)
            {
                Source src;
                src.kind = 'A';
                src.id = uint32(f[0].Get<uint64>());
                AddSourceForItem(uint32(f[1].Get<uint64>()), src);
            });
        }

        void BuildCrafted()
        {
            for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
            {
                SkillLineAbilityEntry const* sla = sSkillLineAbilityStore.LookupEntry(i);
                if (!sla)
                    continue;
                SkillLineEntry const* skill = sSkillLineStore.LookupEntry(sla->SkillLine);
                if (!skill || (skill->categoryId != SKILL_CATEGORY_PROFESSION && skill->categoryId != SKILL_CATEGORY_SECONDARY))
                    continue;
                SpellInfo const* spell = sSpellMgr->GetSpellInfo(sla->Spell);
                if (!spell)
                    continue;
                for (SpellEffectInfo const& eff : spell->Effects)
                {
                    if (eff.Effect != SPELL_EFFECT_CREATE_ITEM || !eff.ItemType || !sData.byItem.count(eff.ItemType))
                        continue;
                    Source src;
                    src.kind = 'C';
                    src.id = sla->Spell;
                    src.a = sla->SkillLine;
                    src.b = std::max(sla->MinSkillLineRank, sla->TrivialSkillLineRankLow);
                    char const* name = skill->name[sWorld->GetDefaultDbcLocale()];
                    src.name = name && *name ? name : skill->name[LOCALE_enUS];
                    // A recipe past 300 skill is Burning Crusade's, past 375 Wrath's.
                    if (GetConfig().progressionEnabled && GetConfig().eraLocks)
                        src.reqState = src.b > 375 ? IP_WOTLK : src.b > 300 ? IP_TBC : 0;
                    AddSourceForItem(eff.ItemType, src);
                }
            }
        }

        void BuildTrainers()
        {
            // Trainer id -> its creatures.
            std::unordered_map<uint32, std::vector<uint32>> creatures;
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(CreatureId AS UNSIGNED), CAST(TrainerId AS UNSIGNED) "
                "FROM creature_default_trainer"))
                do
                {
                    Field* f = result->Fetch();
                    creatures[uint32(f[1].Get<uint64>())].push_back(uint32(f[0].Get<uint64>()));
                } while (result->NextRow());

            Spots spots;
            std::vector<std::pair<Ref, Source>> found;
            for (auto const& [spell, refs] : sData.bySpell)
            {
                auto itr = sTrainerRows.find(spell);
                if (itr == sTrainerRows.end())
                    continue;
                // One source per class: the cheapest row, the first trainer that teaches it.
                std::map<uint32, std::pair<TrainerRow, std::vector<uint32>>> byClass;
                for (TrainerRow const& row : itr->second)
                {
                    auto who = creatures.find(row.trainer);
                    if (who == creatures.end())
                        continue;
                    auto [slot, inserted] = byClass.try_emplace(row.cls, row, std::vector<uint32>());
                    if (!inserted && row.cost < slot->second.first.cost)
                        slot->second.first = row;
                    slot->second.second.insert(slot->second.second.end(), who->second.begin(), who->second.end());
                }
                for (auto const& [cls, rowAndTrainers] : byClass)
                {
                    auto const& [row, trainers] = rowAndTrainers;
                    Source src;
                    src.kind = 'T';
                    src.id = row.trainer;
                    src.a = cls;
                    src.b = row.level;
                    src.c = row.cost;
                    src.d = uint32(trainers.size());
                    src.name = CreatureName(trainers.front());
                    spots.WantCreature(trainers.front());
                    src.at = trainers.front();
                    for (Ref const& ref : refs)
                    {
                        Entry* e = Get(ref);
                        if (cls)
                        {
                            e->flags |= ENTRY_CLASS;
                            e->classMask |= 1u << (cls - 1);
                        }
                        found.emplace_back(ref, src);
                    }
                }
            }
            spots.Resolve();
            for (auto& [ref, src] : found)
            {
                src.spot = spots.Creature(src.at);
                src.where = PlaceName(src.spot);
                src.reqState = SpotEra(src.spot);
                AddSource(ref, src);
            }
        }

        void BuildNotes()
        {
            if (QueryResult result = WorldDatabase.Query("SELECT Kind, ID, Note FROM mod_forever_journal_collection_note ORDER BY Kind, ID, Sort"))
                do
                {
                    Field* f = result->Fetch();
                    std::string kindText = f[0].Get<std::string>();
                    uint8 kind = kindText == "P" ? KIND_PET : kindText == "T" ? KIND_TOY : KIND_MOUNT;
                    if (Entry* e = Find(kind, f[1].Get<uint32>()))
                    {
                        Source src;
                        src.kind = 'N';
                        src.name = f[2].Get<std::string>();
                        e->list.push_back(std::move(src));
                    }
                } while (result->NextRow());
        }

        uint32 BitOf(char kind)
        {
            switch (kind)
            {
                case 'V': return SRC_VENDOR;
                case 'D': case 'G': case 'P': case 'R': return SRC_DROP;
                case 'Q': return SRC_QUEST;
                case 'A': return SRC_ACHIEVEMENT;
                case 'C': return SRC_CRAFTED;
                case 'T': return SRC_TRAINER;
                case 'I': return SRC_CONTAINER;
                case 'F': return SRC_FISHING;
                case 'N': return SRC_NOTE;
                default: return 0;
            }
        }

        // Sorts an entry's sources (open ones first, then by kind and chance or price), keeps
        // MAX_PER_KIND of each kind and works out its source bits and era.
        void Finish(Entry& e)
        {
            static std::string const ORDER = "NVQATCDGPIFR";
            std::stable_sort(e.list.begin(), e.list.end(), [](Source const& a, Source const& b)
            {
                std::size_t ka = ORDER.find(a.kind);
                std::size_t kb = ORDER.find(b.kind);
                if (ka != kb)
                    return ka < kb;
                if (a.reqState != b.reqState)
                    return a.reqState < b.reqState;
                return a.sort > b.sort;
            });
            // The same creature or vendor can come twice (two teaching items): keep the first.
            std::vector<Source> kept;
            std::map<char, uint32> perKind;
            std::set<std::pair<char, uint32>> seen;
            for (Source& s : e.list)
            {
                if (s.kind != 'N' && s.kind != 'R' && !seen.insert({ s.kind, s.id }).second)
                    continue;
                if (++perKind[s.kind] > MAX_PER_KIND)
                {
                    ++e.more[s.kind];
                    continue;
                }
                kept.push_back(std::move(s));
            }
            e.list = std::move(kept);

            // Achievements, containers and world drops carry no era of their own: they only open an
            // entry when it has no other source to go by.
            auto eraless = [](char kind) { return kind == 'N' || kind == 'A' || kind == 'I' || kind == 'R' || kind == '+'; };
            bool anyDated = std::any_of(e.list.begin(), e.list.end(), [&](Source const& s) { return !eraless(s.kind); });
            e.sources = 0;
            uint8 era = 255;
            for (Source const& s : e.list)
            {
                e.sources |= BitOf(s.kind);
                if (s.kind == 'V' && !s.extra.empty())
                    e.sources |= SRC_EVENT;
                if (s.kind != 'N' && (!anyDated || !eraless(s.kind)))
                    era = std::min(era, s.reqState);
            }
            e.reqState = era == 255 ? 0 : era;
            if (e.list.empty())
                e.flags |= ENTRY_UNOBTAINABLE;
        }
    }

    // ---- build ----------------------------------------------------------------------------------

    void Prepare(Loot::LootIds& lootIds)
    {
        sData = Data();
        sTrainerRows.clear();
        sQuestsBySpell.clear();
        sQuestsByItem.clear();

        ReadTrainers();
        ReadQuestRewards();
        BuildCatalog();
        FindLoot(lootIds);
    }

    void Build()
    {
        uint32 start = getMSTime();
        Spots spots;
        std::vector<std::pair<uint32, Source>> pending;
        std::unordered_set<uint32> worldDrops;
        BuildVendors(spots, pending);
        BuildDrops(spots, pending, worldDrops);
        BuildQuests(spots); // resolves the spots of everything wanted so far

        for (auto& [item, src] : pending)
        {
            if (src.kind == 'V' || src.kind == 'D' || src.kind == 'P' || src.kind == 'G')
            {
                src.spot = src.kind == 'G' ? spots.Object(src.id) : spots.Creature(src.id);
                if (src.where.empty())
                    src.where = PlaceName(src.spot);
                // A journal boss's era is its instance's; otherwise the spot's (and the vendor's own).
                // (b is the instance only on D and G rows; on V rows it's the honor price.)
                if ((src.kind != 'D' && src.kind != 'G') || !src.b)
                    src.reqState = std::max(src.reqState, SpotEra(src.spot));
            }
            AddSourceForItem(item, src);
        }
        for (uint32 item : worldDrops)
        {
            Source src;
            src.kind = 'R';
            AddSourceForItem(item, src);
        }
        BuildAchievements();
        BuildCrafted();
        BuildTrainers();
        BuildNotes();

        uint32 h = 2166136261u;
        std::array<std::size_t, KIND_COUNT> shown = {};
        for (uint8 kind = 0; kind < KIND_COUNT; ++kind)
        {
            auto& list = sData.entries[kind];
            for (Entry& e : list)
                Finish(e);
            if (!GetConfig().showUnobtainable)
                list.erase(std::remove_if(list.begin(), list.end(), [](Entry const& e) { return e.list.empty(); }), list.end());
            std::sort(list.begin(), list.end(), [](Entry const& a, Entry const& b) { return a.id < b.id; });
            sData.byId[kind].clear();
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                Entry const& e = list[i];
                sData.byId[kind][e.id] = i;
                h = Fnv(Fnv(Fnv(Fnv(Fnv(h, e.id), e.creature), e.flags), e.sources), e.reqState);
                for (Source const& s : e.list)
                    h = Fnv(Fnv(Fnv(h, uint32(s.kind)), s.id), s.a);
            }
            shown[kind] = list.size();
        }
        sData.byItem.clear();
        sData.bySpell.clear();
        sData.lootItems = {};
        sTrainerRows.clear();
        sQuestsBySpell.clear();
        sQuestsByItem.clear();
        sData.hash = Fnv(h, uint32(GetConfig().showUnobtainable));

        LOG_INFO("server.loading", ">> mod-forever-journal: {} mounts, {} pets and {} toys in {} ms",
            shown[KIND_MOUNT], shown[KIND_PET], shown[KIND_TOY], GetMSTimeDiffToNow(start));
    }

    uint32 Hash(uint32 seed)
    {
        return Fnv(seed, sData.hash);
    }

    // ---- requests -------------------------------------------------------------------------------

    namespace
    {
        bool ParseKind(std::string_view text, uint8& kind)
        {
            for (uint8 k = 0; k < KIND_COUNT; ++k)
                if (text.size() == 1 && text[0] == KIND_LETTERS[k])
                {
                    kind = k;
                    return true;
                }
            return false;
        }

        Entry const* FindEntry(std::vector<std::string_view> const& args, std::size_t at, uint8& kind)
        {
            uint32 id = 0;
            if (args.size() <= at + 1 || !ParseKind(args[at], kind) || !ParseUInt(args[at + 1], id))
                return nullptr;
            return Find(kind, id);
        }

        // A note, escaped, then cut to 200 bytes so its row fits one message (escaping can
        // triple a character): never inside a %XX or a UTF-8 character.
        std::string NoteText(std::string const& text)
        {
            constexpr std::size_t LIMIT = 200;
            std::string out = Escape(text, LIMIT);
            if (out.size() <= LIMIT)
                return out;
            std::size_t cut = LIMIT;
            if (out[cut - 1] == '%')
                cut -= 1;
            else if (out[cut - 2] == '%')
                cut -= 2;
            while (cut > 0 && (uint8(out[cut]) & 0xC0) == 0x80)
                --cut;
            out.resize(cut);
            return out;
        }

        std::string Row(Source const& s, std::size_t i)
        {
            uint32 placed = s.spot.placed ? 1 : 0;
            switch (s.kind)
            {
                case 'V':
                    return Acore::StringFormat("V,{},{},{},{},{},{},{},{},{},{},{}", i, s.id, s.a, s.b, s.c, s.items.empty() ? "-" : s.items,
                        s.reqState, placed, Escape(s.name, 40), Escape(s.where, 40), Escape(s.extra, 30));
                case 'D':
                    return Acore::StringFormat("D,{},{},{},{},{},{},{},{},{},{}", i, s.id, s.a, s.b, s.c, s.reqState, placed, s.d,
                        Escape(s.name, 40), Escape(s.where, 40));
                case 'G':
                    return Acore::StringFormat("G,{},{},{},{},{},{},{},{},{}", i, s.id, s.a, s.b, s.c, s.reqState, placed,
                        Escape(s.name, 40), Escape(s.where, 40));
                case 'P':
                    return Acore::StringFormat("P,{},{},{},{},{},{},{}", i, s.id, s.a, s.reqState, placed, Escape(s.name, 40),
                        Escape(s.where, 40));
                case 'F':
                    return Acore::StringFormat("F,{},{},{},{},{}", i, s.id, s.a, s.reqState, Escape(s.name, 40));
                case 'I':
                    return Acore::StringFormat("I,{},{},{}", i, s.id, s.a);
                case 'R':
                    return Acore::StringFormat("R,{}", i);
                case 'Q':
                    return Acore::StringFormat("Q,{},{},{},{},{},{},{},{},{}", i, s.id, s.a, s.reqState, placed, s.b,
                        Escape(s.name, 48), Escape(s.extra, 30), Escape(s.where, 30));
                case 'A':
                    return Acore::StringFormat("A,{},{}", i, s.id);
                case 'C':
                    return Acore::StringFormat("C,{},{},{},{},{}", i, s.id, s.a, s.b, Escape(s.name, 30));
                case 'T':
                    return Acore::StringFormat("T,{},{},{},{},{},{},{},{},{}", i, s.a, s.b, s.c, s.reqState, placed, s.d,
                        Escape(s.name, 40), Escape(s.where, 40));
                case 'N':
                default:
                    return Acore::StringFormat("N,{},{}", i, NoteText(s.name));
            }
        }
    }

    void HandleCatalog(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint8 kind = 0;
        if (args.size() < 3 || !ParseKind(args[2], kind))
        {
            SendError(player, req, "bad");
            return;
        }
        std::vector<std::string> rows;
        rows.reserve(sData.entries[kind].size());
        for (Entry const& e : sData.entries[kind])
            rows.push_back(Acore::StringFormat("{},{},{},{},{},{},{},{}", e.id, e.creature, e.items.empty() ? 0 : e.items.front(),
                e.quality, e.flags, e.classMask, e.sources, e.reqState));
        std::string const letter(1, KIND_LETTERS[kind]);
        Send(player, "CR:" + req + ":" + letter + ":" + std::to_string(rows.size()));
        SendRows(player, "CD:" + req, rows);
        Send(player, "CE:" + req);
    }

    void HandleSources(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint8 kind = 0;
        Entry const* e = FindEntry(args, 2, kind);
        if (!e)
        {
            SendError(player, req, "none");
            return;
        }
        std::vector<std::string> rows;
        for (std::size_t i = 0; i < e->list.size(); ++i)
            rows.push_back(Row(e->list[i], i));
        std::size_t i = e->list.size();
        for (auto const& [letter, more] : e->more)
            rows.push_back(Acore::StringFormat("+,{},{},{}", i++, letter, more));
        Send(player, "WR:" + req + ":" + std::string(1, KIND_LETTERS[kind]) + ":" + std::to_string(e->id) + ":" + std::to_string(rows.size()));
        SendRows(player, "WD:" + req, rows);
        Send(player, "WE:" + req);
    }

    void HandleOwnedToys(Player* player, std::string const& req)
    {
        std::vector<std::string> rows;
        for (Entry const& e : sData.entries[KIND_TOY])
            if (player->HasItemCount(e.id, 1, true))
                rows.push_back(std::to_string(e.id));
        Send(player, "OR:" + req + ":" + std::to_string(rows.size()));
        SendRows(player, "OD:" + req, rows);
        Send(player, "OE:" + req);
    }

    void HandleMapPin(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint8 kind = 0;
        uint32 index = 0;
        Entry const* e = FindEntry(args, 3, kind);
        if (!e || args.size() < 6 || !ParseUInt(args[5], index) || index >= e->list.size())
        {
            SendError(player, req, "none");
            return;
        }
        Source const& s = e->list[index];
        if (!s.spot.placed)
        {
            SendError(player, req, "none");
            return;
        }
        Index::SendPoi(player, s.spot.map, s.spot.x, s.spot.y, s.kind == 'Q' && !s.extra.empty() ? s.extra : s.name);
        Send(player, "M:" + req + ":ok:" + Escape(Index::WhereText(player, s.spot.map, s.spot.x, s.spot.y, s.spot.zone), 120));
    }
}
