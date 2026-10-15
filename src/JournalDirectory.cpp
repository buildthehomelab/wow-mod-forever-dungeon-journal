/*
 * mod-forever-journal: the Directory, a town guide. Every friendly service NPC and object on the
 * continents: class, profession, riding, weapon and pet trainers, flight masters, innkeepers,
 * banks, auction houses, mailboxes, vendors and the rest, nearest first, flagged on the map.
 *
 * Everything comes from the realm's own spawns, read at startup, so NPCs other modules add are
 * found like any other. Only what the character can use is listed: nothing hostile to them,
 * holiday NPCs outside their event, phased NPCs they can't see, and with mod-individual-progression
 * nothing on a continent their era hasn't opened.
 *
 * The Journal addon shows it as the Directory tab. The Town Guide spell and .find open that tab
 * for characters whose addon said it can (HELLO's ADDON_OPEN bit), and a gossip menu with the
 * same search for everyone else.
 *
 *   N:<req>:C:<category>   -> NR, ND rows, NE    a category (CATEGORY_* below; 1 = your class trainer)
 *   N:<req>:P:<skill>      -> NR, ND rows, NE    a profession's trainers
 *   N:<req>:S:<text>       -> NR, ND rows, NE    by name, title or zone; every word must match
 *   M:<req>:N:<place>:<id> -> M:<req>:ok:<where>  flags a result
 *
 *   NR:<req>:<rows>:<found>
 *   ND rows: <place>,<C|G>,<id>,<name>,<title>,<zone>,<area>,<x x10>,<y x10>,<continent>,<yards>,<direction>,<rank>
 *     x/y are the zone map's coordinates (-1 when the zone has no map), continent is "-" on the
 *     player's own map, yards is -1 elsewhere, direction 0-7 is north, north-east... (8 = none),
 *     rank is the highest profession rank a trainer teaches (0, or 1 Apprentice to 6 Grand Master).
 *
 * Pushed by the server: OPEN:0:directory[:<search text>] opens the tab.
 */

#include "Journal.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "GameObjectData.h"
#include "GossipDef.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Trainer.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace Journal::Directory
{
    namespace
    {
        // The gossip menu's source is the player. The core only accepts a pick from it when the
        // menu id matches the one it was sent with.
        constexpr uint32 GOSSIP_MENU_TOWN_GUIDE = 9500500;

        // npc_text rows from data/sql/db-world: the text at the top of each kind of page.
        constexpr uint32 NPC_TEXT_MAIN = 9500500;
        constexpr uint32 NPC_TEXT_RESULTS = 9500501;
        constexpr uint32 NPC_TEXT_NO_RESULTS = 9500502;

        constexpr uint32 MAP_EBON_HOLD = 609;

        // Flags a spawn can have without being someone you'd go and talk to.
        constexpr uint32 NOT_A_SERVICE = UNIT_NPC_FLAG_SPELLCLICK | UNIT_NPC_FLAG_PLAYER_VEHICLE;

        // A search tests every word against every place; nobody types more than a few words, or
        // searches (in the gossip menu) more than once a second. The addon's searches share the
        // journal's request budget.
        constexpr std::size_t MAX_SEARCH_LENGTH = 64;
        constexpr std::size_t MAX_SEARCH_WORDS = 6;
        constexpr uint32 SEARCH_COOLDOWN_MS = 1000;

        struct DirectoryConfig
        {
            uint32 spellId = 90050;
            bool learnSpell = true;
            uint32 resultsPerPage = 10;
            uint32 maxResults = 60;
            bool saveZoneData = true;
        };

        DirectoryConfig sDirectoryConfig;

        // Gossip menu senders: which part of the menu a pick came from. The action says what.
        enum Sender : uint32
        {
            SENDER_MAIN        = 1, // action: a Submenu
            SENDER_CATEGORY    = 2, // action: a Category
            SENDER_PROFESSION  = 3, // action: a skill line
            SENDER_RESULT      = 4, // action: index into the player's current results
            SENDER_PAGE        = 5, // action: page number
            SENDER_SEARCH      = 6, // the search box
        };

        enum Submenu : uint32
        {
            SUBMENU_MAIN        = 0,
            SUBMENU_PROFESSIONS = 1,
            SUBMENU_MORE        = 2,
        };

        enum class TrainerKind : uint8
        {
            None,
            Class,        // value: class id
            Profession,   // value: skill line
            Riding,
            WeaponMaster,
            Pet,
        };

        // The addon's DirectoryTab.lua uses the same numbers.
        enum Category : uint32
        {
            CATEGORY_CLASS_TRAINER = 1,
            CATEGORY_RIDING,
            CATEGORY_WEAPON_MASTER,
            CATEGORY_PET_TRAINER,
            CATEGORY_FLIGHT_MASTER,
            CATEGORY_INNKEEPER,
            CATEGORY_BANK,
            CATEGORY_AUCTION_HOUSE,
            CATEGORY_MAILBOX,
            CATEGORY_REPAIR,
            CATEGORY_STABLE_MASTER,
            CATEGORY_GUILD_MASTER,
            CATEGORY_TABARD,
            CATEGORY_GUILD_VAULT,
            CATEGORY_BATTLEMASTER,
            CATEGORY_BARBER,
            CATEGORY_REAGENTS,
            CATEGORY_FOOD,
            CATEGORY_GENERAL_GOODS,
            CATEGORY_POISONS,
        };

        struct CategoryDef
        {
            Category id;
            char const* label;
            uint8 icon;
            uint32 npcflag;       // matches creatures with this flag
            uint32 goType;        // or game objects of this type (0 = none)
            TrainerKind trainer;  // or trainers of this kind
            bool mainMenu;        // on the gossip menu's first page, not under "More"
            uint8 onlyClass;      // shown only to this class (0 = everyone)
        };

        // Class trainers and profession trainers get their own entries on the main menu.
        constexpr std::array<CategoryDef, 19> CATEGORIES = {{
            { CATEGORY_FLIGHT_MASTER, "Flight master",   GOSSIP_ICON_TAXI,       UNIT_NPC_FLAG_FLIGHTMASTER,   0,                            TrainerKind::None,         true,  0 },
            { CATEGORY_INNKEEPER,     "Innkeeper",       GOSSIP_ICON_INTERACT_1, UNIT_NPC_FLAG_INNKEEPER,      0,                            TrainerKind::None,         true,  0 },
            { CATEGORY_BANK,          "Bank",            GOSSIP_ICON_MONEY_BAG,  UNIT_NPC_FLAG_BANKER,         0,                            TrainerKind::None,         true,  0 },
            { CATEGORY_AUCTION_HOUSE, "Auction house",   GOSSIP_ICON_MONEY_BAG,  UNIT_NPC_FLAG_AUCTIONEER,     0,                            TrainerKind::None,         true,  0 },
            { CATEGORY_MAILBOX,       "Mailbox",         GOSSIP_ICON_INTERACT_1, UNIT_NPC_FLAG_MAILBOX,        GAMEOBJECT_TYPE_MAILBOX,      TrainerKind::None,         true,  0 },
            { CATEGORY_REPAIR,        "Repairs",         GOSSIP_ICON_VENDOR,     UNIT_NPC_FLAG_REPAIR,         0,                            TrainerKind::None,         true,  0 },
            { CATEGORY_RIDING,        "Riding trainer",  GOSSIP_ICON_TRAINER,    0,                            0,                            TrainerKind::Riding,       false, 0 },
            { CATEGORY_WEAPON_MASTER, "Weapon master",   GOSSIP_ICON_TRAINER,    0,                            0,                            TrainerKind::WeaponMaster, false, 0 },
            { CATEGORY_PET_TRAINER,   "Pet trainer",     GOSSIP_ICON_TRAINER,    0,                            0,                            TrainerKind::Pet,          false, CLASS_HUNTER },
            { CATEGORY_STABLE_MASTER, "Stable master",   GOSSIP_ICON_INTERACT_1, UNIT_NPC_FLAG_STABLEMASTER,   0,                            TrainerKind::None,         false, CLASS_HUNTER },
            { CATEGORY_GUILD_MASTER,  "Guild master",    GOSSIP_ICON_TABARD,     UNIT_NPC_FLAG_PETITIONER,     0,                            TrainerKind::None,         false, 0 },
            { CATEGORY_TABARD,        "Tabard designer", GOSSIP_ICON_TABARD,     UNIT_NPC_FLAG_TABARDDESIGNER, 0,                            TrainerKind::None,         false, 0 },
            { CATEGORY_GUILD_VAULT,   "Guild vault",     GOSSIP_ICON_MONEY_BAG,  UNIT_NPC_FLAG_GUILD_BANKER,   GAMEOBJECT_TYPE_GUILD_BANK,   TrainerKind::None,         false, 0 },
            { CATEGORY_BATTLEMASTER,  "Battlemaster",    GOSSIP_ICON_BATTLE,     UNIT_NPC_FLAG_BATTLEMASTER,   0,                            TrainerKind::None,         false, 0 },
            { CATEGORY_BARBER,        "Barber",          GOSSIP_ICON_INTERACT_1, 0,                            GAMEOBJECT_TYPE_BARBER_CHAIR, TrainerKind::None,         false, 0 },
            { CATEGORY_REAGENTS,      "Reagents",        GOSSIP_ICON_VENDOR,     UNIT_NPC_FLAG_VENDOR_REAGENT, 0,                            TrainerKind::None,         false, 0 },
            { CATEGORY_FOOD,          "Food and drink",  GOSSIP_ICON_VENDOR,     UNIT_NPC_FLAG_VENDOR_FOOD,    0,                            TrainerKind::None,         false, 0 },
            { CATEGORY_GENERAL_GOODS, "General goods",   GOSSIP_ICON_VENDOR,     UNIT_NPC_FLAG_VENDOR_AMMO,    0,                            TrainerKind::None,         false, 0 },
            { CATEGORY_POISONS,       "Poisons",         GOSSIP_ICON_VENDOR,     UNIT_NPC_FLAG_VENDOR_POISON,  0,                            TrainerKind::None,         false, CLASS_ROGUE },
        }};

        // Extra words a spawn can be found by, from its npc flags.
        struct FlagTag
        {
            uint32 flag;
            char const* words;
        };

        constexpr std::array<FlagTag, 17> FLAG_TAGS = {{
            { UNIT_NPC_FLAG_QUESTGIVER,       "quest" },
            { UNIT_NPC_FLAG_VENDOR,           "vendor" },
            { UNIT_NPC_FLAG_VENDOR_AMMO,      "general goods ammo" },
            { UNIT_NPC_FLAG_VENDOR_FOOD,      "food drink" },
            { UNIT_NPC_FLAG_VENDOR_POISON,    "poisons" },
            { UNIT_NPC_FLAG_VENDOR_REAGENT,   "reagents" },
            { UNIT_NPC_FLAG_REPAIR,           "repairs" },
            { UNIT_NPC_FLAG_FLIGHTMASTER,     "flight master taxi" },
            { UNIT_NPC_FLAG_SPIRITHEALER,     "spirit healer" },
            { UNIT_NPC_FLAG_INNKEEPER,        "innkeeper inn" },
            { UNIT_NPC_FLAG_BANKER,           "bank banker" },
            { UNIT_NPC_FLAG_PETITIONER,       "guild master charter" },
            { UNIT_NPC_FLAG_TABARDDESIGNER,   "tabard designer" },
            { UNIT_NPC_FLAG_BATTLEMASTER,     "battlemaster battleground" },
            { UNIT_NPC_FLAG_AUCTIONEER,       "auction house auctioneer" },
            { UNIT_NPC_FLAG_STABLEMASTER,     "stable master" },
            { UNIT_NPC_FLAG_GUILD_BANKER,     "guild vault bank" },
        }};

        // Professions in the order the gossip menu lists them.
        constexpr std::array<uint32, 14> PROFESSIONS = {
            SKILL_ALCHEMY, SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING, SKILL_HERBALISM,
            SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_MINING, SKILL_SKINNING,
            SKILL_TAILORING, SKILL_COOKING, SKILL_FIRST_AID, SKILL_FISHING,
        };

        constexpr std::array<char const*, 7> RANK_NAMES = {
            "", "Apprentice", "Journeyman", "Expert", "Artisan", "Master", "Grand Master",
        };

        // ChrClasses.dbc names aren't loaded by the core.
        char const* ClassName(uint8 classId)
        {
            switch (classId)
            {
                case CLASS_WARRIOR:      return "Warrior";
                case CLASS_PALADIN:      return "Paladin";
                case CLASS_HUNTER:       return "Hunter";
                case CLASS_ROGUE:        return "Rogue";
                case CLASS_PRIEST:       return "Priest";
                case CLASS_DEATH_KNIGHT: return "Death Knight";
                case CLASS_SHAMAN:       return "Shaman";
                case CLASS_MAGE:         return "Mage";
                case CLASS_WARLOCK:      return "Warlock";
                case CLASS_DRUID:        return "Druid";
                default:                 return "Class";
            }
        }

        // A creature or game object template that has at least one spawn in the index.
        struct Entry
        {
            bool gameObject = false;
            uint32 id = 0;
            std::string name;
            std::string subname;
            std::string search;       // lower-case name, subname and tags
            uint32 faction = 0;       // creatures only
            uint32 goType = 0;        // game objects only
            TrainerKind trainer = TrainerKind::None;
            uint32 trainerValue = 0;  // class id or skill line
            uint8 trainerRank = 0;    // highest profession rank taught, 1 (Apprentice) to 6 (Grand Master)
        };

        // One spawn.
        struct Place
        {
            uint32 entry = 0;         // index into Data::entries
            uint32 npcflag = 0;
            uint16 map = 0;
            uint32 zone = 0;
            uint32 area = 0;
            float x = 0.0f;
            float y = 0.0f;
            uint32 phaseMask = 0;
            int16 event = 0;          // > 0: only while that event runs; < 0: gone while it runs
        };

        // Built once at startup and never changed afterwards, so map threads can read it freely.
        struct Data
        {
            bool built = false;
            std::vector<Entry> entries;
            std::vector<Place> places;
            std::unordered_map<uint32, std::string> areaNames;       // zone/area id -> name
            std::unordered_map<uint32, std::string> areaNamesLower;
        };

        Data sData;

        // The gossip menu's page, kept on the Player so each map thread only touches its own.
        struct GossipState : public DataMap::Base
        {
            std::vector<uint32> results; // indexes into Data::places
            uint32 page = 0;
            uint32 lastSearch = 0;       // getMSTime() of the last text search
        };

        // Bots are sessions without a socket. AzerothCore marks them with WorldSession::IsHeadless();
        // older playerbots core forks have WorldSession::IsBot() instead, and older stock cores have
        // neither. Looking for both at compile time lets the module build on all of them.
        template <typename Session, typename = void>
        struct HasIsHeadless : std::false_type { };

        template <typename Session>
        struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

        template <typename Session, typename = void>
        struct HasIsBot : std::false_type { };

        template <typename Session>
        struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

        template <typename Session>
        bool IsBotSession(Session* session)
        {
            if constexpr (HasIsHeadless<Session>::value)
                return session->IsHeadless();
            else if constexpr (HasIsBot<Session>::value)
                return session->IsBot();
            else
                return false;
        }

        bool Enabled()
        {
            return GetConfig().enabled && GetConfig().directory && sData.built;
        }

        std::string Lower(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return out;
        }

        // Single letters match nearly everything, so they're dropped; so are repeats.
        std::vector<std::string> Words(std::string_view text)
        {
            if (text.size() > MAX_SEARCH_LENGTH)
            {
                std::size_t cut = MAX_SEARCH_LENGTH;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) // not inside a UTF-8 character
                    --cut;
                text = text.substr(0, cut);
            }

            std::vector<std::string> words;
            std::string lower = Lower(text);
            std::size_t pos = 0;
            while (pos < lower.size() && words.size() < MAX_SEARCH_WORDS)
            {
                while (pos < lower.size() && std::isspace((unsigned char)lower[pos]))
                    ++pos;
                std::size_t end = pos;
                while (end < lower.size() && !std::isspace((unsigned char)lower[end]))
                    ++end;
                std::string word = lower.substr(pos, end - pos);
                if (word.size() > 1 && std::find(words.begin(), words.end(), word) == words.end())
                    words.push_back(std::move(word));
                pos = end;
            }
            return words;
        }

        std::string const& CachedAreaName(uint32 areaId)
        {
            static std::string const unknown = "Unknown";
            auto itr = sData.areaNames.find(areaId);
            return itr != sData.areaNames.end() ? itr->second : unknown;
        }

        void RememberAreaName(uint32 areaId)
        {
            if (!areaId || sData.areaNames.count(areaId))
                return;

            std::string name = Index::AreaName(areaId);
            if (name.empty())
                return;
            sData.areaNamesLower[areaId] = Lower(name);
            sData.areaNames[areaId] = std::move(name);
        }

        std::string ContinentName(uint32 mapId)
        {
            std::string name = Index::MapName(mapId);
            return name.empty() ? "another world" : name;
        }

        // ---- trainers ---------------------------------------------------------------------------

        // Which skill lines a trainer spell belongs to, and which profession rank it teaches.
        void CollectSkills(uint32 spellId, std::map<uint32, uint32>& votes, std::map<uint32, uint8>& ranks, bool followLearn)
        {
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
            if (!spell)
                return;

            SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
            for (auto itr = bounds.first; itr != bounds.second; ++itr)
                ++votes[itr->second->SkillLine];

            for (SpellEffectInfo const& effect : spell->Effects)
            {
                // Apprentice Alchemy and friends: the skill, and a step of 75 points per rank.
                if (effect.Effect == SPELL_EFFECT_SKILL_STEP && effect.MiscValue > 0)
                {
                    uint32 skill = uint32(effect.MiscValue);
                    votes[skill] += 10;
                    ranks[skill] = std::max<uint8>(ranks[skill], uint8(std::clamp(effect.CalcValue(), 0, 6)));
                }
                else if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && followLearn && effect.TriggerSpell)
                    CollectSkills(effect.TriggerSpell, votes, ranks, false);
            }
        }

        bool IsProfessionSkill(uint32 skill)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
            return line && (line->categoryId == SKILL_CATEGORY_PROFESSION || line->categoryId == SKILL_CATEGORY_SECONDARY);
        }

        bool IsWeaponSkill(uint32 skill)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
            return line && line->categoryId == SKILL_CATEGORY_WEAPON;
        }

        // Reads what kind of trainer a creature is from the trainer tables: class trainers carry
        // their class, profession trainers and weapon masters are recognised by the skills their
        // spells belong to.
        void ClassifyTrainer(Entry& entry, uint32 npcflag)
        {
            Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry.id);
            if (!trainer)
            {
                // Since 3.0 pet trainers only reset pet talents, and many have no trainer data left.
                if ((npcflag & UNIT_NPC_FLAG_TRAINER) && Lower(entry.subname) == "pet trainer")
                    entry.trainer = TrainerKind::Pet;
                return;
            }

            std::map<uint32, uint32> votes;
            std::map<uint32, uint8> ranks;
            for (Trainer::Spell const& spell : trainer->GetSpells())
            {
                if (spell.ReqSkillLine)
                    ++votes[spell.ReqSkillLine];
                CollectSkills(spell.SpellId, votes, ranks, true);
            }

            // Weapon masters are stored as class or as tradeskill trainers, depending on the data.
            if (std::any_of(votes.begin(), votes.end(), [](auto const& vote) { return IsWeaponSkill(vote.first); }))
            {
                entry.trainer = TrainerKind::WeaponMaster;
                return;
            }

            switch (trainer->GetTrainerType())
            {
                case Trainer::Type::Class:
                    if (trainer->GetTrainerRequirement())
                    {
                        entry.trainer = TrainerKind::Class;
                        entry.trainerValue = trainer->GetTrainerRequirement();
                    }
                    break;
                case Trainer::Type::Mount:
                    entry.trainer = TrainerKind::Riding;
                    break;
                case Trainer::Type::Pet:
                    entry.trainer = TrainerKind::Pet;
                    break;
                case Trainer::Type::Tradeskill:
                {
                    uint32 best = 0;
                    uint32 bestVotes = 0;
                    for (auto const& [skill, count] : votes)
                        if (count > bestVotes && IsProfessionSkill(skill))
                        {
                            best = skill;
                            bestVotes = count;
                        }

                    if (best)
                    {
                        entry.trainer = TrainerKind::Profession;
                        entry.trainerValue = best;
                        entry.trainerRank = ranks[best];
                    }
                    break;
                }
                default:
                    break;
            }
        }

        std::string SkillName(uint32 skill)
        {
            if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
            {
                char const* name = line->name[sWorld->GetDefaultDbcLocale()];
                if (!name || !*name)
                    name = line->name[LOCALE_enUS];
                if (name && *name)
                    return name;
            }
            return "Profession";
        }

        // ---- building the index -----------------------------------------------------------------

        bool IsWorldMap(uint32 mapId)
        {
            MapEntry const* map = sMapStore.LookupEntry(mapId);
            return map && map->IsWorldMap();
        }

        // Test and placeholder creatures that Blizzard left in the data.
        bool LooksLikePlaceholder(std::string const& name)
        {
            return name.empty() || name.front() == '[' || name.find("DND") != std::string::npos
                || name.find("(PH)") != std::string::npos;
        }

        std::string BuildSearchText(Entry const& entry, uint32 npcflags)
        {
            std::string text = entry.name + " " + entry.subname;

            for (FlagTag const& tag : FLAG_TAGS)
                if (npcflags & tag.flag)
                    text += std::string(" ") + tag.words;

            switch (entry.trainer)
            {
                case TrainerKind::Class:
                    text += std::string(" trainer ") + ClassName(uint8(entry.trainerValue));
                    break;
                case TrainerKind::Profession:
                    text += " trainer " + SkillName(entry.trainerValue);
                    if (entry.trainerRank)
                        text += std::string(" ") + RANK_NAMES[entry.trainerRank];
                    break;
                case TrainerKind::Riding:
                    text += " riding trainer";
                    break;
                case TrainerKind::WeaponMaster:
                    text += " weapon master trainer";
                    break;
                case TrainerKind::Pet:
                    text += " pet trainer";
                    break;
                default:
                    break;
            }

            return Lower(text);
        }

        using SavedZones = std::unordered_map<uint32, std::pair<uint32, uint32>>;

        // Zone and area ids the database already has (the core fills them in when
        // Calculate.Creature.Zone.Area.Data is on, and so does the Directory). The casts pin the
        // column types, which the Field getters check.
        SavedZones LoadSavedZones(char const* table)
        {
            SavedZones zones;
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(guid AS UNSIGNED), CAST(zoneId AS UNSIGNED), "
                "CAST(areaId AS UNSIGNED) FROM {} WHERE zoneId <> 0", table))
                do
                {
                    Field* fields = result->Fetch();
                    zones[uint32(fields[0].Get<uint64>())] = { uint32(fields[1].Get<uint64>()), uint32(fields[2].Get<uint64>()) };
                } while (result->NextRow());
            return zones;
        }

        std::unordered_map<uint32, int16> LoadEvents(char const* table)
        {
            std::unordered_map<uint32, int16> events;
            if (QueryResult result = WorldDatabase.Query("SELECT CAST(guid AS UNSIGNED), CAST(eventEntry AS SIGNED) FROM {}", table))
                do
                {
                    Field* fields = result->Fetch();
                    events[uint32(fields[0].Get<uint64>())] = int16(fields[1].Get<int64>());
                } while (result->NextRow());
            return events;
        }

        // Zone and area for a spawn: saved in the database, or worked out from the map files (and
        // then saved, so the next start is quick). Only safe before the maps start updating.
        std::pair<uint32, uint32> ZoneOf(SpawnData const& data, SavedZones const& saved, char const* table,
            WorldDatabaseTransaction trans, uint32& computed)
        {
            auto itr = saved.find(data.spawnId);
            if (itr != saved.end())
                return itr->second;

            uint32 zone = 0;
            uint32 area = 0;
            sMapMgr->GetZoneAndAreaId(data.phaseMask, zone, area, data.mapid, data.posX, data.posY, data.posZ);
            // A first start loads a lot of terrain here, before the world loop runs: keep the
            // freeze detector from taking it for a hang.
            if (++computed % 50 == 0)
                ++World::m_worldLoopCounter;

            if (sDirectoryConfig.saveZoneData && zone)
                trans->Append("UPDATE {} SET zoneId = {}, areaId = {} WHERE guid = {}", table, zone, area, data.spawnId);

            return { zone, area };
        }

        Place MakePlace(uint32 entry, SpawnData const& data, std::pair<uint32, uint32> zoneArea,
            std::unordered_map<uint32, int16> const& events, ObjectGuid::LowType spawnId)
        {
            Place place;
            place.entry = entry;
            place.map = uint16(data.mapid);
            place.x = data.posX;
            place.y = data.posY;
            place.phaseMask = data.phaseMask;
            std::tie(place.zone, place.area) = zoneArea;
            if (auto event = events.find(spawnId); event != events.end())
                place.event = event->second;
            RememberAreaName(place.zone);
            RememberAreaName(place.area);
            return place;
        }

        // ---- who sees what ----------------------------------------------------------------------

        struct Viewer
        {
            Player* player = nullptr;
            FactionTemplateEntry const* faction = nullptr;
            uint8 era = 18;
            bool everything = false;
        };

        Viewer MakeViewer(Player* player)
        {
            Viewer viewer;
            viewer.player = player;
            viewer.faction = player->GetFactionTemplateEntry();
            viewer.everything = player->IsGameMaster();
            viewer.era = ProgressionState(player);
            return viewer;
        }

        bool IsOpen(Viewer const& viewer, Place const& place)
        {
            if (viewer.everything)
                return true;
            // Only death knights ever stand in Acherus, whatever their era.
            if (place.map == MAP_EBON_HOLD)
                return viewer.player->getClass() == CLASS_DEATH_KNIGHT;
            return Collections::ZoneEra(place.map, place.zone) <= viewer.era;
        }

        bool IsFriendly(Viewer const& viewer, Entry const& entry)
        {
            if (entry.gameObject || !viewer.faction)
                return true;

            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(entry.faction);
            if (!faction)
                return true;

            if (faction->IsHostileTo(*viewer.faction))
                return false;

            // Reputation can make an otherwise friendly faction hostile.
            return viewer.player->GetReputationRank(faction->faction) > REP_HOSTILE;
        }

        bool IsThere(Viewer const& viewer, Place const& place)
        {
            if (!(place.phaseMask & (PHASEMASK_NORMAL | viewer.player->GetPhaseMask())))
                return false;

            if (place.event > 0)
                return sGameEventMgr->IsActiveEvent(uint16(place.event));
            if (place.event < 0)
                return !sGameEventMgr->IsActiveEvent(uint16(-place.event));
            return true;
        }

        bool CanSee(Viewer const& viewer, Place const& place)
        {
            return IsOpen(viewer, place) && IsThere(viewer, place) && IsFriendly(viewer, sData.entries[place.entry]);
        }

        // ---- searching --------------------------------------------------------------------------

        struct Query
        {
            std::vector<std::string> words;         // every word must match
            CategoryDef const* category = nullptr;
            bool classTrainer = false;
            uint32 profession = 0;                  // skill line
        };

        bool Matches(Query const& query, Viewer const& viewer, Place const& place)
        {
            Entry const& entry = sData.entries[place.entry];

            if (query.classTrainer)
                return entry.trainer == TrainerKind::Class && entry.trainerValue == viewer.player->getClass();

            if (query.profession)
                return entry.trainer == TrainerKind::Profession && entry.trainerValue == query.profession;

            if (CategoryDef const* category = query.category)
            {
                if (category->trainer != TrainerKind::None)
                    return entry.trainer == category->trainer;
                if (entry.gameObject)
                    return category->goType && entry.goType == category->goType;
                return category->npcflag && (place.npcflag & category->npcflag);
            }

            for (std::string const& word : query.words)
            {
                if (entry.search.find(word) != std::string::npos)
                    continue;

                auto zone = sData.areaNamesLower.find(place.zone);
                if (zone != sData.areaNamesLower.end() && zone->second.find(word) != std::string::npos)
                    continue;

                auto area = sData.areaNamesLower.find(place.area);
                if (area != sData.areaNamesLower.end() && area->second.find(word) != std::string::npos)
                    continue;

                return false;
            }

            return !query.words.empty();
        }

        // Places (indexes into Data::places) for a query, at most MaxResults; found is how many
        // there were before the cut.
        std::vector<uint32> Search(Player* player, Query const& query, uint32& found)
        {
            Viewer viewer = MakeViewer(player);

            struct Hit
            {
                uint32 place;
                bool sameMap;
                float distance;
            };

            // The nearest spawn of each NPC in each zone, so a wandering guard shows up once.
            std::map<std::pair<uint32, uint32>, Hit> nearest;
            for (uint32 i = 0; i < sData.places.size(); ++i)
            {
                Place const& place = sData.places[i];
                if (!Matches(query, viewer, place) || !CanSee(viewer, place))
                    continue;

                bool sameMap = place.map == player->GetMapId();
                float distance = sameMap ? player->GetExactDist2d(place.x, place.y) : 0.0f;

                auto key = std::make_pair(place.entry, place.zone);
                auto itr = nearest.find(key);
                if (itr == nearest.end() || (sameMap && distance < itr->second.distance))
                    nearest[key] = { i, sameMap, distance };
            }

            std::vector<Hit> hits;
            hits.reserve(nearest.size());
            for (auto const& [key, hit] : nearest)
                hits.push_back(hit);

            // Your own continent first, nearest first; then the rest by continent and zone.
            std::sort(hits.begin(), hits.end(), [](Hit const& a, Hit const& b)
            {
                if (a.sameMap != b.sameMap)
                    return a.sameMap;
                if (a.sameMap)
                    return a.distance < b.distance;

                Place const& pa = sData.places[a.place];
                Place const& pb = sData.places[b.place];
                if (pa.map != pb.map)
                    return pa.map < pb.map;
                if (pa.zone != pb.zone)
                    return CachedAreaName(pa.zone) < CachedAreaName(pb.zone);
                return sData.entries[pa.entry].name < sData.entries[pb.entry].name;
            });

            found = uint32(hits.size());
            if (hits.size() > sDirectoryConfig.maxResults)
                hits.resize(sDirectoryConfig.maxResults);

            std::vector<uint32> results;
            results.reserve(hits.size());
            for (Hit const& hit : hits)
                results.push_back(hit.place);
            return results;
        }

        CategoryDef const* FindCategory(uint32 id)
        {
            for (CategoryDef const& category : CATEGORIES)
                if (category.id == id)
                    return &category;
            return nullptr;
        }

        // ---- describing a place -----------------------------------------------------------------

        // Map coordinates as the world map shows them, or false if the zone has no map of its own.
        bool ZoneCoordinates(Place const& place, float& x, float& y)
        {
            x = place.x;
            y = place.y;
            Map2ZoneCoordinates(x, y, place.zone);
            return place.zone && x >= 0.0f && x <= 100.0f && y >= 0.0f && y <= 100.0f
                && !(x == place.x && y == place.y);
        }

        std::string Where(Place const& place)
        {
            std::string where = CachedAreaName(place.zone);
            if (place.area && place.area != place.zone)
                where += ", " + CachedAreaName(place.area);

            float x;
            float y;
            if (ZoneCoordinates(place, x, y))
                where += Acore::StringFormat(" ({:.0f}, {:.0f})", x, y);

            return where;
        }

        constexpr std::array<char const*, 8> DIRECTIONS = {
            "north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west",
        };

        uint32 DirectionIndex(Player* player, Place const& place)
        {
            // World x points north and world y points west.
            float north = place.x - player->GetPositionX();
            float east = player->GetPositionY() - place.y;
            float degrees = std::atan2(east, north) * 180.0f / float(M_PI);
            if (degrees < 0.0f)
                degrees += 360.0f;

            return uint32(std::lround(degrees / 45.0f)) % DIRECTIONS.size();
        }

        std::string Title(Entry const& entry)
        {
            std::string title = entry.name;
            if (!entry.subname.empty())
                title += " <" + entry.subname + ">";
            return title;
        }

        // The chat line a picked place gets, from the gossip menu and from the addon's map flag.
        std::string PlaceLine(Player* player, Place const& place)
        {
            Entry const& entry = sData.entries[place.entry];
            if (place.map == player->GetMapId())
                return Acore::StringFormat("{} - {}, {} yards {}", Title(entry), Where(place),
                    uint32(player->GetExactDist2d(place.x, place.y)), DIRECTIONS[DirectionIndex(player, place)]);
            return Acore::StringFormat("{} is in {}: {}", Title(entry), ContinentName(place.map), Where(place));
        }

        std::string Row(Player* player, uint32 index)
        {
            Place const& place = sData.places[index];
            Entry const& entry = sData.entries[place.entry];

            float x = -0.1f;
            float y = -0.1f;
            if (!ZoneCoordinates(place, x, y))
                x = y = -0.1f;

            bool here = place.map == player->GetMapId();
            int32 yards = here ? int32(player->GetExactDist2d(place.x, place.y)) : -1;
            uint32 direction = here ? DirectionIndex(player, place) : 8;

            return Acore::StringFormat("{},{},{},{},{},{},{},{},{},{},{},{},{}",
                index, entry.gameObject ? 'G' : 'C', entry.id,
                Escape(entry.name, 36), Escape(entry.subname, 32),
                Escape(place.zone ? CachedAreaName(place.zone) : std::string(), 28),
                Escape(place.area && place.area != place.zone ? CachedAreaName(place.area) : std::string(), 28),
                int32(std::lround(x * 10.0f)), int32(std::lround(y * 10.0f)),
                here ? std::string("-") : Escape(ContinentName(place.map), 20),
                yards, direction, uint32(entry.trainerRank));
        }

        // ---- the gossip menu (no addon) ---------------------------------------------------------

        GossipState& GetState(Player* player)
        {
            return *player->CustomData.GetDefault<GossipState>("mod-forever-journal-directory");
        }

        void StartMenu(Player* player)
        {
            ClearGossipMenuFor(player);
            player->PlayerTalkClass->GetGossipMenu().SetMenuId(GOSSIP_MENU_TOWN_GUIDE);
        }

        void AddSearchBox(Player* player, char const* label)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, SENDER_SEARCH, 0,
                "Type a name, a title such as \"mage trainer\", or a zone.", 0, true);
        }

        void ShowMainMenu(Player* player)
        {
            StartMenu(player);
            AddSearchBox(player, "Search by name, title or zone...");
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, std::string(ClassName(player->getClass())) + " trainer", SENDER_CATEGORY, CATEGORY_CLASS_TRAINER);
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Profession trainers...", SENDER_MAIN, SUBMENU_PROFESSIONS);

            for (CategoryDef const& category : CATEGORIES)
                if (category.mainMenu && (!category.onlyClass || category.onlyClass == player->getClass()))
                    AddGossipItemFor(player, category.icon, category.label, SENDER_CATEGORY, category.id);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "More...", SENDER_MAIN, SUBMENU_MORE);
            SendGossipMenuFor(player, NPC_TEXT_MAIN, player->GetGUID());
        }

        void ShowMoreMenu(Player* player)
        {
            StartMenu(player);
            for (CategoryDef const& category : CATEGORIES)
                if (!category.mainMenu && (!category.onlyClass || category.onlyClass == player->getClass()))
                    AddGossipItemFor(player, category.icon, category.label, SENDER_CATEGORY, category.id);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", SENDER_MAIN, SUBMENU_MAIN);
            SendGossipMenuFor(player, NPC_TEXT_MAIN, player->GetGUID());
        }

        void ShowProfessionMenu(Player* player)
        {
            StartMenu(player);
            for (uint32 skill : PROFESSIONS)
            {
                std::string label = SkillName(skill);
                if (uint16 max = player->GetMaxSkillValue(skill))
                    label += Acore::StringFormat(" (yours: {})", RANK_NAMES[std::clamp<uint32>(max / 75, 1, 6)]);
                AddGossipItemFor(player, GOSSIP_ICON_TRAINER, label, SENDER_PROFESSION, skill);
            }

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", SENDER_MAIN, SUBMENU_MAIN);
            SendGossipMenuFor(player, NPC_TEXT_MAIN, player->GetGUID());
        }

        std::string ResultText(Player* player, Place const& place)
        {
            Entry const& entry = sData.entries[place.entry];
            std::string text = Title(entry) + "\n";

            if (place.map == player->GetMapId())
            {
                text += Where(place);
                text += Acore::StringFormat(" - {} yd", uint32(player->GetExactDist2d(place.x, place.y)));
            }
            else
                text += ContinentName(place.map) + ": " + Where(place);

            if (entry.trainer == TrainerKind::Profession && entry.trainerRank)
                text += std::string(" - up to ") + RANK_NAMES[entry.trainerRank];

            return text;
        }

        void ShowResultsPage(Player* player)
        {
            GossipState& state = GetState(player);
            StartMenu(player);

            if (state.results.empty())
            {
                AddSearchBox(player, "Search again...");
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to the menu", SENDER_MAIN, SUBMENU_MAIN);
                SendGossipMenuFor(player, NPC_TEXT_NO_RESULTS, player->GetGUID());
                return;
            }

            uint32 perPage = sDirectoryConfig.resultsPerPage;
            uint32 pages = (uint32(state.results.size()) + perPage - 1) / perPage;
            state.page = std::min(state.page, pages - 1);

            uint32 first = state.page * perPage;
            uint32 last = std::min<uint32>(first + perPage, uint32(state.results.size()));
            for (uint32 i = first; i < last; ++i)
                AddGossipItemFor(player, GOSSIP_ICON_DOT, ResultText(player, sData.places[state.results[i]]), SENDER_RESULT, i);

            if (state.page + 1 < pages)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("Next page ({}/{})", state.page + 2, pages), SENDER_PAGE, state.page + 1);
            if (state.page > 0)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous page", SENDER_PAGE, state.page - 1);

            AddSearchBox(player, "New search...");
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to the menu", SENDER_MAIN, SUBMENU_MAIN);
            SendGossipMenuFor(player, NPC_TEXT_RESULTS, player->GetGUID());
        }

        void RunQuery(Player* player, Query const& query)
        {
            GossipState& state = GetState(player);
            uint32 found = 0;
            state.results = Search(player, query, found);
            state.page = 0;
            ShowResultsPage(player);
        }

        void SearchText(Player* player, std::string_view text)
        {
            Query query;
            query.words = Words(text);
            if (query.words.empty())
            {
                if (text.find_first_not_of(" \t\r\n") != std::string_view::npos)
                    ChatHandler(player->GetSession()).SendSysMessage("Search with words of at least two letters.");
                ShowMainMenu(player);
                return;
            }

            GossipState& state = GetState(player);
            uint32 now = getMSTime();
            if (state.lastSearch && getMSTimeDiff(state.lastSearch, now) < SEARCH_COOLDOWN_MS)
            {
                ChatHandler(player->GetSession()).SendSysMessage("Wait a moment before searching again.");
                return;
            }
            state.lastSearch = now;

            RunQuery(player, query);
        }

        void ShowPlace(Player* player, Place const& place)
        {
            Entry const& entry = sData.entries[place.entry];
            Index::SendPoi(player, place.map, place.x, place.y, entry.name);
            ChatHandler(player->GetSession()).PSendSysMessage("|cffffd000Town Guide:|r {}.{}", PlaceLine(player, place),
                place.map == player->GetMapId() ? " Marked on your map." : "");
        }

        void HandleMenuPick(Player* player, uint32 sender, uint32 action)
        {
            switch (sender)
            {
                case SENDER_MAIN:
                    if (action == SUBMENU_PROFESSIONS)
                        ShowProfessionMenu(player);
                    else if (action == SUBMENU_MORE)
                        ShowMoreMenu(player);
                    else
                        ShowMainMenu(player);
                    break;
                case SENDER_CATEGORY:
                {
                    Query query;
                    if (action == CATEGORY_CLASS_TRAINER)
                        query.classTrainer = true;
                    else if (!(query.category = FindCategory(action)))
                    {
                        ShowMainMenu(player);
                        break;
                    }
                    RunQuery(player, query);
                    break;
                }
                case SENDER_PROFESSION:
                {
                    Query query;
                    query.profession = action;
                    RunQuery(player, query);
                    break;
                }
                case SENDER_RESULT:
                {
                    GossipState const& state = GetState(player);
                    CloseGossipMenuFor(player);
                    if (action < state.results.size())
                        ShowPlace(player, sData.places[state.results[action]]);
                    break;
                }
                case SENDER_PAGE:
                    GetState(player).page = action;
                    ShowResultsPage(player);
                    break;
                case SENDER_SEARCH: // the search box sent back empty
                    ShowMainMenu(player);
                    break;
                default:
                    CloseGossipMenuFor(player);
                    break;
            }
        }

        // The Town Guide spell and .find: the addon's tab when it can open it, else the menu.
        void Open(Player* player, std::string_view text)
        {
            if (!Enabled())
            {
                ChatHandler(player->GetSession()).SendSysMessage("The Town Guide is turned off on this server (turning it on takes a restart).");
                return;
            }

            if (AddonFeatures(player) & ADDON_OPEN)
            {
                std::string message = "OPEN:0:directory";
                if (text.find_first_not_of(" \t\r\n") != std::string_view::npos)
                    message += ":" + Escape(text, MAX_SEARCH_LENGTH);
                Send(player, message);
                return;
            }

            if (text.empty())
                ShowMainMenu(player);
            else
                SearchText(player, text);
        }
    }

    void LoadConfig()
    {
        Config& config = GetConfig();
        config.directory = sConfigMgr->GetOption<bool>("Journal.Directory.Enable", true);
        sDirectoryConfig.spellId = sConfigMgr->GetOption<uint32>("Journal.Directory.SpellId", 90050);
        sDirectoryConfig.learnSpell = sConfigMgr->GetOption<bool>("Journal.Directory.LearnSpell", true);
        sDirectoryConfig.resultsPerPage = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Journal.Directory.ResultsPerPage", 10), 1, 25);
        sDirectoryConfig.maxResults = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Journal.Directory.MaxResults", 60), 1, 200);
        sDirectoryConfig.saveZoneData = sConfigMgr->GetOption<bool>("Journal.Directory.SaveZoneData", true);
    }

    bool Ready()
    {
        return Enabled();
    }

    void Build()
    {
        uint32 start = getMSTime();
        sData = Data();

        SavedZones creatureZones = LoadSavedZones("creature");
        SavedZones objectZones = LoadSavedZones("gameobject");
        std::unordered_map<uint32, int16> creatureEvents = LoadEvents("game_event_creature");
        std::unordered_map<uint32, int16> objectEvents = LoadEvents("game_event_gameobject");

        WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
        uint32 computed = 0;

        std::unordered_map<uint32, uint32> creatureEntries; // template entry -> index into entries
        std::unordered_map<uint32, uint32> objectEntries;

        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (!IsWorldMap(data.mapid))
                continue;

            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data.id);
            if (!creature || (creature->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER) || LooksLikePlaceholder(creature->Name))
                continue;

            uint32 npcflag = 0;
            uint32 unitFlags = 0;
            uint32 dynamicFlags = 0;
            ObjectMgr::ChooseCreatureFlags(creature, npcflag, unitFlags, dynamicFlags, &data);
            if (!(npcflag & ~NOT_A_SERVICE) || (unitFlags & UNIT_FLAG_NOT_SELECTABLE))
                continue;

            auto [itr, isNew] = creatureEntries.try_emplace(data.id, uint32(sData.entries.size()));
            if (isNew)
            {
                Entry entry;
                entry.id = data.id;
                entry.name = creature->Name;
                entry.subname = creature->SubName;
                entry.faction = creature->faction;
                ClassifyTrainer(entry, npcflag);
                entry.search = BuildSearchText(entry, npcflag);
                sData.entries.push_back(std::move(entry));
            }

            Place place = MakePlace(itr->second, data, ZoneOf(data, creatureZones, "creature", trans, computed), creatureEvents, spawnId);
            place.npcflag = npcflag;
            sData.places.push_back(place);
        }

        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        {
            if (!IsWorldMap(data.mapid))
                continue;

            GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(data.id);
            if (!object || LooksLikePlaceholder(object->name))
                continue;

            if (object->type != GAMEOBJECT_TYPE_MAILBOX && object->type != GAMEOBJECT_TYPE_BARBER_CHAIR
                && object->type != GAMEOBJECT_TYPE_GUILD_BANK)
                continue;

            auto [itr, isNew] = objectEntries.try_emplace(data.id, uint32(sData.entries.size()));
            if (isNew)
            {
                Entry entry;
                entry.gameObject = true;
                entry.id = data.id;
                entry.name = object->name;
                entry.goType = object->type;
                entry.search = Lower(entry.name + (object->type == GAMEOBJECT_TYPE_MAILBOX ? " mail"
                    : object->type == GAMEOBJECT_TYPE_BARBER_CHAIR ? " barber" : " guild vault bank"));
                sData.entries.push_back(std::move(entry));
            }

            sData.places.push_back(MakePlace(itr->second, data, ZoneOf(data, objectZones, "gameobject", trans, computed), objectEvents, spawnId));
        }

        if (trans->GetSize())
            WorldDatabase.CommitTransaction(trans);

        sData.built = true;
        LOG_INFO("server.loading", ">> mod-forever-journal: directory of {} places ({} kinds of NPC and object); worked out {} zones in {} ms",
            sData.places.size(), sData.entries.size(), computed, GetMSTimeDiffToNow(start));
    }

    // N:<req>:C:<category>, N:<req>:P:<skill> or N:<req>:S:<text>
    void HandleList(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        if (!sData.built || args.size() < 4)
        {
            SendError(player, req, "bad");
            return;
        }

        Query query;
        uint32 value = 0;
        if (args[2] == "C" && ParseUInt(args[3], value))
        {
            if (value == CATEGORY_CLASS_TRAINER)
                query.classTrainer = true;
            else if (!(query.category = FindCategory(value)))
            {
                SendError(player, req, "bad");
                return;
            }
        }
        else if (args[2] == "P" && ParseUInt(args[3], value)
            && std::find(PROFESSIONS.begin(), PROFESSIONS.end(), value) != PROFESSIONS.end())
            query.profession = value;
        else if (args[2] == "S")
        {
            // The addon swaps ':' for spaces, but a search may still arrive in pieces.
            std::string text;
            for (std::size_t i = 3; i < args.size(); ++i)
                text += std::string(args[i]) + " ";
            query.words = Words(text);
            if (query.words.empty())
            {
                SendError(player, req, "bad");
                return;
            }
        }
        else
        {
            SendError(player, req, "bad");
            return;
        }

        uint32 found = 0;
        std::vector<uint32> results = Search(player, query, found);

        // A row fits one message unless a dozen of its characters need escaping (SendRows would
        // drop it); no service NPC's name and title come close.
        std::vector<std::string> rows;
        rows.reserve(results.size());
        for (uint32 index : results)
            rows.push_back(Row(player, index));

        Send(player, Acore::StringFormat("NR:{}:{}:{}", req, rows.size(), found));
        SendRows(player, "ND:" + req, rows);
        Send(player, "NE:" + req);
    }

    // M:<req>:N:<place>:<id>: the id is the creature or object entry the row showed, so a place
    // number from before a restart can't flag someone else.
    void HandleMapPin(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint32 index = 0;
        uint32 id = 0;
        if (!sData.built || args.size() < 5 || !ParseUInt(args[3], index) || !ParseUInt(args[4], id)
            || index >= sData.places.size())
        {
            SendError(player, req, "none");
            return;
        }

        Place const& place = sData.places[index];
        Entry const& entry = sData.entries[place.entry];
        if (entry.id != id || !CanSee(MakeViewer(player), place))
        {
            SendError(player, req, "none");
            return;
        }

        Index::SendPoi(player, place.map, place.x, place.y, entry.name);
        Send(player, "M:" + req + ":ok:" + Escape(PlaceLine(player, place), 160));
    }
}

using namespace Journal;

class JournalDirectoryPlayerScript : public PlayerScript
{
public:
    JournalDirectoryPlayerScript() : PlayerScript("JournalDirectoryPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_SPELL_CAST,
        PLAYERHOOK_ON_GOSSIP_SELECT,
        PLAYERHOOK_ON_GOSSIP_SELECT_CODE,
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!Directory::Enabled() || !Directory::sDirectoryConfig.learnSpell || Directory::IsBotSession(player->GetSession()))
            return;

        uint32 spell = Directory::sDirectoryConfig.spellId;
        if (spell && !player->HasSpell(spell) && sSpellMgr->GetSpellInfo(spell))
            player->learnSpell(spell);
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        uint32 id = Directory::sDirectoryConfig.spellId;
        if (id && spell->GetSpellInfo()->Id == id && GetConfig().enabled && GetConfig().directory)
            Directory::Open(player, {});
    }

    void OnPlayerGossipSelect(Player* player, uint32 menuId, uint32 sender, uint32 action) override
    {
        if (menuId != Directory::GOSSIP_MENU_TOWN_GUIDE)
            return;

        if (Directory::Enabled())
            Directory::HandleMenuPick(player, sender, action);
        else
            CloseGossipMenuFor(player);
    }

    void OnPlayerGossipSelectCode(Player* player, uint32 menuId, uint32 sender, uint32 /*action*/, char const* code) override
    {
        if (menuId != Directory::GOSSIP_MENU_TOWN_GUIDE)
            return;

        if (Directory::Enabled() && sender == Directory::SENDER_SEARCH && code)
            Directory::SearchText(player, code);
        else
            CloseGossipMenuFor(player);
    }
};

class JournalDirectoryCommandScript : public CommandScript
{
public:
    JournalDirectoryCommandScript() : CommandScript("JournalDirectoryCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "find", HandleFindCommand, SEC_PLAYER, Console::No },
        };

        return commandTable;
    }

    // .find opens the Directory; .find <words> searches straight away.
    static bool HandleFindCommand(ChatHandler* handler, Tail text)
    {
        if (Player* player = handler->GetPlayer())
            Directory::Open(player, std::string_view(text));
        return true;
    }
};

namespace Journal::Directory
{
    void AddScripts()
    {
        new JournalDirectoryPlayerScript();
        new JournalDirectoryCommandScript();
    }
}
