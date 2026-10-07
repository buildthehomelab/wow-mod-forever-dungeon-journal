/*
 * mod-dungeon-journal: the journal's index (instances, wings, difficulty tabs, bosses, quests,
 * entrances) and the request handlers that answer the addon from it.
 *
 * Instances come from LFGDungeons.dbc: one instance per map, with the Dungeon Finder's wings
 * (Scarlet Monastery's Graveyard, Library, Armory and Cathedral) as sections of its boss list.
 * A map whose Dungeon Finder entries are a dungeon and a raid (Lower and Upper Blackrock Spire)
 * becomes two instances. Bosses are the map's DungeonEncounter.dbc entries, in encounter bit
 * order; an encounter belongs to the first wing whose last encounter (instance_encounters'
 * lastEncounterDungeon) comes at or after it.
 *
 * Difficulty tabs ("variants") are the map's difficulties, plus mod_dungeon_journal_variant rows
 * such as mod-individual-progression's 40 player Onyxia and Naxxramas. Each tab is listed under
 * an expansion tier, so 40 player Naxxramas sits with the classic raids and the 10 and 25 player
 * versions with Wrath's.
 */

#include "DungeonJournal.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace DungeonJournal::Index
{
    namespace
    {
        constexpr uint8 KIND_DUNGEON = 0;
        constexpr uint8 KIND_RAID = 1;
        constexpr uint32 LFG_TYPE_DUNGEON_ID = 1;
        constexpr uint32 LFG_TYPE_RAID_ID = 2;
        constexpr uint32 LFG_FLAG_SEASONAL_BIT = 0x4;
        constexpr uint32 RARE_KEY_BASE = 100;
        constexpr uint8 WING_RARES = 255;
        constexpr uint8 DIFFICULTY_ANY = 255;
        constexpr uint32 POI_FLAGS = 99;
        constexpr uint32 POI_ICON_RED_FLAG = 7;
        constexpr uint32 MAX_SUMMONS_SHOWN = 8;

        // mod-individual-progression states (IndividualProgression.h)
        constexpr uint8 IP_BLACKWING_LAIR = 1;   // passed Molten Core
        constexpr uint8 IP_AHN_QIRAJ = 4;        // passed pre-AQ
        constexpr uint8 IP_TBC = 8;
        constexpr uint8 IP_TBC_TIER_1 = 9;
        constexpr uint8 IP_TBC_TIER_2 = 10;
        constexpr uint8 IP_SUNWELL = 12;
        constexpr uint8 IP_WOTLK = 13;
        constexpr uint8 IP_ULDUAR = 14;
        constexpr uint8 IP_CRUSADER = 15;
        constexpr uint8 IP_ICECROWN = 16;
        constexpr uint8 IP_RUBY_SANCTUM = 17;

        struct Chest
        {
            uint8 difficulty = DIFFICULTY_ANY;
            uint32 entry = 0;
            uint8 flags = 0; // 1 = hard mode
        };

        struct Boss
        {
            uint32 key = 0;                 // encounter bit, or RARE_KEY_BASE + n
            uint8 wing = 0;
            std::string name;
            uint8 difficulties = 0;         // map difficulties the encounter has
            bool rare = false;
            std::vector<uint32> creatures;  // base entries, the main one first
            std::vector<Chest> chests;
        };

        struct Wing
        {
            uint32 lfgId = 0;
            std::string name;
            uint8 minLevel = 0;
            uint8 maxLevel = 0;
            uint32 lastBit = 0;
        };

        struct Variant
        {
            uint8 id = 0;                   // 0-3 = map difficulty, 100+ = mod_dungeon_journal_variant
            uint8 difficulty = 0;
            uint8 expansion = 0;
            uint8 maxPlayers = 0;
            uint8 minLevel = 0;
            uint8 reqState = 0;
            std::string label;
            std::unordered_map<uint32, uint32> swaps;
        };

        struct Instance
        {
            uint32 id = 0;
            uint32 map = 0;
            uint8 kind = KIND_DUNGEON;
            uint8 expansion = 0;
            uint8 minLevel = 0;
            uint8 maxLevel = 0;
            uint8 targetLevel = 0;
            uint32 lfgId = 0;
            uint32 lfgType = 0;
            std::string name;
            std::vector<Wing> wings;
            std::vector<Variant> variants;
            std::vector<Boss> bosses;
            std::vector<uint32> quests;
            // entrance on its continent
            int32 entranceMap = -1;
            float entranceX = 0.0f;
            float entranceY = 0.0f;
            uint32 entranceZone = 0;
        };

        struct Giver
        {
            char kind = '-';                // C creature, G gameobject, I item
            uint32 entry = 0;
            std::string name;
            uint32 map = 0;
            float x = 0.0f;
            float y = 0.0f;
            uint32 zone = 0;
            bool placed = false;
        };

        struct SeedExtra
        {
            uint8 kind = 0;
            uint8 difficulty = DIFFICULTY_ANY;
            uint32 entry = 0;
            uint8 flags = 0;
        };

        struct SeedVariant
        {
            uint32 map = 0;
            Variant variant;
            bool requiresIP = true;
            bool hidesDifficulty = true;
        };

        struct SearchItem
        {
            std::string name;   // lower case
            uint32 item = 0;
            uint32 instance = 0;
            uint32 boss = 0;
            uint8 variant = 0;
            uint32 chance = 0;  // x100
            uint8 quality = 0;
        };

        struct Data
        {
            std::vector<Instance> instances;
            std::unordered_map<uint32, std::size_t> byId;
            std::unordered_multimap<uint32, std::size_t> byMap;
            std::unordered_map<uint32, Giver> givers;            // quest -> who starts it
            std::unordered_map<uint32, uint32> baseOf;           // difficulty entry -> base entry
            std::unordered_set<uint32> learnable;                // base entries whose casts are remembered
            std::unordered_set<uint32> journalMaps;
            std::vector<SearchItem> search;
            uint32 stamp = 0;
        };

        Data sData;

        // ---- helpers ------------------------------------------------------------------------------

        char const* DbcString(char const* const (&names)[16])
        {
            char const* name = names[sWorld->GetDefaultDbcLocale()];
            return name && *name ? name : names[LOCALE_enUS];
        }

        char const* DbcString(std::array<char const*, 16> const& names)
        {
            char const* name = names[sWorld->GetDefaultDbcLocale()];
            return name && *name ? name : names[LOCALE_enUS];
        }

        std::string AreaName(uint32 areaId)
        {
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
                return DbcString(area->area_name);
            return "";
        }

        std::string MapName(uint32 mapId)
        {
            if (MapEntry const* map = sMapStore.LookupEntry(mapId))
                return DbcString(map->name);
            return "";
        }

        uint32 Fnv(uint32 h, std::string_view text)
        {
            for (char c : text)
            {
                h ^= uint8(c);
                h *= 16777619u;
            }
            return h;
        }

        uint32 Fnv(uint32 h, uint32 v)
        {
            for (int i = 0; i < 4; ++i)
            {
                h ^= (v >> (i * 8)) & 0xFF;
                h *= 16777619u;
            }
            return h;
        }

        // The creature a difficulty fights: the variant's replacement, then its difficulty entry.
        uint32 SwapOf(Variant const& variant, uint32 entry)
        {
            auto itr = variant.swaps.find(entry);
            return itr != variant.swaps.end() ? itr->second : entry;
        }

        // As Creature::InitEntry picks it: the difficulty's entry, else (in raids) heroic falls back
        // to the normal mode of the same size, then down to the base creature.
        CreatureTemplate const* TemplateFor(uint32 base, uint8 difficulty, bool raid)
        {
            CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(base);
            if (!cinfo)
                return nullptr;
            for (uint8 diff = std::min<uint8>(difficulty, MAX_DIFFICULTY - 1); diff > 0;)
            {
                if (uint32 diffEntry = cinfo->DifficultyEntry[diff - 1])
                    if (CreatureTemplate const* found = sObjectMgr->GetCreatureTemplate(diffEntry))
                        return found;
                if (diff >= RAID_DIFFICULTY_10MAN_HEROIC && raid)
                    diff -= 2;
                else
                    --diff;
            }
            return cinfo;
        }

        bool IsRaidMap(uint32 mapId)
        {
            MapEntry const* map = sMapStore.LookupEntry(mapId);
            return map && map->IsRaid();
        }

        uint32 BaseEntry(uint32 entry)
        {
            auto itr = sData.baseOf.find(entry);
            return itr != sData.baseOf.end() ? itr->second : entry;
        }

        // The mod-individual-progression state that opens a map's difficulty (OnPlayerBeforeTeleport).
        uint8 EraRequirement(uint32 map, uint8 expansion)
        {
            Config const& config = GetConfig();
            if (!config.progressionEnabled || !config.eraLocks)
                return 0;
            switch (map)
            {
                case 469: return IP_BLACKWING_LAIR;                     // Blackwing Lair
                case 309: return config.zulGurubState;                  // Zul'Gurub
                case 509: case 531: return IP_AHN_QIRAJ;                // Ahn'Qiraj
                case 568: return config.zulAmanState;                   // Zul'Aman
                case 548: case 550: return IP_TBC_TIER_1;               // Serpentshrine, Tempest Keep
                case 534: case 564: return IP_TBC_TIER_2;               // Hyjal, Black Temple
                case 580: case 585: return IP_SUNWELL;                  // Sunwell, Magisters' Terrace
                case 603: return IP_ULDUAR;
                case 649: case 650: return IP_CRUSADER;
                case 631: case 632: case 658: case 668: return IP_ICECROWN;
                case 724: return IP_RUBY_SANCTUM;
                default: break;
            }
            if (expansion >= 2)
                return IP_WOTLK;
            if (expansion == 1)
                return IP_TBC;
            return 0;
        }

        std::string DifficultyLabel(uint8 kind, uint8 difficulty, uint32 maxPlayers, bool onlyOne)
        {
            if (kind == KIND_DUNGEON)
                return difficulty == DUNGEON_DIFFICULTY_HEROIC ? "Heroic" : "Normal";
            std::string label = std::to_string(maxPlayers) + " Player";
            if (onlyOne)
                return label;
            if (difficulty >= RAID_DIFFICULTY_10MAN_HEROIC)
                label += " (Heroic)";
            return label;
        }

        // ---- seeds --------------------------------------------------------------------------------

        std::map<std::pair<uint32, uint32>, std::vector<SeedExtra>> ReadExtras()
        {
            std::map<std::pair<uint32, uint32>, std::vector<SeedExtra>> extras;
            if (QueryResult result = WorldDatabase.Query("SELECT MapID, Bit, Kind, Difficulty, Entry, Flags FROM mod_dungeon_journal_boss_extra"))
                do
                {
                    Field* f = result->Fetch();
                    SeedExtra e;
                    e.kind = f[2].Get<uint8>();
                    e.difficulty = f[3].Get<uint8>();
                    e.entry = f[4].Get<uint32>();
                    e.flags = f[5].Get<uint8>();
                    extras[{ f[0].Get<uint32>(), f[1].Get<uint32>() }].push_back(e);
                } while (result->NextRow());
            return extras;
        }

        std::vector<SeedVariant> ReadVariants()
        {
            std::vector<SeedVariant> variants;
            if (QueryResult result = WorldDatabase.Query("SELECT MapID, Variant, Difficulty, Expansion, MaxPlayers, MinLevel, "
                "RequiredState, RequiresIP, HidesDifficulty, Label FROM mod_dungeon_journal_variant ORDER BY MapID, Variant"))
                do
                {
                    Field* f = result->Fetch();
                    SeedVariant v;
                    v.map = f[0].Get<uint32>();
                    v.variant.id = std::max<uint8>(100, f[1].Get<uint8>());
                    v.variant.difficulty = std::min<uint8>(f[2].Get<uint8>(), MAX_DIFFICULTY - 1);
                    v.variant.expansion = std::min<uint8>(f[3].Get<uint8>(), 2);
                    v.variant.maxPlayers = f[4].Get<uint8>();
                    v.variant.minLevel = f[5].Get<uint8>();
                    v.variant.reqState = f[6].Get<uint8>();
                    v.requiresIP = f[7].Get<bool>();
                    v.hidesDifficulty = f[8].Get<bool>();
                    v.variant.label = f[9].Get<std::string>();
                    variants.push_back(std::move(v));
                } while (result->NextRow());

            if (QueryResult result = WorldDatabase.Query("SELECT MapID, Variant, FromEntry, ToEntry FROM mod_dungeon_journal_variant_swap"))
                do
                {
                    Field* f = result->Fetch();
                    for (SeedVariant& v : variants)
                        if (v.map == f[0].Get<uint32>() && v.variant.id == f[1].Get<uint8>())
                            v.variant.swaps[f[2].Get<uint32>()] = f[3].Get<uint32>();
                } while (result->NextRow());
            return variants;
        }

        // ---- instances ----------------------------------------------------------------------------

        struct LfgGroup
        {
            uint32 map = 0;
            uint32 type = 0;
            std::vector<LFGDungeonEntry const*> entries;
        };

        void BuildInstances(std::vector<SeedVariant> const& seedVariants)
        {
            std::map<std::pair<uint32, uint32>, LfgGroup> groups;
            std::map<std::pair<uint32, uint32>, uint8> difficultyLevels; // (map, difficulty) -> min level

            for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
            {
                LFGDungeonEntry const* lfg = sLFGDungeonStore.LookupEntry(i);
                if (!lfg || (lfg->TypeID != LFG_TYPE_DUNGEON_ID && lfg->TypeID != LFG_TYPE_RAID_ID))
                    continue;
                if (lfg->Flags & LFG_FLAG_SEASONAL_BIT)
                    continue;
                MapEntry const* map = sMapStore.LookupEntry(lfg->MapID);
                if (!map || !map->IsDungeon())
                    continue;
                if (lfg->Difficulty > 0)
                {
                    auto key = std::make_pair(lfg->MapID, lfg->Difficulty);
                    uint8& lvl = difficultyLevels[key];
                    lvl = lvl ? std::min<uint8>(lvl, uint8(lfg->MinLevel)) : uint8(lfg->MinLevel);
                    continue;
                }
                LfgGroup& g = groups[{ lfg->MapID, lfg->TypeID }];
                g.map = lfg->MapID;
                g.type = lfg->TypeID;
                g.entries.push_back(lfg);
            }

            // Each map's wings in encounter order: a wing ends with the encounter that completes it.
            std::unordered_map<uint32, uint32> lfgLastBit;
            for (uint32 d = 0; d < MAX_DIFFICULTY; ++d)
                for (auto const& [mapKey, group] : groups)
                    if (DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(group.map, Difficulty(d)))
                        for (DungeonEncounter const* enc : *list)
                            if (enc->lastEncounterDungeon && !lfgLastBit.count(enc->lastEncounterDungeon))
                                lfgLastBit[enc->lastEncounterDungeon] = enc->dbcEntry->encounterIndex;

            for (auto& [key, group] : groups)
            {
                Instance inst;
                inst.map = group.map;
                inst.lfgType = group.type;
                inst.kind = group.type == LFG_TYPE_RAID_ID ? KIND_RAID : KIND_DUNGEON;
                MapEntry const* map = sMapStore.LookupEntry(group.map);
                if (!map)
                    continue;

                std::sort(group.entries.begin(), group.entries.end(), [&](LFGDungeonEntry const* a, LFGDungeonEntry const* b)
                {
                    auto la = lfgLastBit.find(a->ID);
                    auto lb = lfgLastBit.find(b->ID);
                    uint32 ba = la != lfgLastBit.end() ? la->second : 1000;
                    uint32 bb = lb != lfgLastBit.end() ? lb->second : 1000;
                    return ba != bb ? ba < bb : a->ID < b->ID;
                });

                inst.lfgId = group.entries.front()->ID;
                inst.id = inst.lfgId;
                for (LFGDungeonEntry const* lfg : group.entries)
                    inst.id = std::min(inst.id, lfg->ID);

                bool split = groups.count({ group.map, group.type == LFG_TYPE_RAID_ID ? LFG_TYPE_DUNGEON_ID : LFG_TYPE_RAID_ID }) != 0;
                std::string mapName = MapName(group.map);
                if (group.entries.size() == 1 || split)
                    inst.name = DbcString(group.entries.front()->Name);
                else
                    inst.name = mapName;

                inst.expansion = 0;
                inst.minLevel = 255;
                for (LFGDungeonEntry const* lfg : group.entries)
                {
                    inst.expansion = std::max<uint8>(inst.expansion, uint8(lfg->ExpansionLevel));
                    inst.minLevel = std::min<uint8>(inst.minLevel, uint8(lfg->MinLevel));
                    inst.maxLevel = std::max<uint8>(inst.maxLevel, uint8(lfg->MaxLevel));
                    uint8 target = uint8(lfg->TargetLevel);
                    inst.targetLevel = inst.targetLevel ? std::min(inst.targetLevel, target) : target;
                }

                if (group.entries.size() > 1)
                {
                    for (LFGDungeonEntry const* lfg : group.entries)
                    {
                        Wing w;
                        w.lfgId = lfg->ID;
                        w.name = DbcString(lfg->Name);
                        // "Scarlet Monastery - Library" -> "Library"
                        std::size_t dash = w.name.find(" - ");
                        if (dash != std::string::npos)
                            w.name = w.name.substr(dash + 3);
                        w.minLevel = uint8(lfg->MinLevel);
                        w.maxLevel = uint8(lfg->MaxLevel);
                        auto itr = lfgLastBit.find(lfg->ID);
                        // A wing no encounter closes sorts last, each under its own key.
                        w.lastBit = itr != lfgLastBit.end() ? itr->second : 1000 + uint32(inst.wings.size());
                        inst.wings.push_back(w);
                    }
                }
                else
                {
                    Wing w;
                    w.lfgId = inst.lfgId;
                    auto itr = lfgLastBit.find(inst.lfgId);
                    w.lastBit = itr != lfgLastBit.end() ? itr->second : 1000;
                    inst.wings.push_back(w);
                }

                // Difficulty tabs: the map's own, then the seeded ones.
                std::vector<uint8> difficulties;
                for (uint8 d = 0; d < MAX_DIFFICULTY; ++d)
                    if (d == 0 || GetMapDifficultyData(group.map, Difficulty(d)))
                        difficulties.push_back(d);

                std::vector<SeedVariant const*> mine;
                for (SeedVariant const& sv : seedVariants)
                {
                    if (sv.map != group.map)
                        continue;
                    if (sv.requiresIP && !GetConfig().progressionEnabled)
                        continue;
                    bool swapsExist = std::all_of(sv.variant.swaps.begin(), sv.variant.swaps.end(),
                        [](auto const& s) { return sObjectMgr->GetCreatureTemplate(s.second) != nullptr; });
                    if (!swapsExist)
                        continue;
                    mine.push_back(&sv);
                }

                for (uint8 d : difficulties)
                {
                    bool hidden = std::any_of(mine.begin(), mine.end(), [d](SeedVariant const* sv)
                        { return sv->hidesDifficulty && sv->variant.difficulty == d; });
                    if (hidden)
                        continue;
                    Variant v;
                    v.id = d;
                    v.difficulty = d;
                    v.expansion = inst.expansion;
                    MapDifficulty const* md = GetMapDifficultyData(group.map, Difficulty(d));
                    v.maxPlayers = uint8(md && md->maxPlayers ? md->maxPlayers : map->maxPlayers);
                    auto lvl = difficultyLevels.find({ group.map, d });
                    v.minLevel = lvl != difficultyLevels.end() ? lvl->second : inst.minLevel;
                    v.reqState = EraRequirement(group.map, inst.expansion);
                    inst.variants.push_back(v);
                }
                bool onlyOne = inst.variants.size() == 1;
                for (Variant& v : inst.variants)
                    v.label = DifficultyLabel(inst.kind, v.difficulty, v.maxPlayers, onlyOne);

                for (SeedVariant const* sv : mine)
                {
                    Variant v = sv->variant;
                    if (!v.maxPlayers)
                        v.maxPlayers = uint8(map->maxPlayers);
                    if (!v.minLevel)
                        v.minLevel = inst.minLevel;
                    if (!GetConfig().eraLocks)
                        v.reqState = 0;
                    inst.variants.push_back(v);
                }

                if (inst.variants.empty())
                    continue;
                sData.instances.push_back(std::move(inst));
            }

            std::sort(sData.instances.begin(), sData.instances.end(), [](Instance const& a, Instance const& b)
            {
                return a.id < b.id;
            });
        }

        // ---- bosses -------------------------------------------------------------------------------

        void AddCreature(Boss& boss, uint32 entry, bool front)
        {
            entry = BaseEntry(entry);
            if (!entry || !sObjectMgr->GetCreatureTemplate(entry))
                return;
            auto itr = std::find(boss.creatures.begin(), boss.creatures.end(), entry);
            if (itr != boss.creatures.end())
            {
                if (front && itr != boss.creatures.begin())
                {
                    boss.creatures.erase(itr);
                    boss.creatures.insert(boss.creatures.begin(), entry);
                }
                return;
            }
            if (front)
                boss.creatures.insert(boss.creatures.begin(), entry);
            else
                boss.creatures.push_back(entry);
        }

        void BuildBosses(std::map<std::pair<uint32, uint32>, std::vector<SeedExtra>> const& extras)
        {
            // Encounters per map, merged across difficulties by bit.
            std::unordered_map<uint32, std::map<uint32, Boss>> byMap;
            std::unordered_map<uint32, std::map<uint32, uint32>> wingEnds; // map -> lastBit -> lfg id

            for (Instance const& inst : sData.instances)
                for (Wing const& w : inst.wings)
                    wingEnds[inst.map][w.lastBit] = w.lfgId;

            for (auto const& [mapId, ends] : wingEnds)
            {
                std::map<uint32, Boss>& bosses = byMap[mapId];
                for (uint8 d = 0; d < MAX_DIFFICULTY; ++d)
                {
                    DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(mapId, Difficulty(d));
                    if (!list)
                        continue;
                    for (DungeonEncounter const* enc : *list)
                    {
                        uint32 bit = enc->dbcEntry->encounterIndex;
                        Boss& boss = bosses[bit];
                        boss.key = bit;
                        boss.difficulties |= uint8(1 << d);
                        if (boss.name.empty())
                            boss.name = DbcString(enc->dbcEntry->encounterName);
                        if (enc->creditType == ENCOUNTER_CREDIT_KILL_CREATURE)
                            AddCreature(boss, enc->creditEntry, false);
                    }
                }

                for (auto& [bit, boss] : bosses)
                {
                    auto seeds = extras.find({ mapId, bit });
                    if (seeds != extras.end())
                        for (SeedExtra const& e : seeds->second)
                        {
                            if (e.kind == 1)
                            {
                                if (sObjectMgr->GetGameObjectTemplate(e.entry))
                                    boss.chests.push_back({ e.difficulty, e.entry, e.flags });
                            }
                            else
                                AddCreature(boss, e.entry, e.kind == 2);
                        }
                }
            }

            // Hand each map's encounters to its instances' wings.
            for (Instance& inst : sData.instances)
            {
                auto mapItr = byMap.find(inst.map);
                if (mapItr == byMap.end())
                    continue;
                std::map<uint32, uint32> const& ends = wingEnds[inst.map];
                for (auto const& [bit, boss] : mapItr->second)
                {
                    auto end = ends.lower_bound(bit);
                    uint32 lfg = end != ends.end() ? end->second : ends.rbegin()->second;
                    for (std::size_t w = 0; w < inst.wings.size(); ++w)
                        if (inst.wings[w].lfgId == lfg)
                        {
                            Boss b = boss;
                            b.wing = uint8(w);
                            inst.bosses.push_back(std::move(b));
                            break;
                        }
                }
            }
        }

        // Rare spawns in the instance, after its encounters.
        void BuildRares()
        {
            if (!GetConfig().rares)
                return;

            std::unordered_set<uint32> taken;
            for (Instance const& inst : sData.instances)
                for (Boss const& b : inst.bosses)
                    taken.insert(b.creatures.begin(), b.creatures.end());

            std::unordered_map<uint32, std::vector<uint32>> raresByMap;
            std::unordered_set<uint32> seen;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                if (!sData.journalMaps.count(data.mapid))
                    continue;
                uint32 entry = BaseEntry(data.id);
                CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
                if (!cinfo || (cinfo->rank != CREATURE_ELITE_RARE && cinfo->rank != CREATURE_ELITE_RAREELITE))
                    continue;
                if (taken.count(entry) || !seen.insert(entry).second)
                    continue;
                raresByMap[data.mapid].push_back(entry);
            }

            for (auto& [mapId, entries] : raresByMap)
            {
                std::sort(entries.begin(), entries.end(), [](uint32 a, uint32 b)
                {
                    CreatureTemplate const* ca = sObjectMgr->GetCreatureTemplate(a);
                    CreatureTemplate const* cb = sObjectMgr->GetCreatureTemplate(b);
                    return ca->minlevel != cb->minlevel ? ca->minlevel < cb->minlevel : ca->Name < cb->Name;
                });

                // Split maps (Blackrock Spire): rares go to the dungeon half.
                Instance* target = nullptr;
                auto range = sData.byMap.equal_range(mapId);
                for (auto itr = range.first; itr != range.second; ++itr)
                {
                    Instance& inst = sData.instances[itr->second];
                    if (!target || (inst.kind == KIND_DUNGEON && target->kind != KIND_DUNGEON))
                        target = &inst;
                }
                if (!target)
                    continue;

                uint8 difficulties = 0;
                for (Variant const& v : target->variants)
                    difficulties |= uint8(1 << v.difficulty);
                uint32 n = 0;
                for (uint32 entry : entries)
                {
                    Boss b;
                    b.key = RARE_KEY_BASE + n++;
                    b.wing = WING_RARES;
                    b.rare = true;
                    b.name = sObjectMgr->GetCreatureTemplate(entry)->Name;
                    b.difficulties = difficulties;
                    b.creatures.push_back(entry);
                    target->bosses.push_back(std::move(b));
                }
            }
        }

        // ---- entrances ----------------------------------------------------------------------------

        void PlaceEntrances()
        {
            struct Spot { uint32 map; float x, y, z; };
            std::unordered_map<uint32, Spot> spots;
            if (QueryResult result = WorldDatabase.Query("SELECT t.target_map, a.map, a.x, a.y, a.z FROM areatrigger_teleport t "
                "JOIN areatrigger a ON a.entry = t.ID ORDER BY t.ID"))
                do
                {
                    Field* f = result->Fetch();
                    uint32 target = f[0].Get<uint32>();
                    uint32 from = f[1].Get<uint32>();
                    MapEntry const* fromMap = sMapStore.LookupEntry(from);
                    if (!sData.journalMaps.count(target) || !fromMap || fromMap->Instanceable() || spots.count(target))
                        continue;
                    spots[target] = { from, f[2].Get<float>(), f[3].Get<float>(), f[4].Get<float>() };
                } while (result->NextRow());

            uint32 computed = 0;
            for (Instance& inst : sData.instances)
            {
                auto itr = spots.find(inst.map);
                Spot spot{};
                if (itr != spots.end())
                    spot = itr->second;
                else
                {
                    MapEntry const* map = sMapStore.LookupEntry(inst.map);
                    if (!map || map->entrance_map < 0 || (map->entrance_x == 0.0f && map->entrance_y == 0.0f))
                        continue;
                    spot.map = uint32(map->entrance_map);
                    spot.x = map->entrance_x;
                    spot.y = map->entrance_y;
                    Map* base = sMapMgr->CreateBaseMap(spot.map);
                    spot.z = base ? base->GetHeight(spot.x, spot.y, 1000.0f, true, 2000.0f) : 0.0f;
                }
                inst.entranceMap = int32(spot.map);
                inst.entranceX = spot.x;
                inst.entranceY = spot.y;
                uint32 area = 0;
                sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, inst.entranceZone, area, spot.map, spot.x, spot.y, spot.z);
                // A first start may load terrain here, before the world loop runs: keep the freeze
                // detector (MaxCoreStuckTime) from taking it for a hang.
                if (++computed % 50 == 0)
                    ++World::m_worldLoopCounter;
            }
        }

        // ---- quests -------------------------------------------------------------------------------

        bool IsRaidQuest(Quest const* quest)
        {
            switch (quest->GetType())
            {
                case QUEST_TYPE_RAID:
                case QUEST_TYPE_RAID_10:
                case QUEST_TYPE_RAID_25:
                    return true;
                default:
                    return false;
            }
        }

        void BuildQuests()
        {
            // Where each creature lives: the maps it is spawned on, plus the journal's own creatures
            // (bosses are often summoned, not spawned).
            std::unordered_map<uint32, std::unordered_set<uint32>> creatureMaps;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                creatureMaps[BaseEntry(data.id)].insert(data.mapid);

            // creature -> the instance it's a boss of
            std::unordered_map<uint32, std::size_t> bossInstance;
            for (std::size_t i = 0; i < sData.instances.size(); ++i)
                for (Boss const& b : sData.instances[i].bosses)
                    for (uint32 c : b.creatures)
                    {
                        creatureMaps[c].insert(sData.instances[i].map);
                        bossInstance.emplace(c, i);
                        for (uint32 s : Abilities::Summons(c))
                            creatureMaps[BaseEntry(s)].insert(sData.instances[i].map);
                    }

            auto onlyOn = [&](uint32 creature) -> int64
            {
                auto itr = creatureMaps.find(BaseEntry(creature));
                if (itr == creatureMaps.end() || itr->second.size() != 1)
                    return -1;
                uint32 map = *itr->second.begin();
                return sData.journalMaps.count(map) ? int64(map) : -1;
            };

            // Quest drops: item -> the one journal map all its droppers live on.
            std::unordered_map<uint32, std::vector<uint32>> lootToCreatures;
            for (auto const& [entry, cinfo] : *sObjectMgr->GetCreatureTemplates())
                if (cinfo.lootid)
                    lootToCreatures[cinfo.lootid].push_back(BaseEntry(entry));

            std::unordered_map<uint32, int64> questItemMap;   // -1 = drops in more than one place
            std::unordered_map<uint32, uint32> questItemBoss;  // item -> a boss creature dropping it
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(Entry AS UNSIGNED), CAST(Item AS UNSIGNED) "
                "FROM creature_loot_template WHERE QuestRequired = 1 AND Reference = 0"))
                do
                {
                    Field* f = result->Fetch();
                    uint32 loot = uint32(f[0].Get<uint64>());
                    uint32 item = uint32(f[1].Get<uint64>());
                    auto droppers = lootToCreatures.find(loot);
                    if (droppers == lootToCreatures.end())
                        continue;
                    for (uint32 creature : droppers->second)
                    {
                        int64 map = onlyOn(creature);
                        auto [itr, inserted] = questItemMap.emplace(item, map);
                        if (!inserted && itr->second != map)
                            itr->second = -1;
                        if (bossInstance.count(creature))
                            questItemBoss.emplace(item, creature);
                    }
                } while (result->NextRow());

            // Who starts each quest.
            std::unordered_map<uint32, uint32> creatureStarter, objectStarter, itemStarter;
            for (auto const& [creature, quest] : *sObjectMgr->GetCreatureQuestRelationMap())
                creatureStarter.emplace(quest, creature);
            for (auto const& [object, quest] : *sObjectMgr->GetGOQuestRelationMap())
                objectStarter.emplace(quest, object);
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                if (proto.StartQuest)
                    itemStarter.emplace(proto.StartQuest, entry);

            std::unordered_map<std::size_t, std::vector<Quest const*>> perInstance;
            for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
            {
                if (quest->IsSeasonal() || quest->HasFlag(QUEST_FLAGS_TRACKING))
                    continue;
                bool anyStarter = creatureStarter.count(questId) || objectStarter.count(questId) || itemStarter.count(questId);
                if (!anyStarter)
                    continue;

                int64 map = -1;
                std::size_t instHint = SIZE_MAX;
                if (quest->GetZoneOrSort() > 0)
                    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(uint32(quest->GetZoneOrSort())))
                        if (sData.journalMaps.count(area->mapid))
                            map = area->mapid;

                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                {
                    int32 target = quest->RequiredNpcOrGo[i];
                    if (target <= 0)
                        continue;
                    int64 m = onlyOn(uint32(target));
                    if (m >= 0 && map < 0)
                        map = m;
                    auto b = bossInstance.find(BaseEntry(uint32(target)));
                    if (b != bossInstance.end() && instHint == SIZE_MAX)
                        instHint = b->second;
                }
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                {
                    uint32 item = quest->RequiredItemId[i];
                    if (!item)
                        continue;
                    auto itr = questItemMap.find(item);
                    if (itr != questItemMap.end() && itr->second >= 0 && map < 0)
                        map = itr->second;
                    auto boss = questItemBoss.find(item);
                    if (boss != questItemBoss.end() && instHint == SIZE_MAX)
                        instHint = bossInstance[boss->second];
                }
                if (map < 0)
                    continue;

                // Which instance on the map: the one whose boss it names, a raid quest to the raid,
                // anything else to the dungeon.
                std::size_t chosen = SIZE_MAX;
                if (instHint != SIZE_MAX && sData.instances[instHint].map == uint32(map))
                    chosen = instHint;
                else
                {
                    auto range = sData.byMap.equal_range(uint32(map));
                    for (auto itr = range.first; itr != range.second; ++itr)
                    {
                        Instance const& inst = sData.instances[itr->second];
                        bool wantRaid = IsRaidQuest(quest);
                        if (chosen == SIZE_MAX || (inst.kind == (wantRaid ? KIND_RAID : KIND_DUNGEON)))
                            chosen = itr->second;
                    }
                }
                if (chosen == SIZE_MAX)
                    continue;
                perInstance[chosen].push_back(quest);

                Giver g;
                if (auto c = creatureStarter.find(questId); c != creatureStarter.end())
                {
                    g.kind = 'C';
                    g.entry = c->second;
                    if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(g.entry))
                        g.name = t->Name;
                }
                else if (auto o = objectStarter.find(questId); o != objectStarter.end())
                {
                    g.kind = 'G';
                    g.entry = o->second;
                    if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(g.entry))
                        g.name = t->name;
                }
                else if (auto it = itemStarter.find(questId); it != itemStarter.end())
                {
                    g.kind = 'I';
                    g.entry = it->second;
                    if (ItemTemplate const* t = sObjectMgr->GetItemTemplate(g.entry))
                        g.name = t->Name1;
                }
                sData.givers[questId] = g;
            }

            for (auto& [index, quests] : perInstance)
            {
                std::sort(quests.begin(), quests.end(), [](Quest const* a, Quest const* b)
                {
                    if (a->GetMinLevel() != b->GetMinLevel())
                        return a->GetMinLevel() < b->GetMinLevel();
                    if (a->GetQuestLevel() != b->GetQuestLevel())
                        return a->GetQuestLevel() < b->GetQuestLevel();
                    return a->GetQuestId() < b->GetQuestId();
                });
                Instance& inst = sData.instances[index];
                for (Quest const* q : quests)
                    inst.quests.push_back(q->GetQuestId());
            }

            // Where the givers stand: their first spawn (creatures and objects).
            std::unordered_map<uint32, Giver*> wantCreature, wantObject;
            for (auto& [quest, g] : sData.givers)
            {
                if (g.kind == 'C')
                    wantCreature.emplace(g.entry, &g);
                else if (g.kind == 'G')
                    wantObject.emplace(g.entry, &g);
            }
            std::unordered_map<uint32, std::tuple<uint32, float, float, float>> creatureSpot, objectSpot;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                if (wantCreature.count(data.id) && !creatureSpot.count(data.id))
                    creatureSpot[data.id] = { data.mapid, data.posX, data.posY, data.posZ };
            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                if (wantObject.count(data.id) && !objectSpot.count(data.id))
                    objectSpot[data.id] = { data.mapid, data.posX, data.posY, data.posZ };

            uint32 computed = 0;
            auto place = [&](Giver& g, std::tuple<uint32, float, float, float> const& spot)
            {
                g.map = std::get<0>(spot);
                g.x = std::get<1>(spot);
                g.y = std::get<2>(spot);
                g.placed = true;
                MapEntry const* map = sMapStore.LookupEntry(g.map);
                if (map && !map->Instanceable())
                {
                    uint32 area = 0;
                    sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, g.zone, area, g.map, g.x, g.y, std::get<3>(spot));
                    if (++computed % 50 == 0)
                        ++World::m_worldLoopCounter;
                }
            };
            for (auto& [quest, g] : sData.givers)
            {
                if (g.kind == 'C')
                {
                    if (auto itr = creatureSpot.find(g.entry); itr != creatureSpot.end())
                        place(g, itr->second);
                }
                else if (g.kind == 'G')
                {
                    if (auto itr = objectSpot.find(g.entry); itr != objectSpot.end())
                        place(g, itr->second);
                }
            }
        }

        std::string GiverPlace(Giver const& g)
        {
            if (!g.placed)
                return g.kind == 'I' ? "Drops in the dungeon" : "";
            if (g.zone)
                return AreaName(g.zone);
            return MapName(g.map);
        }

        // ---- lookups for the handlers -------------------------------------------------------------

        Instance const* FindInstance(std::vector<std::string_view> const& args, std::size_t at)
        {
            uint32 id = 0;
            if (args.size() <= at || !ParseUInt(args[at], id))
                return nullptr;
            auto itr = sData.byId.find(id);
            return itr != sData.byId.end() ? &sData.instances[itr->second] : nullptr;
        }

        Boss const* FindBoss(Instance const& inst, std::string_view text)
        {
            uint32 key = 0;
            if (!ParseUInt(text, key))
                return nullptr;
            for (Boss const& b : inst.bosses)
                if (b.key == key)
                    return &b;
            return nullptr;
        }

        Variant const* FindVariant(Instance const& inst, std::string_view text)
        {
            uint32 id = 0;
            if (!ParseUInt(text, id))
                return nullptr;
            for (Variant const& v : inst.variants)
                if (v.id == id)
                    return &v;
            return nullptr;
        }

        // Does the boss show on this tab? Seeded tabs follow difficulty 0's encounter list.
        bool BossOnVariant(Boss const& boss, Variant const& v)
        {
            uint8 d = v.id >= 100 ? 0 : v.difficulty;
            return (boss.difficulties & (1 << d)) != 0 || (v.id >= 100 && (boss.difficulties & (1 << v.difficulty)));
        }

        bool IsListedItem(ItemTemplate const* proto, uint32 flags)
        {
            if (!proto)
                return false;
            if (GetConfig().showGrayLoot || proto->Quality >= ITEM_QUALITY_UNCOMMON)
                return true;
            return (flags & LOOT_QUEST) || proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_RECIPE;
        }

        struct Pick
        {
            Loot::Drop drop;
            uint32 boss = 0;
        };

        // What a boss (or, with all, every boss but the rares) drops on a tab: its creatures' loot
        // and its chests', each item once at its best chance.
        std::vector<Pick> GatherLoot(Instance const& inst, bool all, uint32 bossKey, Variant const& v)
        {
            std::vector<Pick> picks;
            std::unordered_map<uint32, std::size_t> byItem;

            auto take = [&](Loot::Drop const& d, uint32 key)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(d.item);
                if (!IsListedItem(proto, d.flags))
                    return;
                auto itr = byItem.find(d.item);
                if (itr == byItem.end())
                {
                    byItem[d.item] = picks.size();
                    picks.push_back({ d, key });
                }
                else if (d.chance > picks[itr->second].drop.chance)
                    picks[itr->second] = { d, key };
            };

            for (Boss const& b : inst.bosses)
            {
                if (!all && b.key != bossKey)
                    continue;
                if (all && b.rare)
                    continue; // "every boss" leaves the rares out, like retail
                if (!BossOnVariant(b, v))
                    continue;
                for (uint32 c : b.creatures)
                    if (CreatureTemplate const* t = TemplateFor(SwapOf(v, c), v.difficulty, IsRaidMap(inst.map)))
                        if (t->lootid)
                            for (Loot::Drop const& d : Loot::For(Loot::Store::Creature, t->lootid))
                                take(d, b.key);
                for (Chest const& chest : b.chests)
                {
                    if (chest.difficulty != DIFFICULTY_ANY && chest.difficulty != v.difficulty)
                        continue;
                    GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(chest.entry);
                    if (!go)
                        continue;
                    for (Loot::Drop const& drop : Loot::For(Loot::Store::GameObject, go->GetLootId()))
                    {
                        Loot::Drop d = drop;
                        if (chest.flags & 1)
                            d.flags |= LOOT_HARD_MODE;
                        take(d, b.key);
                    }
                }
            }
            return picks;
        }

        std::string Lower(std::string_view text)
        {
            std::string out(text);
            for (char& c : out)
                if (c >= 'A' && c <= 'Z')
                    c = char(c - 'A' + 'a');
            return out;
        }

        // Every item the journal lists, for F: lower-case name -> where it drops.
        void BuildSearch()
        {
            for (std::size_t i = 0; i < sData.instances.size(); ++i)
            {
                Instance const& inst = sData.instances[i];
                std::unordered_set<uint64> seen; // item << 32 | boss
                for (Boss const& b : inst.bosses)
                    for (Variant const& v : inst.variants)
                    {
                        if (!BossOnVariant(b, v))
                            continue;
                        for (Pick const& p : GatherLoot(inst, false, b.key, v))
                        {
                            if (!seen.insert((uint64(p.drop.item) << 32) | b.key).second)
                                continue;
                            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(p.drop.item);
                            sData.search.push_back({ Lower(proto->Name1), p.drop.item, inst.id, b.key, v.id,
                                uint32(std::lround(p.drop.chance * 100.0f)), uint8(proto->Quality) });
                        }
                    }
            }
        }

        void SendPoi(Player* player, uint32 map, float x, float y, std::string const& name)
        {
            if (player->GetMapId() != map)
                return;
            WorldPacket data(SMSG_GOSSIP_POI, 4 + 4 + 4 + 4 + 4 + name.size() + 1);
            data << uint32(POI_FLAGS);
            data << float(x);
            data << float(y);
            data << uint32(POI_ICON_RED_FLAG);
            data << uint32(0);
            data << name;
            player->SendDirectMessage(&data);
        }

        std::string WhereText(Player* player, uint32 map, float x, float y, uint32 zone)
        {
            std::string text = zone ? AreaName(zone) : MapName(map);
            if (zone)
            {
                float zx = x;
                float zy = y;
                Map2ZoneCoordinates(zx, zy, zone);
                if (zx >= 0.0f && zx <= 100.0f && zy >= 0.0f && zy <= 100.0f)
                    text += Acore::StringFormat(" ({:.0f}, {:.0f})", zx, zy);
            }
            if (player->GetMapId() == map)
                text += Acore::StringFormat(", {} yards away", uint32(player->GetExactDist2d(x, y)));
            else if (zone)
                text += " - " + MapName(map);
            return text;
        }

        std::string ItemList(uint32 const* ids, uint32 const* counts, std::size_t n)
        {
            std::string out;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!ids[i] || !sObjectMgr->GetItemTemplate(ids[i]))
                    continue;
                if (!out.empty())
                    out += '/';
                out += std::to_string(ids[i]) + "*" + std::to_string(std::max<uint32>(1, counts[i]));
            }
            return out.empty() ? "-" : out;
        }
    }

    // ---- build ----------------------------------------------------------------------------------

    void Build()
    {
        uint32 start = getMSTime();
        sData = Data();

        for (auto const& [entry, cinfo] : *sObjectMgr->GetCreatureTemplates())
            for (uint32 diff : cinfo.DifficultyEntry)
                if (diff)
                    sData.baseOf.emplace(diff, entry);

        auto extras = ReadExtras();
        auto variants = ReadVariants();

        BuildInstances(variants);
        for (std::size_t i = 0; i < sData.instances.size(); ++i)
        {
            sData.byId[sData.instances[i].id] = i;
            sData.byMap.emplace(sData.instances[i].map, i);
            sData.journalMaps.insert(sData.instances[i].map);
        }

        BuildBosses(extras);
        BuildRares();

        // Abilities and loot for every creature the journal shows, on any tab.
        std::unordered_set<uint32> bases;
        std::unordered_set<uint32> creatureLoot;
        std::unordered_set<uint32> objectLoot;
        for (Instance const& inst : sData.instances)
            for (Boss const& b : inst.bosses)
            {
                for (Variant const& v : inst.variants)
                {
                    for (uint32 c : b.creatures)
                    {
                        uint32 swapped = SwapOf(v, c);
                        bases.insert(swapped);
                        if (CreatureTemplate const* t = TemplateFor(swapped, v.difficulty, IsRaidMap(inst.map)))
                            if (t->lootid)
                                creatureLoot.insert(t->lootid);
                    }
                }
                for (Chest const& chest : b.chests)
                    if (GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(chest.entry))
                        if (uint32 loot = go->GetLootId())
                            objectLoot.insert(loot);
            }

        // Summoned adds are shown (and learned) too.
        Abilities::Load(std::vector<uint32>(bases.begin(), bases.end()));
        std::unordered_set<uint32> adds;
        for (uint32 base : bases)
            for (uint32 s : Abilities::Summons(base))
                if (!bases.count(BaseEntry(s)))
                    adds.insert(BaseEntry(s));
        if (!adds.empty())
        {
            std::vector<uint32> all(bases.begin(), bases.end());
            all.insert(all.end(), adds.begin(), adds.end());
            Abilities::Load(all);
        }
        sData.learnable = bases;
        sData.learnable.insert(adds.begin(), adds.end());

        Loot::Load(std::vector<uint32>(creatureLoot.begin(), creatureLoot.end()),
            std::vector<uint32>(objectLoot.begin(), objectLoot.end()));

        PlaceEntrances();
        BuildQuests();
        BuildSearch();

        uint32 h = 2166136261u;
        h = Fnv(h, PROTOCOL_VERSION);
        for (Instance const& inst : sData.instances)
        {
            h = Fnv(Fnv(Fnv(h, inst.id), inst.map), inst.name);
            for (Variant const& v : inst.variants)
                h = Fnv(Fnv(Fnv(Fnv(h, v.id), v.reqState), v.expansion), v.label);
            for (Boss const& b : inst.bosses)
            {
                h = Fnv(Fnv(Fnv(h, b.key), b.difficulties), b.name);
                for (uint32 c : b.creatures)
                    h = Fnv(h, c);
                for (Chest const& c : b.chests)
                    h = Fnv(h, c.entry);
            }
            for (uint32 q : inst.quests)
                h = Fnv(h, q);
        }
        h = Loot::Hash(h);
        h = Abilities::Hash(h);
        h = Fnv(h, uint32(GetConfig().showGrayLoot) | (uint32(GetConfig().showWorldDrops) << 1) | (uint32(GetConfig().rares) << 2));
        sData.stamp = h ? h : 1;

        std::size_t bosses = 0;
        std::size_t quests = 0;
        for (Instance const& inst : sData.instances)
        {
            bosses += inst.bosses.size();
            quests += inst.quests.size();
        }
        LOG_INFO("server.loading", ">> mod-dungeon-journal: {} instances, {} bosses, {} quests in {} ms",
            sData.instances.size(), bosses, quests, GetMSTimeDiffToNow(start));
    }

    uint32 Stamp()
    {
        return sData.stamp;
    }

    // ---- D: the instance list -------------------------------------------------------------------
    //
    // DR:<req>:<instances>, then DD rows:
    //   I,<id>,<map>,<kind 0 dungeon/1 raid>,<expansion>,<min level>,<max level>,<target level>,<lfg id>,<name>,<entrance zone>
    //   W,<instance>,<wing index>,<lfg id>,<min level>,<max level>,<name>        (instances with wings)
    //   V,<instance>,<variant>,<difficulty>,<expansion>,<players>,<min level>,<era state>,<label>
    void HandleList(Player* player, std::string const& req)
    {
        std::vector<std::string> rows;
        for (Instance const& inst : sData.instances)
        {
            rows.push_back(Acore::StringFormat("I,{},{},{},{},{},{},{},{},{},{}", inst.id, inst.map, inst.kind, inst.expansion,
                inst.minLevel, inst.maxLevel, inst.targetLevel, inst.lfgId, Escape(inst.name, 60),
                Escape(inst.entranceZone ? AreaName(inst.entranceZone) : std::string(), 40)));
            if (inst.wings.size() > 1)
                for (std::size_t w = 0; w < inst.wings.size(); ++w)
                {
                    Wing const& wing = inst.wings[w];
                    rows.push_back(Acore::StringFormat("W,{},{},{},{},{},{}", inst.id, w, wing.lfgId, wing.minLevel, wing.maxLevel,
                        Escape(wing.name, 50)));
                }
            for (Variant const& v : inst.variants)
                rows.push_back(Acore::StringFormat("V,{},{},{},{},{},{},{},{}", inst.id, v.id, v.difficulty, v.expansion,
                    v.maxPlayers, v.minLevel, v.reqState, Escape(v.label, 40)));
        }
        Send(player, "DR:" + req + ":" + std::to_string(sData.instances.size()));
        SendRows(player, "DD:" + req, rows);
        Send(player, "DE:" + req);
    }

    // ---- B: an instance's bosses ----------------------------------------------------------------
    //
    // BR:<req>:<instance>:<bosses>, then BD rows:
    //   <key>,<wing (255 = rares)>,<display creature>,<flags 1 rare/2 chest>,<difficulty mask>,<name>
    void HandleBosses(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        Instance const* inst = FindInstance(args, 2);
        if (!inst)
        {
            SendError(player, req, "bad");
            return;
        }
        std::vector<std::string> rows;
        for (Boss const& b : inst->bosses)
        {
            uint32 flags = (b.rare ? 1 : 0) | (b.chests.empty() ? 0 : 2);
            uint32 display = b.creatures.empty() ? 0 : b.creatures.front();
            rows.push_back(Acore::StringFormat("{},{},{},{},{},{}", b.key, b.wing, display, flags, b.difficulties, Escape(b.name, 60)));
        }
        Send(player, "BR:" + req + ":" + std::to_string(inst->id) + ":" + std::to_string(rows.size()));
        SendRows(player, "BD:" + req, rows);
        Send(player, "BE:" + req);
    }

    // ---- S: the player's status in an instance --------------------------------------------------
    //
    // SR:<req>:<instance>:<era state>, then SD rows:
    //   V,<variant>,<locked by era 0/1>,<era state needed>,<saved 0/1>,<seconds to reset>,<killed bits>
    //   R,<variant>,<L level|I item|Q quest|A achievement>,<id>,<have 0/1>,<name>
    void HandleStatus(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        Instance const* inst = FindInstance(args, 2);
        if (!inst)
        {
            SendError(player, req, "bad");
            return;
        }
        uint8 state = ProgressionState(player);
        time_t now = time(nullptr);
        std::vector<std::string> rows;
        for (Variant const& v : inst->variants)
        {
            bool locked = state < v.reqState;
            uint32 saved = 0;
            uint32 resetIn = 0;
            uint32 killed = 0;
            if (InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(player->GetGUID(), inst->map, Difficulty(v.difficulty)))
                if (InstanceSave* save = bind->save)
                {
                    saved = 1;
                    killed = save->GetCompletedEncounterMask();
                    time_t reset = save->GetResetTime();
                    if (!reset)
                        reset = sInstanceSaveMgr->GetResetTimeFor(inst->map, Difficulty(v.difficulty));
                    resetIn = reset > now ? uint32(reset - now) : 0;
                }
            rows.push_back(Acore::StringFormat("V,{},{},{},{},{},{}", v.id, locked ? 1 : 0, v.reqState, saved, resetIn, killed));

            DungeonProgressionRequirements const* access = sObjectMgr->GetAccessRequirement(inst->map, Difficulty(v.difficulty));
            if (!access)
                continue;
            if (access->levelMin)
                rows.push_back(Acore::StringFormat("R,{},L,{},{},-", v.id, access->levelMin, player->GetLevel() >= access->levelMin ? 1 : 0));
            TeamId team = player->GetTeamId(true);
            for (ProgressionRequirement const* item : access->items)
            {
                if (item->faction != TEAM_NEUTRAL && item->faction != team)
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->id);
                if (!proto)
                    continue;
                bool have = player->HasItemCount(item->id, 1, true);
                rows.push_back(Acore::StringFormat("R,{},I,{},{},{}", v.id, item->id, have ? 1 : 0, Escape(proto->Name1, 50)));
            }
            for (ProgressionRequirement const* quest : access->quests)
            {
                if (quest->faction != TEAM_NEUTRAL && quest->faction != team)
                    continue;
                Quest const* q = sObjectMgr->GetQuestTemplate(quest->id);
                if (!q)
                    continue;
                bool done = player->IsQuestRewarded(quest->id);
                rows.push_back(Acore::StringFormat("R,{},Q,{},{},{}", v.id, quest->id, done ? 1 : 0, Escape(q->GetTitle(), 50)));
            }
            for (ProgressionRequirement const* ach : access->achievements)
            {
                if (ach->faction != TEAM_NEUTRAL && ach->faction != team)
                    continue;
                AchievementEntry const* entry = sAchievementStore.LookupEntry(ach->id);
                if (!entry)
                    continue;
                bool have = player->HasAchieved(ach->id);
                rows.push_back(Acore::StringFormat("R,{},A,{},{},{}", v.id, ach->id, have ? 1 : 0, Escape(DbcString(entry->name), 50)));
            }
        }
        Send(player, "SR:" + req + ":" + std::to_string(inst->id) + ":" + std::to_string(state));
        SendRows(player, "SD:" + req, rows);
        Send(player, "SE:" + req);
    }

    // ---- A: a boss's abilities on a difficulty tab ----------------------------------------------
    //
    // AR:<req>:<instance>:<boss>:<variant>, then AD rows:
    //   U,<creature>,<level>,<rank>,<health>,<flags 1 = summoned add>,<name>
    //   S,<creature>,<spell>,<tags>,<cast ms>
    void HandleAbilities(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        Instance const* inst = FindInstance(args, 2);
        Boss const* boss = inst && args.size() > 3 ? FindBoss(*inst, args[3]) : nullptr;
        Variant const* v = inst && args.size() > 4 ? FindVariant(*inst, args[4]) : nullptr;
        if (!boss || !v)
        {
            SendError(player, req, "bad");
            return;
        }

        std::vector<std::string> rows;
        std::unordered_set<uint32> shown;
        auto unit = [&](uint32 base, bool add) -> bool
        {
            if (!shown.insert(base).second)
                return false;
            CreatureTemplate const* t = TemplateFor(base, v->difficulty, IsRaidMap(inst->map));
            if (!t)
                return false;
            std::vector<Abilities::Ability> abilities = Abilities::For(base, v->difficulty);
            if (add && abilities.empty())
                return false;
            uint32 health = 0;
            if (CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(t->maxlevel, uint8(t->unit_class)))
                health = stats->GenerateHealth(t);
            rows.push_back(Acore::StringFormat("U,{},{},{},{},{},{}", base, t->maxlevel, t->rank, health, add ? 1 : 0, Escape(t->Name, 60)));
            for (Abilities::Ability const& a : abilities)
                rows.push_back(Acore::StringFormat("S,{},{},{},{}", base, a.spell, a.tags, a.castMs));
            return true;
        };

        std::vector<uint32> mains;
        for (uint32 c : boss->creatures)
            mains.push_back(SwapOf(*v, c));
        for (uint32 c : mains)
            unit(c, false);
        uint32 adds = 0;
        for (uint32 c : mains)
            for (uint32 s : Abilities::Summons(c))
                if (adds < MAX_SUMMONS_SHOWN && unit(BaseEntry(s), true))
                    ++adds;

        Send(player, Acore::StringFormat("AR:{}:{}:{}:{}", req, inst->id, boss->key, v->id));
        SendRows(player, "AD:" + req, rows);
        Send(player, "AE:" + req);
    }

    // ---- L: loot on a difficulty tab ------------------------------------------------------------
    //
    // LR:<req>:<instance>:<boss or all>:<variant>:<items>, then LD rows:
    //   <item>,<chance x100>,<flags>,<min>,<max>,<quality>,<class>,<subclass>,<inventory type>,<item level>,<required level>,<boss>,<name>
    void HandleLoot(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        Instance const* inst = FindInstance(args, 2);
        Variant const* v = inst && args.size() > 4 ? FindVariant(*inst, args[4]) : nullptr;
        uint32 bossKey = 0;
        bool all = args.size() > 3 && args[3] == "all";
        if (!inst || !v || args.size() < 4 || (!all && !ParseUInt(args[3], bossKey)))
        {
            SendError(player, req, "bad");
            return;
        }

        std::vector<Pick> picks = GatherLoot(*inst, all, bossKey, *v);

        std::vector<std::string> rows;
        rows.reserve(picks.size());
        for (Pick const& p : picks)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(p.drop.item);
            rows.push_back(Acore::StringFormat("{},{},{},{},{},{},{},{},{},{},{},{},{}", p.drop.item,
                uint32(std::lround(p.drop.chance * 100.0f)), p.drop.flags, p.drop.minCount, p.drop.maxCount, proto->Quality,
                proto->Class, proto->SubClass, proto->InventoryType, proto->ItemLevel, proto->RequiredLevel, p.boss,
                Escape(proto->Name1, 60)));
        }
        Send(player, Acore::StringFormat("LR:{}:{}:{}:{}:{}", req, inst->id, all ? std::string("all") : std::to_string(bossKey), v->id, rows.size()));
        SendRows(player, "LD:" + req, rows);
        Send(player, "LE:" + req);
    }

    // ---- Q: quests for an instance, with the player's status ------------------------------------
    //
    // QR:<req>:<instance>:<quests>, then QD rows:
    //   Q,<quest>,<state>,<level>,<min level>,<flags>,<xp>,<money>,<giver kind C/G/I/->,<giver entry>,<name>,<giver>,<where>
    //   R,<quest>,<rewards id*n/...>,<choices id*n/...>
    // flags: 1 daily, 2 weekly, 4 repeatable, 8 group/elite, 16 heroic, 32 raid
    void HandleQuests(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        Instance const* inst = FindInstance(args, 2);
        if (!inst)
        {
            SendError(player, req, "bad");
            return;
        }

        bool gm = player->IsGameMaster();
        uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        std::vector<std::string> rows;
        uint32 count = 0;
        for (uint32 questId : inst->quests)
        {
            if (count >= GetConfig().maxQuestsPerInstance)
                break;
            Quest const* q = sObjectMgr->GetQuestTemplate(questId);
            if (!q)
                continue;
            if (!gm && (!player->SatisfyQuestRace(q, false) || !player->SatisfyQuestClass(q, false)))
                continue;

            QuestState state;
            QuestStatus status = player->GetQuestStatus(questId);
            if (status == QUEST_STATUS_COMPLETE)
                state = QSTATE_COMPLETE;
            else if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_FAILED)
                state = QSTATE_ACTIVE;
            else if (!q->IsRepeatable() && player->IsQuestRewarded(questId))
                state = QSTATE_DONE;
            else if (!player->SatisfyQuestLevel(q, false))
                state = QSTATE_LOW_LEVEL;
            else if (!player->SatisfyQuestPreviousQuest(q, false) || !player->SatisfyQuestPrevChain(q, false))
                state = QSTATE_PREREQ;
            else if (player->CanTakeQuest(q, false))
                state = QSTATE_AVAILABLE;
            else
                state = QSTATE_LOCKED;

            uint32 flags = 0;
            if (q->IsDaily())
                flags |= 1;
            if (q->IsWeekly())
                flags |= 2;
            if (q->IsRepeatable())
                flags |= 4;
            switch (q->GetType())
            {
                case QUEST_TYPE_ELITE:
                case QUEST_TYPE_DUNGEON:
                    flags |= 8;
                    break;
                case QUEST_TYPE_HEROIC:
                    flags |= 8 | 16;
                    break;
                case QUEST_TYPE_RAID:
                case QUEST_TYPE_RAID_10:
                case QUEST_TYPE_RAID_25:
                    flags |= 32;
                    break;
                default:
                    break;
            }

            uint32 xp = 0;
            int64 money = q->GetRewOrReqMoney(player->GetLevel());
            if (player->GetLevel() < maxLevel)
            {
                xp = player->CalculateQuestRewardXP(q);
                sScriptMgr->OnPlayerQuestComputeXP(player, q, xp);
            }
            else
                money += q->GetRewMoneyMaxLevel();

            Giver giver;
            if (auto g = sData.givers.find(questId); g != sData.givers.end())
                giver = g->second;

            rows.push_back(Acore::StringFormat("Q,{},{},{},{},{},{},{},{},{},{},{},{}", questId, uint32(state),
                q->GetQuestLevel() > 0 ? q->GetQuestLevel() : 0, q->GetMinLevel(), flags, xp, money > 0 ? money : 0,
                giver.kind, giver.entry, Escape(q->GetTitle(), 48), Escape(giver.name, 30), Escape(GiverPlace(giver), 30)));
            rows.push_back(Acore::StringFormat("R,{},{},{}", questId,
                ItemList(q->RewardItemId, q->RewardItemIdCount, QUEST_REWARDS_COUNT),
                ItemList(q->RewardChoiceItemId, q->RewardChoiceItemCount, QUEST_REWARD_CHOICES_COUNT)));
            ++count;
        }
        Send(player, "QR:" + req + ":" + std::to_string(inst->id) + ":" + std::to_string(count));
        SendRows(player, "QD:" + req, rows);
        Send(player, "QE:" + req);
    }

    // ---- QD: a quest's objectives ---------------------------------------------------------------
    //
    // TR:<req>:<quest>, then TD rows:
    //   O,<part of the objectives text>          (in order; joined by the addon)
    //   K,<creature, or -gameobject>,<count>,<name>
    //   I,<item>,<count>,<name>
    //   P,<previous quest>,<done 0/1>,<name>
    void HandleQuestDetail(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint32 questId = 0;
        Quest const* q = args.size() > 2 && ParseUInt(args[2], questId) ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!q)
        {
            SendError(player, req, "bad");
            return;
        }

        std::vector<std::string> rows;
        std::string text = q->GetObjectives();
        // Placeholders the client fills in ($N, $C...) are left as they are; the addon replaces them.
        std::size_t at = 0;
        while (at < text.size() && rows.size() < 12)
        {
            std::size_t take = std::min<std::size_t>(140, text.size() - at);
            while (take > 0 && at + take < text.size() && (uint8(text[at + take]) & 0xC0) == 0x80)
                --take;
            rows.push_back("O," + Escape(text.substr(at, take), 200));
            at += take;
        }
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 target = q->RequiredNpcOrGo[i];
            if (!target)
                continue;
            std::string name;
            if (target > 0)
            {
                if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(uint32(target)))
                    name = t->Name;
            }
            else if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(uint32(-target)))
                name = t->name;
            rows.push_back(Acore::StringFormat("K,{},{},{}", target, q->RequiredNpcOrGoCount[i], Escape(name, 50)));
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (uint32 item = q->RequiredItemId[i])
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item))
                    rows.push_back(Acore::StringFormat("I,{},{},{}", item, q->RequiredItemCount[i], Escape(proto->Name1, 50)));
        if (q->GetPrevQuestId() > 0)
            if (Quest const* prev = sObjectMgr->GetQuestTemplate(uint32(q->GetPrevQuestId())))
                rows.push_back(Acore::StringFormat("P,{},{},{}", prev->GetQuestId(), player->IsQuestRewarded(prev->GetQuestId()) ? 1 : 0,
                    Escape(prev->GetTitle(), 50)));

        Send(player, "TR:" + req + ":" + std::to_string(questId));
        SendRows(player, "TD:" + req, rows);
        Send(player, "TE:" + req);
    }

    // ---- H: the instance the player is in -------------------------------------------------------
    //
    // H:<req>:<instance>:<variant>   or ERR:<req>:none
    void HandleHere(Player* player, std::string const& req)
    {
        Map* map = player->GetMap();
        if (!map || !map->IsDungeon())
        {
            SendError(player, req, "none");
            return;
        }
        uint8 difficulty = uint8(map->GetDifficulty());
        uint8 state = ProgressionState(player);
        // Split maps (Blackrock Spire): the half that matches the player's raid or party.
        bool wantRaid = map->IsRaid() || (player->GetGroup() && player->GetGroup()->isRaidGroup());

        // Scores, best first: the right half of a split map, the map's difficulty, a tab the
        // character's era has opened (40 player Onyxia beside the 10 player one), the later tier.
        Instance const* best = nullptr;
        Variant const* bestVariant = nullptr;
        std::tuple<bool, bool, bool, uint8> bestScore{};
        auto range = sData.byMap.equal_range(map->GetId());
        for (auto itr = range.first; itr != range.second; ++itr)
        {
            Instance const& inst = sData.instances[itr->second];
            for (Variant const& v : inst.variants)
            {
                std::tuple<bool, bool, bool, uint8> score{ (inst.kind == KIND_RAID) == wantRaid, v.difficulty == difficulty,
                    state >= v.reqState, v.expansion };
                if (!bestVariant || score > bestScore)
                {
                    best = &inst;
                    bestVariant = &v;
                    bestScore = score;
                }
            }
        }
        if (!best || !bestVariant)
        {
            SendError(player, req, "none");
            return;
        }
        Send(player, Acore::StringFormat("H:{}:{}:{}", req, best->id, bestVariant->id));
    }

    // ---- M: a map flag on an entrance or a quest giver ------------------------------------------
    //
    // M:<req>:E:<instance>  or  M:<req>:Q:<quest>   -> M:<req>:ok:<where>, or ERR:<req>:none
    void HandleMapPin(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint32 id = 0;
        if (args.size() < 4 || !ParseUInt(args[3], id))
        {
            SendError(player, req, "bad");
            return;
        }

        if (args[2] == "E")
        {
            auto itr = sData.byId.find(id);
            if (itr == sData.byId.end() || sData.instances[itr->second].entranceMap < 0)
            {
                SendError(player, req, "none");
                return;
            }
            Instance const& inst = sData.instances[itr->second];
            uint32 map = uint32(inst.entranceMap);
            SendPoi(player, map, inst.entranceX, inst.entranceY, inst.name);
            Send(player, "M:" + req + ":ok:" + Escape(WhereText(player, map, inst.entranceX, inst.entranceY, inst.entranceZone), 120));
            return;
        }
        if (args[2] == "Q")
        {
            auto itr = sData.givers.find(id);
            if (itr == sData.givers.end() || !itr->second.placed)
            {
                SendError(player, req, "none");
                return;
            }
            Giver const& g = itr->second;
            SendPoi(player, g.map, g.x, g.y, g.name);
            Send(player, "M:" + req + ":ok:" + Escape(WhereText(player, g.map, g.x, g.y, g.zone), 120));
            return;
        }
        SendError(player, req, "bad");
    }

    // ---- F: search by name ----------------------------------------------------------------------
    //
    // FR:<req>:<results>, then FD rows (at most 40, instances and bosses first):
    //   I,<instance>
    //   B,<instance>,<boss>
    //   L,<instance>,<boss>,<variant>,<item>,<chance x100>,<quality>
    void HandleSearch(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        std::string text = args.size() > 2 ? Lower(args[2]) : std::string();
        if (text.size() < 3)
        {
            SendError(player, req, "bad");
            return;
        }
        constexpr std::size_t MAX_RESULTS = 40;
        std::vector<std::string> rows;
        for (Instance const& inst : sData.instances)
        {
            if (rows.size() >= MAX_RESULTS)
                break;
            bool hit = Lower(inst.name).find(text) != std::string::npos;
            for (Wing const& w : inst.wings)
                if (!w.name.empty() && Lower(w.name).find(text) != std::string::npos)
                    hit = true;
            if (hit)
                rows.push_back("I," + std::to_string(inst.id));
        }
        for (Instance const& inst : sData.instances)
            for (Boss const& b : inst.bosses)
            {
                if (rows.size() >= MAX_RESULTS)
                    break;
                bool hit = Lower(b.name).find(text) != std::string::npos;
                for (uint32 c : b.creatures)
                    if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(c))
                        if (Lower(t->Name).find(text) != std::string::npos)
                            hit = true;
                if (hit)
                    rows.push_back(Acore::StringFormat("B,{},{}", inst.id, b.key));
            }
        for (SearchItem const& s : sData.search)
        {
            if (rows.size() >= MAX_RESULTS)
                break;
            if (s.name.find(text) != std::string::npos)
                rows.push_back(Acore::StringFormat("L,{},{},{},{},{},{}", s.instance, s.boss, s.variant, s.item, s.chance, s.quality));
        }
        Send(player, "FR:" + req + ":" + std::to_string(rows.size()));
        SendRows(player, "FD:" + req, rows);
        Send(player, "FE:" + req);
    }

    // ---- learning -------------------------------------------------------------------------------

    void OnCreatureCast(Unit* caster, SpellInfo const* spell)
    {
        Creature* creature = caster->ToCreature();
        if (!creature || !creature->IsInWorld() || !sData.journalMaps.count(creature->GetMapId()))
            return;
        // A boss under a player's control casts what the player picks.
        if (creature->GetCharmerOrOwnerGUID().IsPlayer())
            return;
        uint32 base = BaseEntry(creature->GetEntry());
        if (!sData.learnable.count(base))
            return;
        Abilities::Learn(base, spell->Id);
    }
}
