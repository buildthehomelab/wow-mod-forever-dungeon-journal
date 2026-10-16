/*
 * mod-forever-journal: the Lore tab. A codex of the world's lore that fills in as a character
 * plays: walking into a zone or one of its places, talking to someone, defeating someone, or
 * taking and finishing quests unlocks entries. Each zone has an overview, its places, its people
 * and its history.
 *
 * Entries live in the world database:
 *
 *   mod_forever_journal_lore          one entry: its zone, kind (0 zone, 1 place, 2 person,
 *                                     3 history), who sees it (faction, races, classes), the
 *                                     progression stage it opens at, title and text
 *   mod_forever_journal_lore_trigger  what unlocks it: 1 an area (by id, or by name inside the
 *                                     entry's zone), 2 talking to a creature, 3 killing one,
 *                                     4 accepting a quest, 5 turning one in
 *
 * and each character's discoveries in mod_forever_journal_lore_known (characters database).
 *
 * Areas unlock the way the game's own exploration does: alive and not on a flight path. An area
 * reached in flight is checked again once the character has landed (the game reports no area change
 * on landing), and so is the area a character logs in to. At login whatever a character has already
 * explored, and the quests it has taken or done, are credited without a word, so characters older
 * than the module don't start from nothing.
 *
 * With mod-individual-progression, an entry opens at its own Era, or at the stage its areas'
 * zones open (Outland and Northrend), whichever is later; until then it can't be discovered.
 *
 * Threads: the hooks run on map threads (areas, kills, gossip, quests) or the world thread
 * (login, addon requests, commands, the landing check), which never overlap. A character's
 * discoveries are kept on the Player (CustomData), and the lore itself only changes from
 * .journal lore reload on the world thread. Only the list of characters waiting to land is shared
 * between map threads, under a mutex.
 *
 *   Y:<req>          -> YR:<req>:<known>:<total>:<zone you're in>, YD rows, YE
 *                       Z,<zone>,<group>,<known>,<total>,<era>,<name>
 *   U:<req>:<zone>   -> UR:<req>:<zone>:<known>:<total>, UD rows, UE
 *                       E,<entry>,<kind>,<state>,<era>,<title>,<hint>
 *                       state 0 undiscovered, 1 known (credited at login), 2 discovered while
 *                       playing; hint (undiscovered only) says how it is found
 *   V:<req>:<entry>  -> VR:<req>:<entry>:<zone>:<kind>:<title>, VD rows (the text, in pieces), VE
 *
 * Sent unasked when a character discovers an entry while playing:
 *
 *   LN:0:<entry>:<zone>:<kind>:<title>
 *
 * The Library: every book a character reads (a book, plaque or tablet in the world, or a letter or
 * note in its bags: anything with page text) goes on its shelf to be read again from the tab. A
 * book is its first page; the same book in several places counts once.
 *
 *   J:<req>          -> JR:<req>:<books>, JD rows, JE
 *                       B,<book>,<time read>,<title>,<where it was read>
 *   P:<req>:<book>   -> PR:<req>:<book>:<pages>:<title>, PD rows "<page>.<piece>" (the pages'
 *                       text in pieces, each row one piece), PE
 *   BN:0:<book>:<title>   sent unasked when a book goes on the shelf
 *
 * Released under the MIT License.
 */

#include "Journal.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace Journal::Lore
{
    namespace
    {
        enum TriggerType : uint8
        {
            TRIGGER_AREA         = 1,
            TRIGGER_TALK         = 2,
            TRIGGER_KILL         = 3,
            TRIGGER_QUEST_ACCEPT = 4,
            TRIGGER_QUEST_DONE   = 5,
        };

        enum EntryKind : uint8
        {
            KIND_ZONE    = 0,
            KIND_PLACE   = 1,
            KIND_PERSON  = 2,
            KIND_HISTORY = 3,
            KIND_COUNT   = 4,
        };

        enum EntryState : uint8
        {
            STATE_UNKNOWN = 0,
            STATE_KNOWN   = 1, // credited at login
            STATE_FOUND   = 2, // discovered while playing
        };

        enum LoreFaction : uint8
        {
            FACTION_BOTH     = 0,
            FACTION_ALLIANCE = 1,
            FACTION_HORDE    = 2,
        };

        // The text goes out in pieces of at most this many bytes once escaped.
        constexpr std::size_t TEXT_PIECE = 200;

        // A book's pages: more than this is a loop in page_text.
        constexpr uint32 MAX_PAGES = 100;

        // How often characters who were flying (or just logged in) are checked for having landed.
        constexpr uint32 LANDING_CHECK_MS = 1000;

        struct Trigger
        {
            uint8 type = 0;
            uint32 value = 0;
            uint32 faction = 0; // talk and kill: the creature's faction template
        };

        struct Entry
        {
            uint32 id = 0;
            uint32 zone = 0;
            uint8 kind = KIND_PLACE;
            uint16 sort = 0;
            uint8 faction = FACTION_BOTH;
            uint32 races = 0;   // race mask, 0 = every race
            uint32 classes = 0; // class mask, 0 = every class
            uint8 era = 0;
            std::string title;
            std::string body;
            std::string hint;   // how it's found, for the locked placeholder
            std::vector<Trigger> triggers;
        };

        struct Zone
        {
            uint32 id = 0;
            char group = 'X';
            uint8 era = 0;
            std::string name;
            std::vector<std::size_t> entries; // by kind, then sort order
        };

        // Rebuilt by Load(); only the world thread does that, and never while maps update.
        struct Data
        {
            bool ready = false;
            std::vector<Entry> entries;
            std::unordered_map<uint32, std::size_t> byId;
            std::unordered_map<uint64, std::vector<std::size_t>> byTrigger;
            std::vector<Zone> zones;
            std::unordered_map<uint32, std::size_t> zoneById;
            std::vector<uint32> areaTriggers;          // every area id that unlocks something
            std::vector<Trigger> questTriggers;        // every quest trigger, for the login catch-up
            std::unordered_map<uint32, std::string> books; // first page -> title
        };

        Data sData;
        bool sEnabled = true;

        // A character's discoveries: entry -> unix time found, 0 when credited at login.
        struct BookRead
        {
            uint32 time = 0;
            uint32 zone = 0;
        };

        struct KnownState : public DataMap::Base
        {
            bool loaded = false;
            std::unordered_map<uint32, uint32> known;
            std::unordered_map<uint32, BookRead> books; // the Library: first page -> when and where
        };

        // Characters whose area wasn't credited because they were flying, dead or just logging in:
        // checked from the world thread until they're on the ground.
        std::mutex sLandingLock;
        std::unordered_set<ObjectGuid::LowType> sLanding;

        void WaitForLanding(Player* player)
        {
            std::lock_guard<std::mutex> lock(sLandingLock);
            sLanding.insert(player->GetGUID().GetCounter());
        }

        uint64 Key(uint8 type, uint32 value)
        {
            return (uint64(type) << 32) | value;
        }

        KnownState& StateOf(Player* player)
        {
            return *player->CustomData.GetDefault<KnownState>("mod-forever-journal-lore");
        }

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

        // The character can find it: its faction, race and class, and at least one trigger it can
        // reach (a quest only some races or classes are given, a creature hostile to it to talk to
        // or friendly to it to kill, can't unlock it).
        bool Visible(Player* player, Entry const& e)
        {
            if (e.faction == FACTION_ALLIANCE && player->GetTeamId() != TEAM_ALLIANCE)
                return false;
            if (e.faction == FACTION_HORDE && player->GetTeamId() != TEAM_HORDE)
                return false;
            if (e.races && !(e.races & player->getRaceMask()))
                return false;
            if (e.classes && !(e.classes & player->getClassMask()))
                return false;

            FactionTemplateEntry const* mine = player->GetFactionTemplateEntry();
            for (Trigger const& t : e.triggers)
            {
                if (t.type == TRIGGER_AREA)
                    return true;
                if (t.type == TRIGGER_TALK || t.type == TRIGGER_KILL)
                {
                    // Nobody talks to someone hostile to them, or kills someone friendly.
                    FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(t.faction);
                    if (!theirs || !mine)
                        return true;
                    if (t.type == TRIGGER_TALK ? !theirs->IsHostileTo(*mine) : !theirs->IsFriendlyTo(*mine))
                        return true;
                    continue;
                }
                Quest const* quest = sObjectMgr->GetQuestTemplate(t.value);
                if (!quest)
                    continue;
                if (quest->GetAllowableRaces() && !(quest->GetAllowableRaces() & player->getRaceMask()))
                    continue;
                if (quest->GetRequiredClasses() && !(quest->GetRequiredClasses() & player->getClassMask()))
                    continue;
                return true;
            }
            return false;
        }

        // Marks what one trigger unlocks for the character as known; the newly found go in found.
        // stage: the character's progression stage, worked out by the caller once.
        void Credit(Player* player, KnownState& state, uint8 type, uint32 value, uint8 stage, uint32 time,
            std::vector<Entry const*>& found)
        {
            auto itr = sData.byTrigger.find(Key(type, value));
            if (itr == sData.byTrigger.end())
                return;
            for (std::size_t i : itr->second)
            {
                Entry const& e = sData.entries[i];
                if (e.era > stage || state.known.count(e.id) || !Visible(player, e))
                    continue;
                state.known[e.id] = time;
                found.push_back(&e);
            }
        }

        bool Tracks(Player* player)
        {
            return sEnabled && sData.ready && player && GetConfig().enabled && !IsBot(player);
        }

        void Save(Player* player, std::vector<Entry const*> const& found, uint32 time)
        {
            // One statement for the lot: the login catch-up can find hundreds at once.
            constexpr std::size_t ROWS_PER_INSERT = 200;
            uint32 guid = player->GetGUID().GetCounter();
            for (std::size_t start = 0; start < found.size(); start += ROWS_PER_INSERT)
            {
                std::string sql = "INSERT IGNORE INTO `mod_forever_journal_lore_known` (`guid`, `entry`, `time`) VALUES ";
                std::size_t end = std::min(found.size(), start + ROWS_PER_INSERT);
                for (std::size_t i = start; i < end; ++i)
                {
                    if (i != start)
                        sql += ',';
                    sql += Acore::StringFormat("({},{},{})", guid, found[i]->id, time);
                }
                CharacterDatabase.Execute(sql);
            }
        }

        // A name, escaped, cut to fit `limit` bytes once escaped (escaping can triple a character):
        // never inside a %XX or a UTF-8 character.
        std::string Fit(std::string const& text, std::size_t limit)
        {
            std::string out = Escape(text, limit);
            if (out.size() <= limit)
                return out;
            std::size_t cut = limit;
            if (out[cut - 1] == '%')
                cut -= 1;
            else if (out[cut - 2] == '%')
                cut -= 2;
            while (cut > 0 && (uint8(out[cut]) & 0xC0) == 0x80)
                --cut;
            out.resize(cut);
            return out;
        }

        void Announce(Player* player, std::vector<Entry const*> const& found)
        {
            for (Entry const* e : found)
                Send(player, Acore::StringFormat("LN:0:{}:{}:{}:{}", e->id, e->zone, e->kind, Fit(e->title, 120)));
        }

        // A trigger fired while playing: saves and announces what it unlocked.
        void Discover(Player* player, uint8 type, uint32 value)
        {
            if (!Tracks(player) || !sData.byTrigger.count(Key(type, value)))
                return;
            KnownState& state = StateOf(player);
            if (!state.loaded)
                return;

            uint32 now = uint32(std::time(nullptr));
            std::vector<Entry const*> found;
            Credit(player, state, type, value, ProgressionState(player), now, found);
            if (found.empty())
                return;
            Save(player, found, now);
            Announce(player, found);
        }

        // The area and every area it is inside of.
        void DiscoverArea(Player* player, uint32 areaId)
        {
            if (!Tracks(player))
                return;
            if (!player->IsAlive() || player->IsInFlight())
            {
                // Landing reports no area change: look again once the character is down. (A ghost
                // brought back gets one, from ResurrectPlayer.)
                if (player->IsInFlight())
                    WaitForLanding(player);
                return;
            }
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
            for (uint8 i = 0; area && i < 5; ++i)
            {
                Discover(player, TRIGGER_AREA, area->ID);
                area = area->zone ? sAreaTableStore.LookupEntry(area->zone) : nullptr;
            }
        }

        // A book was read: on the shelf it goes, the first time.
        void ReadBook(Player* player, uint32 page, uint32 zone)
        {
            if (!page || !Tracks(player))
                return;
            auto book = sData.books.find(page);
            if (book == sData.books.end())
                return;
            KnownState& state = StateOf(player);
            if (!state.loaded || state.books.count(page))
                return;
            uint32 now = uint32(std::time(nullptr));
            state.books[page] = { now, zone };
            CharacterDatabase.Execute("INSERT IGNORE INTO `mod_forever_journal_book_read` (`guid`, `book`, `zone`, `time`) VALUES ({}, {}, {}, {})",
                player->GetGUID().GetCounter(), page, zone, now);
            Send(player, Acore::StringFormat("BN:0:{}:{}", page, Fit(book->second, 120)));
        }

        // The page text a book object shows, 0 for objects that aren't read.
        uint32 PageOf(GameObjectTemplate const* info)
        {
            if (!info)
                return 0;
            if (info->type == GAMEOBJECT_TYPE_TEXT)
                return info->text.pageID;
            if (info->type == GAMEOBJECT_TYPE_GOOBER)
                return info->goober.pageId;
            return 0;
        }

        // Letters written as HTML (<HTML><BODY><P>...) as plain text with line breaks.
        std::string PlainPage(std::string const& text)
        {
            if (Lower(text.substr(0, 6)) != "<html>")
                return text;
            std::string out;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '<')
                {
                    std::size_t end = text.find('>', i);
                    if (end == std::string::npos)
                        break;
                    std::string tag = Lower(text.substr(i + 1, end - i - 1));
                    if (tag.rfind("br", 0) == 0 || tag == "/p" || tag.rfind("/h", 0) == 0)
                        out += '\n';
                    i = end;
                }
                else if (text[i] == '&')
                {
                    static std::pair<char const*, char> const ENTITIES[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' } };
                    bool matched = false;
                    for (auto const& [name, c] : ENTITIES)
                        if (text.compare(i, std::strlen(name), name) == 0)
                        {
                            out += c;
                            i += std::strlen(name) - 1;
                            matched = true;
                            break;
                        }
                    if (!matched)
                        out += '&';
                }
                else
                    out += text[i];
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
                out.pop_back();
            return out;
        }

        // The explored bit the game sets for the area (Player::CheckAreaExploreAndOutdoor).
        bool Explored(Player* player, AreaTableEntry const* area)
        {
            uint32 offset = area->exploreFlag / 32;
            if (offset >= PLAYER_EXPLORED_ZONES_SIZE)
                return false;
            return player->GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + offset) & (1u << (area->exploreFlag % 32));
        }

        // At login: reads the character's discoveries, then quietly credits what it explored and
        // the quests it took or finished before.
        void LoadCharacter(Player* player)
        {
            KnownState& state = StateOf(player);
            state.known.clear();
            state.loaded = true;

            if (QueryResult result = CharacterDatabase.Query("SELECT `entry`, `time` FROM `mod_forever_journal_lore_known` WHERE `guid` = {}",
                player->GetGUID().GetCounter()))
            {
                do
                {
                    Field* f = result->Fetch();
                    state.known[f[0].Get<uint32>()] = f[1].Get<uint32>();
                } while (result->NextRow());
            }

            state.books.clear();
            if (QueryResult result = CharacterDatabase.Query("SELECT `book`, `time`, `zone` FROM `mod_forever_journal_book_read` WHERE `guid` = {}",
                player->GetGUID().GetCounter()))
            {
                do
                {
                    Field* f = result->Fetch();
                    state.books[f[0].Get<uint32>()] = { f[1].Get<uint32>(), f[2].Get<uint32>() };
                } while (result->NextRow());
            }

            // Areas explored: an explored area counts for every area it's inside of too.
            std::unordered_set<uint32> visited;
            for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
            {
                AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
                if (!area || !Explored(player, area))
                    continue;
                for (uint8 depth = 0; area && depth < 5; ++depth)
                {
                    if (!visited.insert(area->ID).second)
                        break;
                    area = area->zone ? sAreaTableStore.LookupEntry(area->zone) : nullptr;
                }
            }

            uint8 stage = ProgressionState(player);
            std::vector<Entry const*> found;
            for (uint32 areaId : sData.areaTriggers)
                if (visited.count(areaId))
                    Credit(player, state, TRIGGER_AREA, areaId, stage, 0, found);

            for (Trigger const& t : sData.questTriggers)
            {
                bool done = player->GetQuestRewardStatus(t.value);
                bool taken = done || player->GetQuestStatus(t.value) != QUEST_STATUS_NONE;
                if (t.type == TRIGGER_QUEST_DONE ? done : taken)
                    Credit(player, state, t.type, t.value, stage, 0, found);
            }

            if (!found.empty())
                Save(player, found, 0);

            // The area the character is in: entering the world reports it before this runs, and a
            // new character hasn't explored it yet.
            WaitForLanding(player);
        }

        std::string CreatureName(uint32 entry)
        {
            if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry))
                return info->Name;
            return "";
        }

        std::string QuestTitle(uint32 id)
        {
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(id))
                return quest->GetTitle();
            return "";
        }

        // "Explore Goldshire", "Speak with Marshal Dughan": the trigger most players meet first.
        std::string HintFor(Entry const& e)
        {
            static constexpr uint8 ORDER[] = { TRIGGER_AREA, TRIGGER_TALK, TRIGGER_QUEST_ACCEPT, TRIGGER_QUEST_DONE, TRIGGER_KILL };
            for (uint8 type : ORDER)
                for (Trigger const& t : e.triggers)
                {
                    if (t.type != type)
                        continue;
                    std::string name;
                    switch (type)
                    {
                        case TRIGGER_AREA:
                            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(t.value))
                                return "Explore " + std::string(DbcString(area->area_name));
                            break;
                        case TRIGGER_TALK:
                            name = CreatureName(t.value);
                            if (!name.empty())
                                return "Speak with " + name;
                            break;
                        case TRIGGER_KILL:
                            name = CreatureName(t.value);
                            if (!name.empty())
                                return "Defeat " + name;
                            break;
                        case TRIGGER_QUEST_ACCEPT:
                            name = QuestTitle(t.value);
                            if (!name.empty())
                                return "Take the quest \"" + name + "\"";
                            break;
                        case TRIGGER_QUEST_DONE:
                            name = QuestTitle(t.value);
                            if (!name.empty())
                                return "Complete \"" + name + "\"";
                            break;
                        default:
                            break;
                    }
                }
            return "Keep exploring";
        }

        std::string AreaKey(char const* name)
        {
            return Lower(name ? name : "");
        }

        // The text, escaped, in pieces that each fit one row; never cut inside a UTF-8 character.
        std::vector<std::string> Pieces(std::string const& text)
        {
            std::vector<std::string> out;
            std::string piece;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                char c = text[i];
                bool startsCharacter = (uint8(c) & 0xC0) != 0x80;
                if (startsCharacter && piece.size() >= TEXT_PIECE)
                {
                    out.push_back(piece == "-" ? "%2D" : piece);
                    piece.clear();
                }
                if (c == ',' || c == ';' || c == ':' || c == '|' || c == '%' || c == '\r')
                    piece += Acore::StringFormat("%{:02X}", uint8(c));
                else if (c == '\n')
                    piece += "%0A";
                else
                    piece += c;
            }
            if (!piece.empty())
                out.push_back(piece == "-" ? "%2D" : piece);
            return out;
        }

        uint32 ZoneHere(Player* player)
        {
            AreaTableEntry const* zone = TopZone(player->GetAreaId());
            return zone ? zone->ID : player->GetZoneId();
        }

        // ---- loading --------------------------------------------------------------------------

        Data Read()
        {
            Data data;

            if (QueryResult result = WorldDatabase.Query("SELECT `ID`, `Zone`, `Kind`, `SortOrder`, `Faction`, `Races`, `Classes`, `Era`, "
                "`Title`, `Body` FROM `mod_forever_journal_lore`"))
            {
                do
                {
                    Field* f = result->Fetch();
                    Entry e;
                    e.id = f[0].Get<uint32>();
                    e.zone = f[1].Get<uint32>();
                    e.kind = std::min<uint8>(f[2].Get<uint8>(), KIND_COUNT - 1);
                    e.sort = f[3].Get<uint16>();
                    e.faction = f[4].Get<uint8>();
                    e.races = f[5].Get<uint32>();
                    e.classes = f[6].Get<uint32>();
                    e.era = f[7].Get<uint8>();
                    e.title = f[8].Get<std::string>();
                    e.body = f[9].Get<std::string>();
                    if (!sAreaTableStore.LookupEntry(e.zone))
                    {
                        LOG_ERROR("sql.sql", "mod-forever-journal: lore entry {} is filed under zone {}, which isn't in AreaTable.dbc; skipped",
                            e.id, e.zone);
                        continue;
                    }
                    data.byId[e.id] = data.entries.size();
                    data.entries.push_back(std::move(e));
                } while (result->NextRow());
            }

            // Area names inside each zone, for triggers that name their area.
            std::unordered_map<std::string, std::vector<uint32>> areasByName;
            for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
                if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(i))
                    areasByName[AreaKey(area->area_name[LOCALE_enUS])].push_back(area->ID);

            if (QueryResult result = WorldDatabase.Query("SELECT `Entry`, `Type`, `Value`, `Area` FROM `mod_forever_journal_lore_trigger`"))
            {
                do
                {
                    Field* f = result->Fetch();
                    uint32 id = f[0].Get<uint32>();
                    uint8 type = f[1].Get<uint8>();
                    uint32 value = f[2].Get<uint32>();
                    std::string areaName = f[3].Get<std::string>();

                    auto entryItr = data.byId.find(id);
                    if (entryItr == data.byId.end())
                    {
                        LOG_ERROR("sql.sql", "mod-forever-journal: lore trigger for entry {}, which doesn't exist", id);
                        continue;
                    }
                    Entry& e = data.entries[entryItr->second];

                    std::vector<uint32> values;
                    switch (type)
                    {
                        case TRIGGER_AREA:
                            if (value)
                            {
                                if (sAreaTableStore.LookupEntry(value))
                                    values.push_back(value);
                            }
                            else
                            {
                                // By name: only areas of the entry's own zone (names repeat across zones).
                                auto byName = areasByName.find(AreaKey(areaName.c_str()));
                                if (byName != areasByName.end())
                                    for (uint32 areaId : byName->second)
                                        if (AreaTableEntry const* top = TopZone(areaId); top && top->ID == e.zone)
                                            values.push_back(areaId);
                            }
                            if (values.empty())
                                LOG_ERROR("sql.sql", "mod-forever-journal: lore entry {}: no area {} in zone {}", id,
                                    value ? std::to_string(value) : "\"" + areaName + "\"", e.zone);
                            break;
                        case TRIGGER_TALK:
                        case TRIGGER_KILL:
                            if (sObjectMgr->GetCreatureTemplate(value))
                                values.push_back(value);
                            else
                                LOG_ERROR("sql.sql", "mod-forever-journal: lore entry {}: creature {} doesn't exist", id, value);
                            break;
                        case TRIGGER_QUEST_ACCEPT:
                        case TRIGGER_QUEST_DONE:
                            if (sObjectMgr->GetQuestTemplate(value))
                                values.push_back(value);
                            else
                                LOG_ERROR("sql.sql", "mod-forever-journal: lore entry {}: quest {} doesn't exist", id, value);
                            break;
                        default:
                            LOG_ERROR("sql.sql", "mod-forever-journal: lore entry {}: unknown trigger type {}", id, type);
                            break;
                    }
                    for (uint32 v : values)
                    {
                        uint32 faction = 0;
                        if (type == TRIGGER_TALK || type == TRIGGER_KILL)
                            faction = sObjectMgr->GetCreatureTemplate(v)->faction;
                        e.triggers.push_back({ type, v, faction });
                    }
                } while (result->NextRow());
            }

            for (std::size_t i = 0; i < data.entries.size(); ++i)
            {
                Entry& e = data.entries[i];
                if (e.triggers.empty())
                {
                    LOG_ERROR("sql.sql", "mod-forever-journal: lore entry {} \"{}\" has nothing that unlocks it", e.id, e.title);
                    continue;
                }

                // Opens no earlier than the zones its areas are in (Outland, Northrend).
                AreaTableEntry const* home = sAreaTableStore.LookupEntry(e.zone);
                uint8 areaEra = Collections::ZoneEra(home->mapid, e.zone);
                for (Trigger const& t : e.triggers)
                    if (t.type == TRIGGER_AREA)
                        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(t.value))
                            if (AreaTableEntry const* top = TopZone(area->ID))
                                areaEra = std::max(areaEra, Collections::ZoneEra(area->mapid, top->ID));
                e.era = std::max(e.era, areaEra);
                e.hint = HintFor(e);

                for (Trigger const& t : e.triggers)
                {
                    std::vector<std::size_t>& list = data.byTrigger[Key(t.type, t.value)];
                    if (list.empty())
                    {
                        if (t.type == TRIGGER_AREA)
                            data.areaTriggers.push_back(t.value);
                        else if (t.type == TRIGGER_QUEST_ACCEPT || t.type == TRIGGER_QUEST_DONE)
                            data.questTriggers.push_back(t);
                    }
                    list.push_back(i);
                }

                auto [zoneItr, inserted] = data.zoneById.emplace(e.zone, data.zones.size());
                if (inserted)
                {
                    Zone z;
                    z.id = e.zone;
                    z.group = Quests::GroupOfZone(e.zone);
                    z.era = Collections::ZoneEra(home->mapid, e.zone);
                    z.name = Index::AreaName(e.zone);
                    data.zones.push_back(std::move(z));
                }
                data.zones[zoneItr->second].entries.push_back(i);
            }

            for (Zone& z : data.zones)
                std::sort(z.entries.begin(), z.entries.end(), [&data](std::size_t a, std::size_t b)
                {
                    Entry const& x = data.entries[a];
                    Entry const& y = data.entries[b];
                    return std::tie(x.kind, x.sort, x.id) < std::tie(y.kind, y.sort, y.id);
                });

            // Books: objects that show a page (books, plaques, tablets), then readable items. Named
            // after the first thing that shows them; an object's name wins over an item's.
            for (auto const& [entry, info] : *sObjectMgr->GetGameObjectTemplates())
                if (uint32 page = PageOf(&info); page && sObjectMgr->GetPageText(page))
                    data.books.emplace(page, info.name);
            for (auto const& [entry, item] : *sObjectMgr->GetItemTemplateStore())
                if (item.PageText && sObjectMgr->GetPageText(item.PageText))
                    data.books.emplace(item.PageText, item.Name1);

            data.ready = true;
            return data;
        }
    }

    bool Ready()
    {
        return sEnabled && sData.ready;
    }

    void Load()
    {
        sEnabled = sConfigMgr->GetOption<bool>("Journal.Lore.Enable", true);
        if (!sEnabled)
            return;

        uint32 started = getMSTime();
        sData = Read();
        LOG_INFO("server.loading", ">> mod-forever-journal: {} lore entries in {} zones and {} books in {} ms", sData.entries.size(),
            sData.zones.size(), sData.books.size(), GetMSTimeDiffToNow(started));
    }

    // ---- requests -------------------------------------------------------------------------------

    void HandleZones(Player* player, std::string const& req)
    {
        KnownState& state = StateOf(player);
        uint32 knownAll = 0, totalAll = 0;
        std::vector<std::string> rows;
        for (Zone const& z : sData.zones)
        {
            uint32 known = 0, total = 0;
            for (std::size_t i : z.entries)
            {
                Entry const& e = sData.entries[i];
                if (!Visible(player, e))
                    continue;
                ++total;
                if (state.known.count(e.id))
                    ++known;
            }
            if (!total)
                continue;
            knownAll += known;
            totalAll += total;
            rows.push_back(Acore::StringFormat("Z,{},{},{},{},{},{}", z.id, z.group, known, total, z.era, Fit(z.name, 100)));
        }
        Send(player, Acore::StringFormat("YR:{}:{}:{}:{}", req, knownAll, totalAll, ZoneHere(player)));
        SendRows(player, "YD:" + req, rows);
        Send(player, "YE:" + req);
    }

    void HandleZone(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint32 zoneId = 0;
        if (args.size() < 3 || !ParseUInt(args[2], zoneId))
        {
            SendError(player, req, "bad");
            return;
        }
        auto zoneItr = sData.zoneById.find(zoneId);
        if (zoneItr == sData.zoneById.end())
        {
            SendError(player, req, "none");
            return;
        }

        KnownState& state = StateOf(player);
        uint32 known = 0, total = 0;
        std::vector<std::string> rows;
        for (std::size_t i : sData.zones[zoneItr->second].entries)
        {
            Entry const& e = sData.entries[i];
            if (!Visible(player, e))
                continue;
            ++total;
            uint8 entryState = STATE_UNKNOWN;
            if (auto knownItr = state.known.find(e.id); knownItr != state.known.end())
            {
                entryState = knownItr->second ? STATE_FOUND : STATE_KNOWN;
                ++known;
            }
            rows.push_back(Acore::StringFormat("E,{},{},{},{},{},{}", e.id, e.kind, entryState, e.era, Fit(e.title, 80),
                entryState == STATE_UNKNOWN ? Fit(e.hint, 100) : std::string("-")));
        }
        Send(player, Acore::StringFormat("UR:{}:{}:{}:{}", req, zoneId, known, total));
        SendRows(player, "UD:" + req, rows);
        Send(player, "UE:" + req);
    }

    void HandleEntry(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint32 id = 0;
        if (args.size() < 3 || !ParseUInt(args[2], id))
        {
            SendError(player, req, "bad");
            return;
        }
        auto itr = sData.byId.find(id);
        if (itr == sData.byId.end())
        {
            SendError(player, req, "none");
            return;
        }
        Entry const& e = sData.entries[itr->second];
        // Undiscovered text stays on the server, except for game masters.
        if (!StateOf(player).known.count(e.id) && !player->IsGameMaster())
        {
            SendError(player, req, "locked");
            return;
        }

        Send(player, Acore::StringFormat("VR:{}:{}:{}:{}:{}", req, e.id, e.zone, e.kind, Fit(e.title, 120)));
        SendRows(player, "VD:" + req, Pieces(e.body));
        Send(player, "VE:" + req);
    }

    void HandleLibrary(Player* player, std::string const& req)
    {
        KnownState& state = StateOf(player);
        std::vector<std::string> rows;
        for (auto const& [page, read] : state.books)
        {
            auto book = sData.books.find(page);
            if (book == sData.books.end())
                continue; // gone from the database since
            rows.push_back(Acore::StringFormat("B,{},{},{},{}", page, read.time, Fit(book->second, 80),
                read.zone ? Fit(Index::AreaName(read.zone), 60) : std::string("-")));
        }
        Send(player, Acore::StringFormat("JR:{}:{}", req, rows.size()));
        SendRows(player, "JD:" + req, rows);
        Send(player, "JE:" + req);
    }

    void HandleBook(Player* player, std::string const& req, std::vector<std::string_view> const& args)
    {
        uint32 page = 0;
        if (args.size() < 3 || !ParseUInt(args[2], page))
        {
            SendError(player, req, "bad");
            return;
        }
        auto book = sData.books.find(page);
        if (book == sData.books.end())
        {
            SendError(player, req, "none");
            return;
        }
        // Books not read yet stay on the server, except for game masters.
        if (!StateOf(player).books.count(page) && !player->IsGameMaster())
        {
            SendError(player, req, "locked");
            return;
        }

        std::vector<std::string> rows;
        std::unordered_set<uint32> seen;
        uint32 count = 0;
        for (uint32 id = page; id && count < MAX_PAGES && seen.insert(id).second; ++count)
        {
            PageText const* text = sObjectMgr->GetPageText(id);
            if (!text)
                break;
            for (std::string const& piece : Pieces(PlainPage(text->Text)))
                rows.push_back(Acore::StringFormat("{}.{}", count + 1, piece));
            id = text->NextPage;
        }
        Send(player, Acore::StringFormat("PR:{}:{}:{}:{}", req, page, count, Fit(book->second, 120)));
        SendRows(player, "PD:" + req, rows);
        Send(player, "PE:" + req);
    }

    // ---- hooks ----------------------------------------------------------------------------------

    void OnLogin(Player* player)
    {
        if (!Tracks(player))
            return;
        LoadCharacter(player);
    }

    // World thread, while no map updates: credits the area of characters who have landed.
    void CheckLandings(uint32 diff)
    {
        static uint32 timer = 0;
        timer += diff;
        if (timer < LANDING_CHECK_MS)
            return;
        timer = 0;

        std::vector<ObjectGuid::LowType> waiting;
        {
            std::lock_guard<std::mutex> lock(sLandingLock);
            if (sLanding.empty())
                return;
            waiting.assign(sLanding.begin(), sLanding.end());
        }
        for (ObjectGuid::LowType guid : waiting)
        {
            Player* player = ObjectAccessor::FindPlayerByLowGUID(guid);
            bool done = !player || !player->IsInWorld() || !Tracks(player);
            if (!done && player->IsAlive() && !player->IsInFlight())
            {
                DiscoverArea(player, player->GetAreaId());
                done = true;
            }
            if (done)
            {
                std::lock_guard<std::mutex> lock(sLandingLock);
                sLanding.erase(guid);
            }
        }
    }
}

using namespace Journal;

class JournalLorePlayerScript : public PlayerScript
{
public:
    JournalLorePlayerScript() : PlayerScript("JournalLorePlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_UPDATE_AREA,
            PLAYERHOOK_ON_CREATURE_KILL_CREDIT,
            PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
            PLAYERHOOK_ON_DELETE_FROM_DB,
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        Lore::OnLogin(player);
    }

    void OnPlayerUpdateArea(Player* player, uint32 /*oldArea*/, uint32 newArea) override
    {
        Lore::DiscoverArea(player, newArea);
    }

    // Everyone in the group who shares the kill, the way quest kill credit is shared.
    void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
    {
        if (!player || !killed || !Lore::sData.byTrigger.count(Lore::Key(Lore::TRIGGER_KILL, killed->GetEntry())))
            return;
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                if (Player* member = itr->GetSource())
                    if (member->IsInMap(killed) && member->IsAtGroupRewardDistance(killed))
                        Lore::Discover(member, Lore::TRIGGER_KILL, killed->GetEntry());
        }
        else
            Lore::Discover(player, Lore::TRIGGER_KILL, killed->GetEntry());
    }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        Lore::Discover(player, Lore::TRIGGER_QUEST_ACCEPT, quest->GetQuestId());
    }

    // Called once a quest is turned in. Quests that complete straight away count as taken too.
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        Lore::Discover(player, Lore::TRIGGER_QUEST_ACCEPT, quest->GetQuestId());
        Lore::Discover(player, Lore::TRIGGER_QUEST_DONE, quest->GetQuestId());
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `mod_forever_journal_lore_known` WHERE `guid` = {}", guid);
        trans->Append("DELETE FROM `mod_forever_journal_book_read` WHERE `guid` = {}", guid);
    }
};

// Talking to someone: opening their gossip or quest window. Never takes the conversation over.
class JournalLoreCreatureScript : public AllCreatureScript
{
public:
    JournalLoreCreatureScript() : AllCreatureScript("JournalLoreCreatureScript") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (player && creature)
            Lore::Discover(player, Lore::TRIGGER_TALK, creature->GetEntry());
        return false;
    }
};

// Reading a book, plaque or tablet in the world: using an object that shows a page.
class JournalLoreGameObjectScript : public AllGameObjectScript
{
public:
    JournalLoreGameObjectScript() : AllGameObjectScript("JournalLoreGameObjectScript") { }

    bool CanGameObjectGossipHello(Player* player, GameObject* go) override
    {
        if (player && go)
            if (uint32 page = Lore::PageOf(go->GetGOInfo()))
                Lore::ReadBook(player, page, go->GetZoneId());
        return false;
    }
};

// Reading a letter or note in the bags (CMSG_READ_ITEM has no script hook of its own). Runs where
// the packet's handler runs, just before it.
class JournalLoreServerScript : public ServerScript
{
public:
    JournalLoreServerScript() : ServerScript("JournalLoreServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != CMSG_READ_ITEM || packet.size() < 2 || !session)
            return true;
        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld())
            return true;
        Item* item = player->GetItemByPos(packet.read<uint8>(0), packet.read<uint8>(1));
        if (item && item->GetTemplate()->PageText && player->CanUseItem(item) == EQUIP_ERR_OK)
            Lore::ReadBook(player, item->GetTemplate()->PageText, player->GetZoneId());
        return true;
    }
};

class JournalLoreWorldScript : public WorldScript
{
public:
    JournalLoreWorldScript() : WorldScript("JournalLoreWorldScript", { WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 diff) override
    {
        Lore::CheckLandings(diff);
    }
};

class JournalLoreCommandScript : public CommandScript
{
public:
    JournalLoreCommandScript() : CommandScript("JournalLoreCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable loreTable =
        {
            { "",         HandleStatus,   SEC_GAMEMASTER,    Console::No },
            { "discover", HandleDiscover, SEC_GAMEMASTER,    Console::No },
            { "reset",    HandleReset,    SEC_GAMEMASTER,    Console::No },
            { "reload",   HandleReload,   SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable journalTable =
        {
            { "lore", loreTable },
        };
        static ChatCommandTable commandTable =
        {
            { "journal", journalTable },
        };
        return commandTable;
    }

    static Player* Target(ChatHandler* handler)
    {
        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
            handler->SendErrorMessage("Select a player first.", false);
        return target;
    }

    // .journal lore: how much lore there is, and how much of it the selected character has found.
    static bool HandleStatus(ChatHandler* handler)
    {
        if (!Lore::Ready())
        {
            handler->SendErrorMessage("The Lore tab is turned off (Journal.Lore.Enable).", false);
            return false;
        }
        Player* target = Target(handler);
        if (!target)
            return false;
        uint32 known = 0, total = 0;
        Lore::KnownState& state = Lore::StateOf(target);
        for (Lore::Entry const& e : Lore::sData.entries)
            if (Lore::Visible(target, e))
            {
                ++total;
                if (state.known.count(e.id))
                    ++known;
            }
        handler->PSendSysMessage("Lore: {} entries in {} zones, {} books. {} has found {} of the {} it can find, and read {} books.",
            Lore::sData.entries.size(), Lore::sData.zones.size(), Lore::sData.books.size(), target->GetName(), known, total,
            state.books.size());
        return true;
    }

    // .journal lore discover <entry|all>: unlocks an entry (or every one) for the selected character.
    static bool HandleDiscover(ChatHandler* handler, Tail what)
    {
        if (!Lore::Ready())
            return false;
        Player* target = Target(handler);
        if (!target || IsBot(target))
            return false;
        Lore::KnownState& state = Lore::StateOf(target);
        if (!state.loaded)
            return false;

        uint32 id = 0;
        bool all = std::string_view(what) == "all";
        if (!all && !ParseUInt(std::string_view(what), id))
        {
            handler->SendErrorMessage("Usage: .journal lore discover <entry|all>", false);
            return false;
        }

        uint32 now = uint32(std::time(nullptr));
        std::vector<Lore::Entry const*> found;
        for (Lore::Entry const& e : Lore::sData.entries)
            if ((all || e.id == id) && !state.known.count(e.id) && Lore::Visible(target, e))
            {
                state.known[e.id] = now;
                found.push_back(&e);
            }
        Lore::Save(target, found, now);
        if (!all)
            Lore::Announce(target, found);
        handler->PSendSysMessage("{} discovered {} lore entries.", target->GetName(), found.size());
        return true;
    }

    // .journal lore reset: forgets everything the selected character found and read. The login
    // catch-up credits explored areas and quests again at its next login.
    static bool HandleReset(ChatHandler* handler)
    {
        Player* target = Target(handler);
        if (!target)
            return false;
        Lore::StateOf(target).known.clear();
        Lore::StateOf(target).books.clear();
        CharacterDatabase.Execute("DELETE FROM `mod_forever_journal_lore_known` WHERE `guid` = {}", target->GetGUID().GetCounter());
        CharacterDatabase.Execute("DELETE FROM `mod_forever_journal_book_read` WHERE `guid` = {}", target->GetGUID().GetCounter());
        handler->PSendSysMessage("{}'s lore discoveries and library are cleared.", target->GetName());
        return true;
    }

    // .journal lore reload: reads the lore tables again. Discoveries are kept by entry id.
    static bool HandleReload(ChatHandler* handler)
    {
        Lore::Load();
        handler->PSendSysMessage("Lore reloaded: {} entries in {} zones.", Lore::sData.entries.size(), Lore::sData.zones.size());
        return true;
    }
};

namespace Journal::Lore
{
    void AddScripts()
    {
        new JournalLorePlayerScript();
        new JournalLoreCreatureScript();
        new JournalLoreGameObjectScript();
        new JournalLoreServerScript();
        new JournalLoreWorldScript();
        new JournalLoreCommandScript();
    }
}
