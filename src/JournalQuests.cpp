/*
 * mod-forever-journal: the Quests tab. Every quest a character can be given on this realm, under
 * the headings the quest log uses (the quest's zone or sort): zones by continent, dungeons, raids,
 * battlegrounds, classes, professions, world events and the rest, with who gives it and where.
 *
 * A quest is listed when something hands it out: a creature or an object (some only while a
 * world event runs), an item, a spell, a SmartAI script, the Dungeon Finder or the quest before it
 * in a chain. Disabled quests and the hidden tracking quests are left out.
 *
 *   G:<req>              -> GR:<req>:<headings>:<the player's heading>, then GD rows:
 *                           G,<group>,<heading>,<quests>,<countable>,<done>,<era state>,<low level>,<high level>,<name>
 *   Z:<req>:<heading>    -> ZR:<req>:<heading>:<quests sent>:<quests>, ZD rows (Q/R, as Index::HandleQuests), ZE
 *   X:<req>:<text>       -> XR:<req>:<quests>, XD rows: C,<quest>,<heading> before its Q/R rows, XE
 *
 * groups: E Eastern Kingdoms, K Kalimdor, O Outland, N Northrend, D dungeons, R raids,
 * B battlegrounds, C classes, P professions, W world events, X the rest. Headings are a zone's
 * area id, SORT_BASE + a quest sort, INSTANCE_BASE + a map, or 0 for quests with no place at all.
 * countable leaves out repeatable quests, which are never done for good.
 */

#include "Journal.h"
#include "ConditionMgr.h"
#include "Config.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "GameObject.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Journal::Quests
{
    namespace
    {
        constexpr uint32 OTHER = 0;
        constexpr uint32 SORT_BASE = 100000;
        constexpr uint32 INSTANCE_BASE = 200000;
        constexpr uint32 IP_QUEST_BASE = 66000;
        constexpr uint32 IP_STATE_MAX = 18;
        constexpr std::size_t MAX_HEADING_QUESTS = 400;
        constexpr std::size_t MAX_SEARCH_RESULTS = 50;

        // The quest log's sorts (QuestSort.dbc, 3.3.5 names: 41 and 241 were reused for Day of
        // the Dead and the Argent Tournament).
        struct SortInfo
        {
            uint32 sort;
            char group;
            char const* name;
        };

        constexpr SortInfo SORTS[] =
        {
            { QUEST_SORT_EPIC, 'X', "Epic" },
            { QUEST_SORT_SEASONAL, 'W', "Seasonal" },
            { QUEST_SORT_HERBALISM, 'P', "Herbalism" },
            { QUEST_SORT_BATTLEGROUNDS, 'B', "Battlegrounds" },
            { QUEST_SORT_ULDAMN_OLD, 'W', "Day of the Dead" },
            { QUEST_SORT_WARLOCK, 'C', "Warlock" },
            { QUEST_SORT_WARRIOR, 'C', "Warrior" },
            { QUEST_SORT_SHAMAN, 'C', "Shaman" },
            { QUEST_SORT_FISHING, 'P', "Fishing" },
            { QUEST_SORT_BLACKSMITHING, 'P', "Blacksmithing" },
            { QUEST_SORT_PALADIN, 'C', "Paladin" },
            { QUEST_SORT_MAGE, 'C', "Mage" },
            { QUEST_SORT_ROGUE, 'C', "Rogue" },
            { QUEST_SORT_ALCHEMY, 'P', "Alchemy" },
            { QUEST_SORT_LEATHERWORKING, 'P', "Leatherworking" },
            { QUEST_SORT_ENGINEERING, 'P', "Engineering" },
            { QUEST_SORT_TREASURE_MAP, 'X', "Treasure Map" },
            { QUEST_SORT_SUNKEN_TEMPLE_OLD, 'X', "Argent Tournament" },
            { QUEST_SORT_HUNTER, 'C', "Hunter" },
            { QUEST_SORT_PRIEST, 'C', "Priest" },
            { QUEST_SORT_DRUID, 'C', "Druid" },
            { QUEST_SORT_TAILORING, 'P', "Tailoring" },
            { QUEST_SORT_SPECIAL, 'W', "Special" },
            { QUEST_SORT_COOKING, 'P', "Cooking" },
            { QUEST_SORT_FIRST_AID, 'P', "First Aid" },
            { QUEST_SORT_LEGENDARY, 'X', "Legendary" },
            { QUEST_SORT_DARKMOON_FAIRE, 'W', "Darkmoon Faire" },
            { QUEST_SORT_AHN_QIRAJ_WAR, 'X', "Ahn'Qiraj War" },
            { QUEST_SORT_LUNAR_FESTIVAL, 'W', "Lunar Festival" },
            { QUEST_SORT_REPUTATION, 'X', "Reputation" },
            { QUEST_SORT_INVASION, 'W', "Scourge Invasion" },
            { QUEST_SORT_MIDSUMMER, 'W', "Midsummer" },
            { QUEST_SORT_BREWFEST, 'W', "Brewfest" },
            { QUEST_SORT_INSCRIPTION, 'P', "Inscription" },
            { QUEST_SORT_DEATH_KNIGHT, 'C', "Death Knight" },
            { QUEST_SORT_JEWELCRAFTING, 'P', "Jewelcrafting" },
            { QUEST_SORT_NOBLEGARDEN, 'W', "Noblegarden" },
            { QUEST_SORT_PILGRIMS_BOUNTY, 'W', "Pilgrim's Bounty" },
            { QUEST_SORT_LOVE_IS_IN_THE_AIR, 'W', "Love is in the Air" },
        };

        struct Spot
        {
            uint32 map = 0;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            uint32 zone = 0;      // 0 in instances
            bool placed = false;
            // The mod-individual-progression states its spawns are there for, from their
            // npc_ipp_* / gobject_ipp_* scripts: from eraMin, until eraEnd (255: for good).
            uint8 eraMin = 0;
            uint8 eraEnd = 255;
            uint32 phases = 0;    // every phase its spawns are in
        };

        // mod-individual-progression's awareness scripts: the states a creature or object
        // with one shows itself to (IndividualProgressionAwareness.cpp).
        struct EraScript
        {
            char const* name;
            uint8 min;
            uint8 end;
        };

        constexpr EraScript ERA_SCRIPTS[] =
        {
            { "npc_ipp_preaq", 3, 255 }, { "gobject_ipp_preaq", 3, 255 },
            { "npc_ipp_we", 3, 4 },
            { "npc_ipp_aqwewar", 3, 5 },
            { "npc_ipp_aqwar", 4, 5 }, { "gobject_ipp_aqwar", 4, 5 },
            { "npc_ipp_aq", 5, 255 },
            { "npc_ipp_si", 6, 7 }, { "gobject_ipp_si", 6, 7 },
            { "npc_ipp_naxx40", 6, 255 }, { "gobject_ipp_naxx40", 6, 255 },
            { "npc_ipp_pre_naxx40", 0, 6 },
            { "npc_ipp_pre_tbc", 0, 8 }, { "gobject_ipp_pre_tbc", 0, 8 },
            { "npc_ipp_tbc", 8, 255 }, { "gobject_ipp_tbc", 8, 255 },
            { "npc_ipp_tbc_pre_t3", 0, 10 },
            { "npc_ipp_tbc_pvp", 8, 13 },
            { "npc_ipp_tbc_S1", 8, 13 }, { "npc_ipp_tbc_S2", 8, 13 }, { "npc_ipp_tbc_S3", 8, 13 }, { "npc_ipp_tbc_S4", 8, 13 },
            { "npc_ipp_wotlk_S5", 13, 255 }, { "npc_ipp_wotlk_S6", 13, 255 }, { "npc_ipp_wotlk_S7", 13, 255 },
            { "npc_ipp_wotlk_S8", 13, 255 },
            { "npc_ipp_tbc_t3", 10, 255 },
            { "gobject_ipp_tbc_t4", 12, 255 },
            { "npc_ipp_pre_wotlk", 0, 13 },
            { "npc_ipp_wotlk", 13, 255 }, { "gobject_ipp_wotlk", 13, 255 },
            { "npc_ipp_wotlk_ulduar", 14, 255 },
            { "npc_ipp_wotlk_totc", 15, 255 },
            { "npc_ipp_wotlk_icc", 16, 255 },
        };

        // mod-individual-progression's phases (an aura it puts on players by area and stage,
        // checkIPPhasing): the states a giver seen only in one of them is there for, by its zone.
        constexpr uint32 IPP_PHASE = 65536;
        constexpr uint32 IPP_PHASE_II = 131072;
        constexpr uint32 IPP_PHASE_III = 262144;
        constexpr uint32 IPP_PHASE_IV = 524288;

        struct EraPhase
        {
            uint32 phase;
            uint32 zone;
            uint8 min;
            uint8 end;
        };

        constexpr EraPhase ERA_PHASES[] =
        {
            // The AQ war in Silithus and its camps in Darkshore.
            { IPP_PHASE, 1377, 4, 5 }, { IPP_PHASE, 148, 4, 5 }, { IPP_PHASE_II, 1377, 5, 255 },
            // The Emerald Dragons' portals.
            { IPP_PHASE, 331, 2, 255 }, { IPP_PHASE, 47, 2, 255 }, { IPP_PHASE, 357, 2, 255 }, { IPP_PHASE, 10, 2, 255 },
            // The Scourge Invasion.
            { IPP_PHASE, 14, 6, 7 }, { IPP_PHASE, 85, 6, 7 }, { IPP_PHASE, 215, 6, 7 }, { IPP_PHASE, 12, 6, 7 },
            { IPP_PHASE, 1, 6, 7 }, { IPP_PHASE, 141, 6, 7 }, { IPP_PHASE, 139, 6, 7 }, { IPP_PHASE, 4, 6, 7 },
            { IPP_PHASE, 440, 6, 7 }, { IPP_PHASE, 618, 6, 7 }, { IPP_PHASE, 16, 6, 7 }, { IPP_PHASE, 46, 6, 7 },
            { IPP_PHASE, 25, 6, 7 }, { IPP_PHASE, 51, 6, 7 }, { IPP_PHASE, 1519, 6, 7 }, { IPP_PHASE, 1637, 6, 7 },
            // The Argent Tournament.
            { IPP_PHASE, 210, 15, 255 },
            // Silvermoon before the Sunwell, and the Isle of Quel'Danas's offensive.
            { IPP_PHASE_II, 3487, 0, 12 }, { IPP_PHASE_II, 4080, 12, 255 }, { IPP_PHASE_III, 4080, 12, 255 },
            { IPP_PHASE_IV, 4080, 12, 255 },
        };

        // Narrows a spot's era window when every spawn of it is in one of
        // mod-individual-progression's phases.
        void ApplyEraPhase(Spot& spot)
        {
            Config const& config = GetConfig();
            if (!config.progressionEnabled || !config.eraLocks || (spot.phases & PHASEMASK_NORMAL) || !spot.zone)
                return;
            for (EraPhase const& e : ERA_PHASES)
                if ((spot.phases & e.phase) && e.zone == spot.zone)
                {
                    spot.eraMin = std::max(spot.eraMin, e.min);
                    spot.eraEnd = std::min(spot.eraEnd, e.end);
                    return;
                }
        }

        // The states a script id shows its creature or object to; [0, 255) for everything else.
        std::pair<uint8, uint8> EraWindow(uint32 scriptId)
        {
            Config const& config = GetConfig();
            if (!scriptId || !config.progressionEnabled || !config.eraLocks)
                return { 0, 255 };
            std::string const& name = sObjectMgr->GetScriptName(scriptId);
            if (name == "npc_ipp_zg")
                return { config.zulGurubState, 255 };
            if (name == "npc_ipp_za")
                return { config.zulAmanState, 255 };
            if (name == "npc_ipp_ds2")
                return { uint8(sConfigMgr->GetOption<bool>("IndividualProgression.AllowEarlyDungeonSet2", false, false) ? 0 : 3), 255 };
            for (EraScript const& e : ERA_SCRIPTS)
                if (name == e.name)
                    return { e.min, e.end };
            return { 0, 255 };
        }

        struct Entry
        {
            uint32 heading = OTHER;
            char giverKind = '-';  // C creature, G gameobject, I item, - something else
            uint32 giverEntry = 0;
            std::string giver;
            Spot spot;
            uint8 era = 0;
            uint8 eraEnd = 255;    // the state its giver goes away at
            uint8 step = 0;        // quests before it in its chain
            std::string title;     // lower case, for the search
        };

        struct Heading
        {
            uint32 id = OTHER;
            char group = 'X';
            std::string name;
            uint8 era = 0;
            uint8 low = 0;
            uint8 high = 0;
            uint32 zone = 0;       // the zone a zone heading is
            uint32 map = 0;        // the map an instance heading is
            std::vector<uint32> quests;
        };

        struct Data
        {
            std::unordered_map<uint32, Entry> entries;
            std::vector<Heading> headings;
            std::unordered_map<uint32, std::size_t> byId;
        };

        Data sData;

        // The zone an area is in (an area of a subzone, a subzone of a zone).
        AreaTableEntry const* TopZone(uint32 areaId)
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
            for (uint8 i = 0; area && area->zone && i < 4; ++i)
                if (AreaTableEntry const* parent = sAreaTableStore.LookupEntry(area->zone))
                    area = parent;
                else
                    break;
            return area;
        }

        char MapGroup(uint32 mapId)
        {
            switch (mapId)
            {
                case 0: case 609: return 'E'; // Eastern Kingdoms, the Scarlet Enclave
                case 1: return 'K';
                case 530: return 'O';
                case 571: return 'N';
                default: break;
            }
            MapEntry const* map = sMapStore.LookupEntry(mapId);
            if (!map)
                return 'X';
            if (map->IsBattlegroundOrArena())
                return 'B';
            if (map->IsRaid())
                return 'R';
            if (map->Instanceable())
                return 'D';
            return 'X';
        }

        char ZoneGroup(AreaTableEntry const* zone)
        {
            switch (zone->ID)
            {
                case 3430: case 3433: case 3487: case 4080: // Eversong Woods, Ghostlands, Silvermoon, Isle of Quel'Danas
                    return 'E';
                case 3524: case 3525: case 3557:            // Azuremyst Isle, Bloodmyst Isle, the Exodar
                    return 'K';
                default:
                    return MapGroup(zone->mapid);
            }
        }

        Heading& HeadingFor(uint32 id)
        {
            auto [itr, inserted] = sData.byId.emplace(id, sData.headings.size());
            if (inserted)
            {
                sData.headings.emplace_back();
                sData.headings.back().id = id;
            }
            return sData.headings[itr->second];
        }

        Heading& ZoneHeading(AreaTableEntry const* zone)
        {
            bool fresh = !sData.byId.count(zone->ID);
            Heading& h = HeadingFor(zone->ID);
            if (fresh)
            {
                h.group = ZoneGroup(zone);
                h.name = Index::AreaName(zone->ID);
                h.zone = zone->ID;
                h.map = zone->mapid;
                h.era = Collections::ZoneEra(zone->mapid, zone->ID);
                // Battlegrounds aren't in the dungeon journal: by their map's expansion.
                if (MapEntry const* map = sMapStore.LookupEntry(zone->mapid); map && map->Instanceable())
                    h.era = std::max(h.era, Index::MapEra(zone->mapid));
            }
            return h;
        }

        Heading& SortHeading(uint32 sort)
        {
            for (SortInfo const& info : SORTS)
                if (info.sort == sort)
                {
                    bool fresh = !sData.byId.count(SORT_BASE + sort);
                    Heading& h = HeadingFor(SORT_BASE + sort);
                    if (fresh)
                    {
                        h.group = info.group;
                        h.name = info.name;
                    }
                    return h;
                }
            return HeadingFor(OTHER);
        }

        // A quest with neither zone nor sort, given inside an instance.
        Heading& InstanceHeading(uint32 mapId)
        {
            bool fresh = !sData.byId.count(INSTANCE_BASE + mapId);
            Heading& h = HeadingFor(INSTANCE_BASE + mapId);
            if (fresh)
            {
                Index::Place place;
                h.group = MapGroup(mapId);
                h.name = Index::FindInstanceOnMap(mapId, place) ? place.name : Index::MapName(mapId);
                h.map = mapId;
                h.era = Index::MapEra(mapId);
            }
            return h;
        }

        // The zone a spot on an open map is in; 0 in instances. A first start may load terrain
        // here, before the world loop runs: keep the freeze detector from taking it for a hang.
        uint32 ZoneAt(uint32 map, float x, float y, float z)
        {
            static uint32 computed = 0;
            MapEntry const* entry = sMapStore.LookupEntry(map);
            if (!entry || entry->Instanceable())
                return 0;
            uint32 zone = 0;
            uint32 area = 0;
            sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, area, map, x, y, z);
            if (++computed % 50 == 0)
                ++World::m_worldLoopCounter;
            return zone;
        }

        // Where each giver stands: of its spawns, one in the normal phase on an open map in the
        // most open era (a Darkmoon Faire barker in Elwynn rather than Terokkar), else the first.
        // Its era window covers all its spawns: one spawn for classic and one for later is there
        // for good. templateScript: the giver's own script id.
        template <class Spawns, class TemplateScript>
        void ChooseSpots(Spawns const& spawns, std::unordered_map<uint32, Spot>& wanted, TemplateScript&& templateScript)
        {
            std::unordered_map<uint32, uint32> best;
            auto score = [](uint32 map, uint32 phaseMask)
            {
                MapEntry const* entry = sMapStore.LookupEntry(map);
                return ((phaseMask & PHASEMASK_NORMAL) ? 0u : 1000u) + (entry && !entry->Instanceable() ? 0u : 100u)
                    + Index::MapEra(map);
            };
            for (auto const& [spawnId, data] : spawns)
            {
                auto itr = wanted.find(data.id);
                if (itr == wanted.end())
                    continue;
                auto [eraMin, eraEnd] = EraWindow(data.ScriptId ? data.ScriptId : templateScript(data.id));
                uint32 s = score(data.mapid, data.phaseMask);
                auto [b, inserted] = best.emplace(data.id, s);
                itr->second.phases |= data.phaseMask;
                if (!inserted)
                {
                    itr->second.eraMin = std::min(itr->second.eraMin, eraMin);
                    itr->second.eraEnd = std::max(itr->second.eraEnd, eraEnd);
                    if (s >= b->second)
                        continue;
                }
                else
                {
                    itr->second.eraMin = eraMin;
                    itr->second.eraEnd = eraEnd;
                }
                b->second = s;
                Spot& spot = itr->second;
                spot.map = data.mapid;
                spot.x = data.posX;
                spot.y = data.posY;
                spot.z = data.posZ;
                spot.placed = true;
            }
            for (auto& [entry, spot] : wanted)
                if (spot.placed)
                {
                    spot.zone = ZoneAt(spot.map, spot.x, spot.y, spot.z);
                    ApplyEraPhase(spot);
                }
        }

        // The quest before this one: the one it asks for, or the one that offers it on turn-in.
        uint32 ParentOf(Quest const* q)
        {
            if (q->GetPrevQuestId() > 0)
                return uint32(q->GetPrevQuestId());
            if (!q->prevChainQuests.empty())
                return q->prevChainQuests.front();
            return 0;
        }

        QuestState StateOf(Player* player, Quest const* q, uint8 era, uint8 eraEnd)
        {
            uint32 questId = q->GetQuestId();
            QuestStatus status = player->GetQuestStatus(questId);
            if (status == QUEST_STATUS_COMPLETE)
                return QSTATE_COMPLETE;
            if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_FAILED)
                return QSTATE_ACTIVE;
            if (!q->IsRepeatable() && player->IsQuestRewarded(questId))
                return QSTATE_DONE;
            if (era && ProgressionState(player) < era)
                return QSTATE_ERA;
            // Its giver is gone at the character's stage (game masters see every spawn).
            if (eraEnd < 255 && !player->IsGameMaster() && ProgressionState(player) >= eraEnd)
                return QSTATE_LOCKED;
            if (!player->SatisfyQuestLevel(q, false))
                return QSTATE_LOW_LEVEL;
            if (!player->SatisfyQuestPreviousQuest(q, false) || !player->SatisfyQuestPrevChain(q, false))
                return QSTATE_PREREQ;
            if (player->CanTakeQuest(q, false))
                return QSTATE_AVAILABLE;
            return QSTATE_LOCKED;
        }

        // A quest that can still be done for good by the character, or was: not repeatable, its
        // giver not gone at their stage, not a breadcrumb they've gone past.
        bool Countable(Player* player, Quest const* q, Entry const& e)
        {
            if (q->IsRepeatable())
                return false;
            uint32 questId = q->GetQuestId();
            if (player->IsQuestRewarded(questId))
                return true;
            if (e.eraEnd < 255 && !player->IsGameMaster() && ProgressionState(player) >= e.eraEnd)
                return false;
            if (uint32 target = q->GetBreadcrumbForQuestId())
                if (player->IsQuestRewarded(target) || player->GetQuestStatus(target) != QUEST_STATUS_NONE)
                    return false;
            return true;
        }

        bool CanSee(Player* player, Quest const* q)
        {
            return player->IsGameMaster() || (player->SatisfyQuestRace(q, false) && player->SatisfyQuestClass(q, false));
        }

        // Where the giver is, said only when it isn't the heading itself.
        std::string WhereOf(Entry const& e, Heading const* heading)
        {
            if (!e.spot.placed)
                return "";
            if (e.spot.zone)
                return heading && heading->zone == e.spot.zone ? "" : Index::AreaName(e.spot.zone);
            if (heading && heading->map == e.spot.map)
                return "";
            Index::Place place;
            if (Index::FindInstanceOnMap(e.spot.map, place))
                return place.name;
            return Index::MapName(e.spot.map);
        }

        void AppendEntry(Player* player, Quest const* q, Entry const& e, Heading const* heading, std::vector<std::string>& rows)
        {
            AppendRows(player, q, e.giverKind, e.giverEntry, e.giver, WhereOf(e, heading), rows);
        }
    }

    void Build()
    {
        uint32 start = getMSTime();
        sData = Data();

        // Who hands each quest out.
        std::unordered_map<uint32, std::pair<char, uint32>> giver;
        for (auto const& [creature, quest] : *sObjectMgr->GetCreatureQuestRelationMap())
            giver.emplace(quest, std::make_pair('C', creature));
        for (auto const& [object, quest] : *sObjectMgr->GetGOQuestRelationMap())
            giver.emplace(quest, std::make_pair('G', object));
        // Holiday quests are given out only while their event runs.
        if (QueryResult result = WorldDatabase.Query("SELECT CAST(quest AS UNSIGNED), CAST(id AS UNSIGNED) FROM game_event_creature_quest"))
            do
            {
                Field* f = result->Fetch();
                giver.emplace(uint32(f[0].Get<uint64>()), std::make_pair('C', uint32(f[1].Get<uint64>())));
            } while (result->NextRow());
        if (QueryResult result = WorldDatabase.Query("SELECT CAST(quest AS UNSIGNED), CAST(id AS UNSIGNED) FROM game_event_gameobject_quest"))
            do
            {
                Field* f = result->Fetch();
                giver.emplace(uint32(f[0].Get<uint64>()), std::make_pair('G', uint32(f[1].Get<uint64>())));
            } while (result->NextRow());
        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
            if (proto.StartQuest)
                giver.emplace(proto.StartQuest, std::make_pair('I', entry));

        // Quests something else hands out: a spell, a SmartAI script, the Dungeon Finder's reward
        // quests, or the quest before them in a chain.
        std::unordered_set<uint32> started;
        for (uint32 i = 0; i < sSpellMgr->GetSpellInfoStoreSize(); ++i)
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(i))
                for (SpellEffectInfo const& eff : spell->Effects)
                    if (eff.Effect == SPELL_EFFECT_QUEST_START && eff.MiscValue > 0)
                        started.insert(uint32(eff.MiscValue));
        if (QueryResult result = WorldDatabase.Query("SELECT CAST(action_param1 AS UNSIGNED) FROM smart_scripts WHERE action_type = 7"))
            do
            {
                started.insert(uint32(result->Fetch()[0].Get<uint64>())); // SMART_ACTION_OFFER_QUEST
            } while (result->NextRow());
        if (QueryResult result = WorldDatabase.Query("SELECT CAST(firstQuestId AS UNSIGNED), CAST(otherQuestId AS UNSIGNED) FROM lfg_dungeon_rewards"))
            do
            {
                Field* f = result->Fetch();
                started.insert(uint32(f[0].Get<uint64>()));
                started.insert(uint32(f[1].Get<uint64>()));
            } while (result->NextRow());
        for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
            if (quest->GetNextQuestInChain())
                started.insert(quest->GetNextQuestInChain());

        // The highest mod-individual-progression state a quest's own conditions ask for.
        std::unordered_map<uint32, uint8> conditionEra;
        if (GetConfig().progressionEnabled && GetConfig().eraLocks)
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(SourceEntry AS UNSIGNED), CAST(ConditionValue1 AS UNSIGNED) "
                "FROM conditions WHERE SourceTypeOrReferenceId = {} AND ConditionTypeOrReference = {} AND NegativeCondition = 0 "
                "AND ConditionValue1 BETWEEN {} AND {}", uint32(CONDITION_SOURCE_TYPE_QUEST_AVAILABLE), uint32(CONDITION_QUESTREWARDED),
                IP_QUEST_BASE + 1, IP_QUEST_BASE + IP_STATE_MAX))
                do
                {
                    Field* f = result->Fetch();
                    uint8 state = uint8(f[1].Get<uint64>() - IP_QUEST_BASE);
                    uint8& era = conditionEra[uint32(f[0].Get<uint64>())];
                    era = std::max(era, state);
                } while (result->NextRow());

        // The quests, and where their givers stand.
        std::vector<Quest const*> quests;
        std::unordered_map<uint32, Spot> creatureSpots, objectSpots;
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            if (quest->HasFlag(QUEST_FLAGS_TRACKING))
                continue;
            auto g = giver.find(questId);
            if (g == giver.end() && !started.count(questId))
                continue;
            if (DisableMgr::IsDisabledFor(DISABLE_TYPE_QUEST, questId, nullptr))
                continue;
            quests.push_back(quest);
            Entry& e = sData.entries[questId];
            e.title = Lower(quest->GetTitle());
            if (g == giver.end())
                continue;
            e.giverKind = g->second.first;
            e.giverEntry = g->second.second;
            if (e.giverKind == 'C')
            {
                creatureSpots.emplace(e.giverEntry, Spot());
                if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(e.giverEntry))
                    e.giver = t->Name;
            }
            else if (e.giverKind == 'G')
            {
                objectSpots.emplace(e.giverEntry, Spot());
                if (GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(e.giverEntry))
                    e.giver = t->name;
            }
            else if (ItemTemplate const* t = sObjectMgr->GetItemTemplate(e.giverEntry))
                e.giver = t->Name1;
        }
        ChooseSpots(sObjectMgr->GetAllCreatureData(), creatureSpots, [](uint32 entry)
        {
            CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry);
            return t ? t->ScriptID : 0u;
        });
        ChooseSpots(sObjectMgr->GetAllGOData(), objectSpots, [](uint32 entry)
        {
            GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(entry);
            return t ? t->ScriptId : 0u;
        });

        // A creature or object that's never spawned (nor summoned) gives nothing.
        quests.erase(std::remove_if(quests.begin(), quests.end(), [&](Quest const* q)
        {
            Entry const& e = sData.entries[q->GetQuestId()];
            bool gone = (e.giverKind == 'C' && !creatureSpots[e.giverEntry].placed && !Index::IsSummonable(e.giverEntry))
                || (e.giverKind == 'G' && !objectSpots[e.giverEntry].placed);
            if (gone && !started.count(q->GetQuestId()))
            {
                sData.entries.erase(q->GetQuestId());
                return true;
            }
            return false;
        }), quests.end());

        // Headings, and each quest's era.
        for (Quest const* q : quests)
        {
            Entry& e = sData.entries[q->GetQuestId()];
            if (e.giverKind == 'C')
                e.spot = creatureSpots[e.giverEntry];
            else if (e.giverKind == 'G')
                e.spot = objectSpots[e.giverEntry];

            AreaTableEntry const* zone = nullptr;
            int32 sort = q->GetZoneOrSort();
            if (sort > 0)
                zone = TopZone(uint32(sort));
            else if (sort == 0 && e.spot.placed && e.spot.zone)
                zone = TopZone(e.spot.zone);

            Heading* h;
            if (zone)
                h = &ZoneHeading(zone);
            else if (sort < 0)
                h = &SortHeading(uint32(-sort));
            else if (sort == 0 && e.spot.placed && !e.spot.zone)
                h = &InstanceHeading(e.spot.map);
            else
                h = &HeadingFor(OTHER);
            e.heading = h->id;
            h->quests.push_back(q->GetQuestId());

            uint8 era = std::max(h->era, e.spot.eraMin);
            e.eraEnd = e.spot.eraEnd;
            if (e.spot.placed)
                era = std::max(era, Collections::ZoneEra(e.spot.map, e.spot.zone));
            if (auto itr = conditionEra.find(q->GetQuestId()); itr != conditionEra.end())
                era = std::max(era, itr->second);
            e.era = era;
        }
        if (Heading* other = sData.byId.count(OTHER) ? &sData.headings[sData.byId[OTHER]] : nullptr)
            other->name = "Miscellaneous";

        // Quests with neither zone nor sort given inside an instance join the instance's own
        // heading, when its other quests made one.
        for (Heading& h : sData.headings)
        {
            if (h.id < INSTANCE_BASE || h.quests.empty())
                continue;
            for (Heading& zone : sData.headings)
            {
                if (!zone.zone || zone.map != h.map)
                    continue;
                for (uint32 questId : h.quests)
                    sData.entries[questId].heading = zone.id;
                zone.quests.insert(zone.quests.end(), h.quests.begin(), h.quests.end());
                h.quests.clear();
                break;
            }
        }
        sData.headings.erase(std::remove_if(sData.headings.begin(), sData.headings.end(),
            [](Heading const& h) { return h.quests.empty(); }), sData.headings.end());

        // Chains: how many quests come before each, and the quest that starts it, so a heading
        // lists a chain's quests one after another, in the order they're done.
        std::unordered_map<uint32, std::pair<uint32, uint8>> chain; // quest -> (first quest, step)
        for (Quest const* q : quests)
        {
            uint32 root = q->GetQuestId();
            uint8 step = 0;
            std::unordered_set<uint32> seen{ root };
            while (step < 60)
            {
                Quest const* cur = sObjectMgr->GetQuestTemplate(root);
                uint32 parent = cur ? ParentOf(cur) : 0;
                if (!parent || !seen.insert(parent).second || !sObjectMgr->GetQuestTemplate(parent))
                    break;
                root = parent;
                ++step;
            }
            chain[q->GetQuestId()] = { root, step };
            sData.entries[q->GetQuestId()].step = step;
        }

        auto levelOf = [](Quest const* q) { return uint32(q->GetQuestLevel() > 0 ? q->GetQuestLevel() : q->GetMinLevel()); };
        for (Heading& h : sData.headings)
        {
            auto key = [&](uint32 id)
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(id);
                auto [rootId, step] = chain[id];
                Quest const* root = sObjectMgr->GetQuestTemplate(rootId);
                return std::make_tuple(root->GetMinLevel(), levelOf(root), rootId, step, levelOf(q), id);
            };
            std::sort(h.quests.begin(), h.quests.end(), [&](uint32 a, uint32 b) { return key(a) < key(b); });

            // The level range most of its quests are in.
            std::vector<uint32> levels;
            for (uint32 id : h.quests)
                if (Quest const* q = sObjectMgr->GetQuestTemplate(id); q && q->GetQuestLevel() > 0)
                    levels.push_back(uint32(q->GetQuestLevel()));
            std::sort(levels.begin(), levels.end());
            if (!levels.empty())
            {
                h.low = uint8(std::min<uint32>(levels[levels.size() / 10], 255));
                h.high = uint8(std::min<uint32>(levels[(levels.size() - 1) * 9 / 10], 255));
            }
        }

        // Group by group, then in levelling order.
        std::string const groups = "EKONDRBCPWX";
        std::sort(sData.headings.begin(), sData.headings.end(), [&](Heading const& a, Heading const& b)
        {
            std::size_t ga = groups.find(a.group), gb = groups.find(b.group);
            if (ga != gb)
                return ga < gb;
            if (a.id == OTHER || b.id == OTHER)
                return b.id == OTHER && a.id != OTHER;
            // Classes, professions and events by name; places by level.
            if (a.group == 'C' || a.group == 'P' || a.group == 'W' || a.group == 'X')
                return a.name < b.name;
            if (a.low != b.low)
                return a.low < b.low;
            return a.name < b.name;
        });
        sData.byId.clear();
        for (std::size_t i = 0; i < sData.headings.size(); ++i)
            sData.byId[sData.headings[i].id] = i;

        LOG_INFO("server.loading", ">> mod-forever-journal: {} quests under {} headings in {} ms",
            quests.size(), sData.headings.size(), GetMSTimeDiffToNow(start));
    }

    void AppendRows(Player* player, Quest const* q, char giverKind, uint32 giverEntry, std::string_view giver,
        std::string_view where, std::vector<std::string>& rows)
    {
        uint32 questId = q->GetQuestId();
        uint8 era = 0;
        uint8 eraEnd = 255;
        uint8 step = 0;
        if (auto itr = sData.entries.find(questId); itr != sData.entries.end())
        {
            era = itr->second.era;
            eraEnd = itr->second.eraEnd;
            step = itr->second.step;
        }
        QuestState state = StateOf(player, q, era, eraEnd);

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
        if (player->GetLevel() < sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        {
            xp = player->CalculateQuestRewardXP(q);
            sScriptMgr->OnPlayerQuestComputeXP(player, q, xp);
        }
        else
            money += q->GetRewMoneyMaxLevel();

        rows.push_back(Acore::StringFormat("Q,{},{},{},{},{},{},{},{},{},{},{},{},{},{}", questId, uint32(state),
            q->GetQuestLevel() > 0 ? q->GetQuestLevel() : 0, q->GetMinLevel(), flags, xp, money > 0 ? money : 0,
            giverKind, giverEntry, Escape(q->GetTitle(), 60), Escape(giver, 30), Escape(where, 30), uint32(step), uint32(era)));
        rows.push_back(Acore::StringFormat("R,{},{},{}", questId,
            Index::ItemList(q->RewardItemId, q->RewardItemIdCount, QUEST_REWARDS_COUNT),
            Index::ItemList(q->RewardChoiceItemId, q->RewardChoiceItemCount, QUEST_REWARD_CHOICES_COUNT)));
    }

    void HandleCategories(Player* player, std::string const& req)
    {
        std::vector<std::string> rows;
        for (Heading const& h : sData.headings)
        {
            uint32 total = 0;
            uint32 countable = 0;
            uint32 done = 0;
            std::unordered_map<int32, bool> exclusive; // a group of alternatives counts once
            for (uint32 questId : h.quests)
            {
                Quest const* q = sObjectMgr->GetQuestTemplate(questId);
                if (!q || !CanSee(player, q))
                    continue;
                ++total;
                if (!Countable(player, q, sData.entries[questId]))
                    continue;
                bool rewarded = player->IsQuestRewarded(questId);
                if (q->GetExclusiveGroup() > 0)
                {
                    auto [itr, inserted] = exclusive.emplace(q->GetExclusiveGroup(), rewarded);
                    itr->second = itr->second || rewarded;
                    continue;
                }
                ++countable;
                if (rewarded)
                    ++done;
            }
            for (auto const& [group, rewarded] : exclusive)
            {
                ++countable;
                if (rewarded)
                    ++done;
            }
            if (total)
                rows.push_back(Acore::StringFormat("G,{},{},{},{},{},{},{},{},{}", h.group, h.id, total, countable, done,
                    uint32(h.era), uint32(h.low), uint32(h.high), Escape(h.name, 40)));
        }

        // The heading of where the player is: their zone, or the instance they're in.
        uint32 here = OTHER;
        if (AreaTableEntry const* zone = TopZone(player->GetZoneId()); zone && sData.byId.count(zone->ID))
            here = zone->ID;
        else if (sData.byId.count(INSTANCE_BASE + player->GetMapId()))
            here = INSTANCE_BASE + player->GetMapId();

        Send(player, "GR:" + req + ":" + std::to_string(rows.size()) + ":" + std::to_string(here));
        SendRows(player, "GD:" + req, rows);
        Send(player, "GE:" + req);
    }

    void HandleCategory(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint32 id = 0;
        auto itr = args.size() > 2 && ParseUInt(args[2], id) ? sData.byId.find(id) : sData.byId.end();
        if (itr == sData.byId.end())
        {
            SendError(player, req, "bad");
            return;
        }
        Heading const& h = sData.headings[itr->second];
        std::vector<std::string> rows;
        uint32 count = 0;
        uint32 total = 0;
        for (uint32 questId : h.quests)
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(questId);
            if (!q || !CanSee(player, q))
                continue;
            if (++total > MAX_HEADING_QUESTS)
                continue;
            AppendEntry(player, q, sData.entries[questId], &h, rows);
            ++count;
        }
        Send(player, "ZR:" + req + ":" + std::to_string(h.id) + ":" + std::to_string(count) + ":" + std::to_string(total));
        SendRows(player, "ZD:" + req, rows);
        Send(player, "ZE:" + req);
    }

    void HandleSearch(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        std::string text = args.size() > 2 ? Lower(args[2]) : std::string();
        if (text.size() < 3)
        {
            SendError(player, req, "bad");
            return;
        }
        std::vector<std::string> rows;
        uint32 count = 0;
        for (Heading const& h : sData.headings)
            for (uint32 questId : h.quests)
            {
                if (count >= MAX_SEARCH_RESULTS)
                    break;
                Entry const& e = sData.entries[questId];
                if (e.title.find(text) == std::string::npos)
                    continue;
                Quest const* q = sObjectMgr->GetQuestTemplate(questId);
                if (!q || !CanSee(player, q))
                    continue;
                rows.push_back(Acore::StringFormat("C,{},{}", questId, h.id));
                AppendEntry(player, q, e, &h, rows);
                ++count;
            }
        Send(player, "XR:" + req + ":" + std::to_string(count));
        SendRows(player, "XD:" + req, rows);
        Send(player, "XE:" + req);
    }

    bool ShowGiver(Player* player, std::string const& req, uint32 questId)
    {
        auto itr = sData.entries.find(questId);
        if (itr == sData.entries.end() || !itr->second.spot.placed)
            return false;
        Entry const& e = itr->second;
        Index::SendPoi(player, e.spot.map, e.spot.x, e.spot.y, e.giver);
        Send(player, "M:" + req + ":ok:" + Escape(Index::WhereText(player, e.spot.map, e.spot.x, e.spot.y, e.spot.zone), 120));
        return true;
    }
    char GroupOfZone(uint32 zoneId)
    {
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
        return zone ? ZoneGroup(zone) : 'X';
    }
}
