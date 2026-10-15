/*
 * mod-forever-journal: loot tables and the chance one kill drops each item.
 *
 * The core keeps its loot templates private, so the rows are read from the database once at
 * startup, for the bosses' loot ids (and the collections' creatures, chests, containers, fishing
 * zones and pickpocket tables) and every reference they reach. The chances follow
 * LootTemplate::Process:
 *   - an ungrouped item rolls on its own chance;
 *   - a group drops at most one entry: the explicitly chanced ones take their slice of a 0-100
 *     roll in order, and the rest of the roll is split evenly between the zero-chance entries;
 *   - a reference rolls its chance, then runs the referenced table MaxCount times.
 * Entries of another loot mode are left out before a group rolls; hard-mode-only items are worked
 * out in a second pass and flagged. Repeated runs of a one-group, all-equal table never give the
 * same item twice, as in the core. Other paths to the same item are combined as independent
 * rolls. The realm's drop rates (quality, referenced chance and amount) are applied the way the
 * core applies them; Rate.Drop.Item.GroupAmount is not.
 *
 * A reference shared by many loot tables is a world drop table (the level 30-35 greens and so
 * on); it is left out unless Journal.Loot.ShowWorldDrops is on, as retail's journal does.
 *
 * A token is a quest or junk item handed in for other items: one of it is what a quest that
 * rewards items asks for (the tier 4-6 tokens, Ahn'Qiraj's and Naxxramas' 40 player ones, the Head
 * of Onyxia), or what a vendor's extended cost asks for (the tier 7-10 tokens). Items turned into
 * by more than MAX_TOKEN_RESULTS things are currencies, not tokens.
 */

#include "Journal.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Journal::Loot
{
    namespace
    {
        struct Row
        {
            uint32 item = 0;
            uint32 reference = 0;
            float chance = 0.0f;
            bool quest = false;
            uint16 lootMode = 1;
            uint8 group = 0;
            uint8 minCount = 1;
            uint8 maxCount = 1;
        };

        using Table = std::vector<Row>;

        std::array<std::unordered_map<uint32, Table>, STORE_COUNT> sTables;
        std::unordered_map<uint32, Table> sReference;

        // How many loot tables use each reference, to tell world drop tables apart.
        std::unordered_map<uint32, uint32> sReferenceUsers;

        // (source type, loot id, item) with rows in `conditions`.
        std::unordered_set<uint64> sConditioned;

        std::array<std::unordered_map<uint32, std::vector<Drop>>, STORE_COUNT> sDrops;
        std::vector<Drop> const sNone;

        constexpr std::size_t MAX_TOKEN_RESULTS = 40;

        struct TokenResult
        {
            uint32 item = 0;
            uint32 classes = 0;   // CLASSMASK bits, 0 = any
            uint32 races = 0;     // the quest's races, 0 = any
            char kind = 'Q';      // Q quest, V vendor
        };

        std::unordered_map<uint32, std::vector<TokenResult>> sTokens;

        uint32 PlayableClasses(uint32 mask)
        {
            mask &= CLASSMASK_ALL_PLAYABLE;
            return mask == CLASSMASK_ALL_PLAYABLE ? 0 : mask;
        }

        // Both restrictions at once; 0 means any class.
        uint32 BothClasses(uint32 a, uint32 b)
        {
            return !a ? b : !b ? a : a & b;
        }

        bool IsTokenKind(uint32 item)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            return proto && (proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_MISC);
        }

        void AddTokenResult(uint32 token, uint32 item, uint32 classes, uint32 races, char kind)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!proto || item == token)
                return;
            uint32 own = PlayableClasses(proto->AllowableClass);
            classes = BothClasses(PlayableClasses(classes), own);
            if ((own || classes) && !classes)
                return; // a quest for classes that can't use the item
            std::vector<TokenResult>& list = sTokens[token];
            for (TokenResult& r : list)
                if (r.item == item)
                {
                    // The same item from another quest: for whoever either is for.
                    r.classes = r.classes && classes ? (r.classes | classes) : 0;
                    r.races = r.races && races ? (r.races | races) : 0;
                    return;
                }
            list.push_back({ item, classes, races, kind });
        }

        constexpr uint16 LOOT_MODE_DEFAULT_BIT = 1;
        constexpr uint32 MAX_DEPTH = 6;

        // conditions.SourceTypeOrReferenceId
        constexpr uint32 COND_CREATURE_LOOT = 1;
        constexpr uint32 COND_FISHING_LOOT = 3;
        constexpr uint32 COND_GAMEOBJECT_LOOT = 4;
        constexpr uint32 COND_ITEM_LOOT = 5;
        constexpr uint32 COND_PICKPOCKETING_LOOT = 8;
        constexpr uint32 COND_REFERENCE_LOOT = 10;

        // Per Store: its table and its conditions source type.
        constexpr std::array<char const*, STORE_COUNT> STORE_TABLES = { "creature_loot_template",
            "gameobject_loot_template", "item_loot_template", "fishing_loot_template", "pickpocketing_loot_template" };
        constexpr std::array<uint32, STORE_COUNT> STORE_CONDITIONS = { COND_CREATURE_LOOT, COND_GAMEOBJECT_LOOT,
            COND_ITEM_LOOT, COND_FISHING_LOOT, COND_PICKPOCKETING_LOOT };

        uint64 ConditionKey(uint32 source, uint32 loot, uint32 item)
        {
            return (uint64(source) << 56) ^ (uint64(loot) << 28) ^ uint64(item);
        }

        template <class Fn>
        void QueryByIds(std::string_view table, std::vector<uint32> const& ids, Fn&& onRow)
        {
            for (std::size_t i = 0; i < ids.size(); i += 500)
            {
                std::string list;
                for (std::size_t j = i; j < std::min(ids.size(), i + 500); ++j)
                {
                    if (!list.empty())
                        list += ',';
                    list += std::to_string(ids[j]);
                }
                std::string query = Acore::StringFormat("SELECT CAST(Entry AS UNSIGNED), CAST(Item AS UNSIGNED), "
                    "CAST(Reference AS SIGNED), Chance, QuestRequired, LootMode, GroupId, MinCount, MaxCount "
                    "FROM {} WHERE Entry IN ({})", table, list);
                if (QueryResult result = WorldDatabase.Query(query))
                    do
                    {
                        onRow(result->Fetch());
                    } while (result->NextRow());
            }
        }

        // Reads a table's rows for the ids, and returns the references they use.
        std::vector<uint32> ReadTable(std::string_view name, std::vector<uint32> const& ids,
            std::unordered_map<uint32, Table>& into)
        {
            std::unordered_set<uint32> refs;
            QueryByIds(name, ids, [&](Field* f)
            {
                Row row;
                uint32 entry = uint32(f[0].Get<uint64>());
                row.item = uint32(f[1].Get<uint64>());
                int64 ref = f[2].Get<int64>();
                row.reference = ref > 0 ? uint32(ref) : uint32(-ref);
                row.chance = f[3].Get<float>();
                row.quest = f[4].Get<bool>();
                row.lootMode = f[5].Get<uint16>();
                row.group = f[6].Get<uint8>();
                row.minCount = std::max<uint8>(1, f[7].Get<uint8>());
                row.maxCount = std::max<uint8>(row.minCount, f[8].Get<uint8>());
                // A negative chance is the old quest marker.
                if (row.chance < 0.0f)
                {
                    row.chance = -row.chance;
                    row.quest = true;
                }
                if (row.reference)
                {
                    row.item = 0;
                    refs.insert(row.reference);
                }
                into[entry].push_back(row);
            });
            return std::vector<uint32>(refs.begin(), refs.end());
        }

        void ReadReferenceUsers()
        {
            for (char const* table : { "creature_loot_template", "gameobject_loot_template", "reference_loot_template",
                "item_loot_template", "skinning_loot_template", "pickpocketing_loot_template" })
            {
                if (QueryResult result = WorldDatabase.Query("SELECT CAST(ABS(Reference) AS UNSIGNED), COUNT(DISTINCT Entry) "
                    "FROM {} WHERE Reference <> 0 GROUP BY ABS(Reference)", table))
                    do
                    {
                        Field* f = result->Fetch();
                        sReferenceUsers[uint32(f[0].Get<uint64>())] += uint32(f[1].Get<uint64>());
                    } while (result->NextRow());
            }
        }

        void ReadConditions()
        {
            if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT SourceTypeOrReferenceId, SourceGroup, SourceEntry "
                "FROM conditions WHERE SourceTypeOrReferenceId IN ({}, {}, {}, {}, {}, {})", COND_CREATURE_LOOT,
                COND_FISHING_LOOT, COND_GAMEOBJECT_LOOT, COND_ITEM_LOOT, COND_PICKPOCKETING_LOOT, COND_REFERENCE_LOOT))
                do
                {
                    Field* f = result->Fetch();
                    sConditioned.insert(ConditionKey(uint32(f[0].Get<int32>()), f[1].Get<uint32>(), uint32(f[2].Get<int32>())));
                } while (result->NextRow());
        }

        bool IsWorldDrop(uint32 reference)
        {
            auto itr = sReferenceUsers.find(reference);
            return itr != sReferenceUsers.end() && itr->second > GetConfig().worldDropThreshold;
        }

        float QualityRate(uint32 item)
        {
            static decltype(RATE_DROP_ITEM_POOR) const rates[] = { RATE_DROP_ITEM_POOR, RATE_DROP_ITEM_NORMAL, RATE_DROP_ITEM_UNCOMMON,
                RATE_DROP_ITEM_RARE, RATE_DROP_ITEM_EPIC, RATE_DROP_ITEM_LEGENDARY, RATE_DROP_ITEM_ARTIFACT };
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!proto || proto->Quality >= std::size(rates))
                return 1.0f;
            return sWorld->getRate(rates[proto->Quality]);
        }

        // item -> what one run of a table gives
        struct Odds
        {
            float p = 0.0f;
            uint32 flags = 0;
            uint8 minCount = 1;
            uint8 maxCount = 1;
        };
        using OddsMap = std::unordered_map<uint32, Odds>;

        void Add(OddsMap& into, uint32 item, float p, uint32 flags, uint8 minCount, uint8 maxCount)
        {
            if (p <= 0.0f)
                return;
            float const q = std::min(p, 1.0f);
            auto [itr, inserted] = into.try_emplace(item);
            Odds& o = itr->second;
            if (inserted)
            {
                o = { q, flags, minCount, maxCount };
                return;
            }
            o.p = 1.0f - (1.0f - o.p) * (1.0f - q);
            o.minCount = std::min(o.minCount, minCount);
            o.maxCount = std::max(o.maxCount, maxCount);
            // "Only with the quest" and "hard mode only" hold when every path says so; the others
            // when any does.
            constexpr uint32 EVERY_PATH = LOOT_QUEST | LOOT_HARD_MODE;
            o.flags = (o.flags & flags & EVERY_PATH) | ((o.flags | flags) & ~EVERY_PATH);
        }

        OddsMap Compute(Table const& table, uint32 condSource, uint32 lootId, uint32 depth, bool hardMode);

        // A table that is one group of zero-chance entries (most raid boss tables): repeated runs
        // never pick the same equippable item twice (LootGroupInvalidSelector), so n runs over k
        // entries give each n/k rather than 1-(1-1/k)^n.
        bool IsEqualChanceGroup(Table const& table)
        {
            if (table.empty())
                return false;
            uint8 group = table.front().group;
            for (Row const& row : table)
                if (!group || row.group != group || row.chance > 0.0f || row.reference)
                    return false;
            return true;
        }

        void AddReference(OddsMap& into, Row const& row, float p, uint32 depth, uint32 extraFlags, bool hardMode)
        {
            if (depth >= MAX_DEPTH || p <= 0.0f)
                return;
            bool world = IsWorldDrop(row.reference);
            if (world && !GetConfig().showWorldDrops)
                return;
            auto itr = sReference.find(row.reference);
            if (itr == sReference.end())
                return;
            OddsMap sub = Compute(itr->second, COND_REFERENCE_LOOT, row.reference, depth + 1, hardMode);
            // The core runs the reference uint32(MaxCount * rate) times, which can be none.
            float n = std::floor(float(row.maxCount) * sWorld->getRate(RATE_DROP_ITEM_REFERENCED_AMOUNT));
            if (n < 1.0f)
                return;
            bool noRepeats = IsEqualChanceGroup(itr->second);
            for (auto const& [item, odds] : sub)
            {
                float once = noRepeats ? std::min(1.0f, n * odds.p) : 1.0f - std::pow(1.0f - odds.p, n);
                uint32 flags = odds.flags | extraFlags | (world ? uint32(LOOT_SHARED) : 0u);
                Add(into, item, p * once, flags, odds.minCount, odds.maxCount);
            }
        }

        uint32 RowFlags(Row const& row, uint32 condSource, uint32 lootId)
        {
            uint32 flags = 0;
            if (row.quest)
                flags |= LOOT_QUEST;
            if (!(row.lootMode & LOOT_MODE_DEFAULT_BIT))
                flags |= LOOT_HARD_MODE;
            if (row.item && sConditioned.count(ConditionKey(condSource, lootId, row.item)))
                flags |= LOOT_CONDITION;
            return flags;
        }

        // Entries of another loot mode (Ulduar's hard modes) are left out before rolling, as the core
        // does; with hardMode they're in.
        bool InMode(Row const& row, bool hardMode)
        {
            return hardMode || (row.lootMode & LOOT_MODE_DEFAULT_BIT);
        }

        OddsMap Compute(Table const& table, uint32 condSource, uint32 lootId, uint32 depth, bool hardMode)
        {
            OddsMap out;
            float const refRate = sWorld->getRate(RATE_DROP_ITEM_REFERENCED);

            // ungrouped entries
            for (Row const& row : table)
            {
                if (row.group || !InMode(row, hardMode))
                    continue;
                uint32 flags = RowFlags(row, condSource, lootId);
                if (row.reference)
                {
                    float p = row.chance >= 100.0f ? 1.0f : row.chance * refRate / 100.0f;
                    AddReference(out, row, p, depth, flags & LOOT_HARD_MODE, hardMode);
                }
                else
                {
                    float p = row.chance >= 100.0f ? 1.0f : row.chance * QualityRate(row.item) / 100.0f;
                    Add(out, row.item, p, flags, row.minCount, row.maxCount);
                }
            }

            // groups: one entry each
            std::vector<uint8> groups;
            for (Row const& row : table)
                if (row.group && std::find(groups.begin(), groups.end(), row.group) == groups.end())
                    groups.push_back(row.group);

            for (uint8 group : groups)
            {
                float remaining = 100.0f;
                std::vector<Row const*> equal;
                std::vector<std::pair<Row const*, float>> picks;
                for (Row const& row : table)
                {
                    if (row.group != group || !InMode(row, hardMode))
                        continue;
                    if (row.chance > 0.0f)
                    {
                        float slice = row.chance >= 100.0f ? remaining : std::min(row.chance, remaining);
                        if (slice > 0.0f)
                            picks.emplace_back(&row, slice / 100.0f);
                        remaining -= slice;
                    }
                    else
                        equal.push_back(&row);
                }
                if (!equal.empty() && remaining > 0.0f)
                    for (Row const* row : equal)
                        picks.emplace_back(row, remaining / 100.0f / float(equal.size()));

                for (auto const& [row, p] : picks)
                {
                    uint32 flags = RowFlags(*row, condSource, lootId);
                    if (row->reference)
                        AddReference(out, *row, p, depth, flags & LOOT_HARD_MODE, hardMode);
                    else
                        Add(out, row->item, p, flags, row->minCount, row->maxCount);
                }
            }
            return out;
        }

        // Normal mode's odds, plus what only hard mode drops (flagged).
        OddsMap ComputeBothModes(Table const& table, uint32 condSource, uint32 lootId)
        {
            OddsMap normal = Compute(table, condSource, lootId, 0, false);
            bool anyHard = false;
            std::function<bool(Table const&, uint32)> hasHard = [&](Table const& t, uint32 depth) -> bool
            {
                for (Row const& row : t)
                {
                    if (!(row.lootMode & LOOT_MODE_DEFAULT_BIT))
                        return true;
                    if (row.reference && depth < MAX_DEPTH)
                    {
                        auto itr = sReference.find(row.reference);
                        if (itr != sReference.end() && hasHard(itr->second, depth + 1))
                            return true;
                    }
                }
                return false;
            };
            anyHard = hasHard(table, 0);
            if (!anyHard)
                return normal;
            for (auto& [item, odds] : normal)
                odds.flags &= ~uint32(LOOT_HARD_MODE);
            OddsMap hard = Compute(table, condSource, lootId, 0, true);
            for (auto const& [item, odds] : hard)
                if (!normal.count(item))
                {
                    Odds o = odds;
                    o.flags |= LOOT_HARD_MODE;
                    normal.emplace(item, o);
                }
            return normal;
        }

        std::vector<Drop> ToDrops(OddsMap const& odds, uint32 extraFlags)
        {
            std::vector<Drop> drops;
            drops.reserve(odds.size());
            for (auto const& [item, o] : odds)
            {
                Drop d;
                d.item = item;
                d.chance = std::min(100.0f, o.p * 100.0f);
                d.flags = o.flags | extraFlags;
                d.minCount = o.minCount;
                d.maxCount = o.maxCount;
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item))
                {
                    if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
                        d.flags |= LOOT_BOP;
                }
                else
                    continue;
                drops.push_back(d);
            }
            std::sort(drops.begin(), drops.end(), [](Drop const& a, Drop const& b)
            {
                return a.chance != b.chance ? a.chance > b.chance : a.item < b.item;
            });
            return drops;
        }
    }

    void Load(LootIds const& ids)
    {
        for (auto& tables : sTables)
            tables.clear();
        sReference.clear();
        sReferenceUsers.clear();
        sConditioned.clear();
        for (auto& drops : sDrops)
            drops.clear();

        ReadReferenceUsers();
        ReadConditions();

        std::vector<uint32> pending;
        for (std::size_t store = 0; store < STORE_COUNT; ++store)
        {
            std::vector<uint32> refs = ReadTable(STORE_TABLES[store], ids[store], sTables[store]);
            pending.insert(pending.end(), refs.begin(), refs.end());
        }

        // References, level by level. World drop tables are read too when they're shown.
        for (uint32 depth = 0; depth < MAX_DEPTH && !pending.empty(); ++depth)
        {
            std::vector<uint32> wanted;
            for (uint32 ref : pending)
                if (!sReference.count(ref) && (GetConfig().showWorldDrops || !IsWorldDrop(ref)))
                    wanted.push_back(ref);
            std::sort(wanted.begin(), wanted.end());
            wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
            for (uint32 ref : wanted)
                sReference[ref]; // known, even when empty
            pending = ReadTable("reference_loot_template", wanted, sReference);
        }

        for (std::size_t store = 0; store < STORE_COUNT; ++store)
        {
            uint32 extra = store == std::size_t(Store::GameObject) ? uint32(LOOT_CHEST) : 0u;
            for (auto const& [id, table] : sTables[store])
                sDrops[store][id] = ToDrops(ComputeBothModes(table, STORE_CONDITIONS[store], id), extra);
        }

        LOG_INFO("server.loading", ">> mod-forever-journal: {} creature, {} chest, {} container, {} fishing and {} pickpocket "
            "loot tables, {} references", sDrops[0].size(), sDrops[1].size(), sDrops[2].size(), sDrops[3].size(),
            sDrops[4].size(), sReference.size());
    }

    std::vector<Drop> const& For(Store store, uint32 lootId)
    {
        auto const& map = sDrops[std::size_t(store)];
        auto itr = map.find(lootId);
        return itr != map.end() ? itr->second : sNone;
    }

    bool IsWorldDropTable(uint32 reference)
    {
        return IsWorldDrop(reference);
    }

    void BuildTokens()
    {
        sTokens.clear();

        for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
        {
            std::vector<uint32> rewards;
            for (uint32 item : quest->RewardItemId)
                if (item)
                    rewards.push_back(item);
            for (uint32 item : quest->RewardChoiceItemId)
                if (item)
                    rewards.push_back(item);
            if (rewards.empty())
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                uint32 token = quest->RequiredItemId[i];
                if (!token || quest->RequiredItemCount[i] != 1 || !IsTokenKind(token))
                    continue;
                for (uint32 item : rewards)
                    AddTokenResult(token, item, quest->GetRequiredClasses(), quest->GetAllowableRaces(), 'Q');
            }
        }

        if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT CAST(item AS UNSIGNED), CAST(ExtendedCost AS UNSIGNED) "
            "FROM npc_vendor WHERE item > 0 AND ExtendedCost > 0"))
            do
            {
                Field* f = result->Fetch();
                uint32 item = uint32(f[0].Get<uint64>());
                ItemExtendedCostEntry const* cost = sItemExtendedCostStore.LookupEntry(uint32(f[1].Get<uint64>()));
                if (!cost)
                    continue;
                for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
                    if (cost->reqitem[i] && cost->reqitemcount[i] == 1 && IsTokenKind(cost->reqitem[i]))
                        AddTokenResult(cost->reqitem[i], item, 0, 0, 'V');
            } while (result->NextRow());

        for (auto itr = sTokens.begin(); itr != sTokens.end();)
        {
            if (itr->second.empty() || itr->second.size() > MAX_TOKEN_RESULTS)
                itr = sTokens.erase(itr);
            else
            {
                std::sort(itr->second.begin(), itr->second.end(), [](TokenResult const& a, TokenResult const& b) { return a.item < b.item; });
                ++itr;
            }
        }
        LOG_INFO("server.loading", ">> mod-forever-journal: {} tokens", sTokens.size());
    }

    bool IsToken(uint32 item)
    {
        return sTokens.count(item) != 0;
    }

    uint32 ClassesOf(uint32 item, bool asToken)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        if (!proto)
            return 0;
        if (uint32 own = PlayableClasses(proto->AllowableClass))
            return own;
        if (!asToken)
            return 0;
        auto itr = sTokens.find(item);
        if (itr == sTokens.end())
            return 0;
        uint32 mask = 0;
        for (TokenResult const& r : itr->second)
        {
            if (!r.classes)
                return 0;
            mask |= r.classes;
        }
        return PlayableClasses(mask);
    }

    // KR:<req>:<token>:<items>, then KD rows:
    //   <item>,<classes>,<Q quest/V vendor>,<quality>,<item level>,<name>
    // Quests the player's race can't take are left out.
    void HandleTokens(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint32 token = 0;
        if (args.size() < 3 || !ParseUInt(args[2], token))
        {
            SendError(player, req, "bad");
            return;
        }
        std::vector<std::string> rows;
        if (auto itr = sTokens.find(token); itr != sTokens.end())
            for (TokenResult const& r : itr->second)
            {
                if (r.races && !(r.races & player->getRaceMask()))
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(r.item);
                if (!proto)
                    continue;
                rows.push_back(Acore::StringFormat("{},{},{},{},{},{}", r.item, r.classes, r.kind, proto->Quality, proto->ItemLevel,
                    Escape(proto->Name1, 60)));
            }
        Send(player, Acore::StringFormat("KR:{}:{}:{}", req, token, rows.size()));
        SendRows(player, "KD:" + req, rows);
        Send(player, "KE:" + req);
    }

    uint32 Hash(uint32 seed)
    {
        // Order-independent: XOR of each drop's own FNV hash.
        auto mix = [](uint32 h, uint32 v)
        {
            for (int i = 0; i < 4; ++i)
            {
                h ^= (v >> (i * 8)) & 0xFF;
                h *= 16777619u;
            }
            return h;
        };
        uint32 acc = 0;
        for (std::size_t store = 0; store < STORE_COUNT; ++store)
            for (auto const& [id, drops] : sDrops[store])
                for (Drop const& d : drops)
                    acc ^= mix(mix(mix(2166136261u + uint32(store), id), d.item), uint32(d.chance * 100.0f) ^ (d.flags << 20));
        // Tokens set flags and classes in the loot rows.
        for (auto const& [token, results] : sTokens)
            for (TokenResult const& r : results)
                acc ^= mix(mix(mix(2166136261u, token), r.item), r.classes);
        return mix(seed, acc);
    }
}
