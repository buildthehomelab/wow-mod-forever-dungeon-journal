/*
 * mod-forever-journal: configuration, the addon transport, request dispatch and script
 * registration.
 *
 * Wire format, both directions: "<COMMAND>:<request id>:<field>:<field>..." after the addon
 * prefix and a tab. Rows inside a field are separated by ';' and their values by ','. List
 * answers come as "<X>R" (meta), any number of "<X>D" (rows) and "<X>E" (end).
 *
 *   HELLO:<req>:<protocol>[:<addon features>] -> HELLO:<req>:<version>:<flags>:<stamp>:<era state>
 *   D:<req>                           -> DR:<req>:<instances>, DD rows (I/W/V), DE
 *   B:<req>:<instance>                -> BR, BD rows, BE        bosses
 *   S:<req>:<instance>                -> SR, SD rows (V/R), SE   lockouts, era locks, requirements
 *   A:<req>:<instance>:<boss>:<var>   -> AR, AD rows (U/S/N/E/R), AE  abilities, enrage, rewards
 *   L:<req>:<instance>:<boss>:<var>   -> LR, LD rows, LE        loot (boss "all" = every boss)
 *   K:<req>:<item>                    -> KR, KD rows, KE        what a token turns into
 *   Q:<req>:<instance>                -> QR, QD rows, QE        quests with the player's status
 *   QD:<req>:<quest>                  -> TR, TD rows (O/K/I), TE quest objectives
 *   H:<req>                           -> H:<req>:<instance>:<variant>   the instance you're in
 *   M:<req>:<E|Q>:<id>                -> M:<req>:ok:<where>     flags an entrance or quest giver
 *   F:<req>:<text>                    -> FR, FD rows (I/B/L), FE  instances, bosses and loot by name
 *   C:<req>:<M|P|T>                   -> CR, CD rows, CE        mounts, pets or toys
 *   W:<req>:<M|P|T>:<id>              -> WR, WD rows, WE        where one comes from
 *   O:<req>                           -> OR, OD rows, OE        toys the character has
 *   M:<req>:S:<M|P|T>:<id>:<source>   -> M:<req>:ok:<where>     flags a source
 *   G:<req>                           -> GR, GD rows, GE        quest headings, with the player's counts
 *   Z:<req>:<heading>                 -> ZR, ZD rows (Q/R), ZE  every quest under a heading
 *   X:<req>:<text>                    -> XR, XD rows (C/Q/R), XE  quests by name
 *   N:<req>:<C|P|S>:<value>           -> NR, ND rows, NE        the town directory
 *   M:<req>:N:<place>:<entry>         -> M:<req>:ok:<where>     flags a directory result
 *
 * The server sends one message unasked: OPEN:0:<tab>[:<argument>] opens one of the addon's tabs
 * (the Town Guide spell and .find), only to addons whose HELLO said they can (ADDON_OPEN).
 *
 * Addon messages arrive as CMSG_MESSAGECHAT on the world thread, so requests are answered
 * straight from the hook. The rows are documented beside their handlers (JournalIndex.cpp,
 * JournalCollections.cpp, JournalQuests.cpp and JournalDirectory.cpp).
 */

#include "Journal.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <charconv>
#include <mutex>
#include <unordered_map>

namespace Journal
{
    namespace
    {
        Config sConfig;

        // Requests walk the index on the world thread and answers can run to dozens of messages;
        // addon whispers are never muted by the chat flood check, so each character gets a budget.
        constexpr float REQUEST_BURST = 16.0f;
        constexpr float REQUEST_RATE = 4.0f;

        struct SessionState
        {
            float tokens = REQUEST_BURST;
            uint32 lastRefill = 0;
        };

        std::unordered_map<ObjectGuid::LowType, SessionState> sSessions;

        // Written from HELLO (world thread), read when the Town Guide is cast (map threads).
        std::mutex sFeaturesLock;
        std::unordered_map<ObjectGuid::LowType, uint32> sFeatures;

        constexpr uint32 IP_PROGRESSION_QUEST_BASE = 66000;
        constexpr uint8 IP_STATE_MAX = 18;
    }

    Config& GetConfig()
    {
        return sConfig;
    }

    uint32 AddonFeatures(Player* player)
    {
        std::lock_guard<std::mutex> lock(sFeaturesLock);
        auto itr = sFeatures.find(player->GetGUID().GetCounter());
        return itr != sFeatures.end() ? itr->second : 0;
    }

    void Send(Player* player, std::string const& payload)
    {
        if (!player || !player->GetSession())
            return;

        std::string full = std::string(PREFIX) + "\t" + payload;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, full);
        player->GetSession()->SendPacket(&data);
    }

    void SendRows(Player* player, std::string const& header, std::vector<std::string> const& rows)
    {
        std::string line;
        std::size_t const budget = MAX_PAYLOAD - header.size() - 1;
        for (std::string const& row : rows)
        {
            if (row.size() > budget)
                continue; // never happens with Escape's length cap; a cut row would corrupt the list
            if (!line.empty() && line.size() + 1 + row.size() > budget)
            {
                Send(player, header + ":" + line);
                line.clear();
            }
            if (!line.empty())
                line += ';';
            line += row;
        }
        if (!line.empty())
            Send(player, header + ":" + line);
    }

    void SendError(Player* player, std::string const& req, std::string_view what)
    {
        Send(player, "ERR:" + req + ":" + std::string(what));
    }

    bool ParseUInt(std::string_view text, uint32& out)
    {
        if (text.empty())
            return false;
        auto result = std::from_chars(text.data(), text.data() + text.size(), out, 10);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }

    std::string Escape(std::string_view text, std::size_t maxLength)
    {
        if (text.empty())
            return "-";

        if (text.size() > maxLength)
        {
            // Cut on a character boundary: never inside a UTF-8 sequence.
            std::size_t cut = maxLength;
            while (cut > 0 && (uint8(text[cut]) & 0xC0) == 0x80)
                --cut;
            text = text.substr(0, cut);
        }

        static char const* const HEX = "0123456789ABCDEF";
        std::string out;
        out.reserve(text.size());
        for (char c : text)
        {
            if (c == ',' || c == ';' || c == ':' || c == '|' || c == '%' || c == '\n' || c == '\r')
            {
                out += '%';
                out += HEX[(uint8(c) >> 4) & 0xF];
                out += HEX[uint8(c) & 0xF];
            }
            else
                out += c;
        }
        // A lone "-" would read back as empty.
        return out == "-" ? "%2D" : out;
    }

    uint8 ProgressionState(Player* player)
    {
        if (!sConfig.progressionEnabled || !sConfig.eraLocks || player->IsGameMaster())
            return IP_STATE_MAX;

        uint8 state = 0;
        for (uint8 i = 1; i <= IP_STATE_MAX; ++i)
            if (player->GetQuestStatus(IP_PROGRESSION_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
                state = i;
        return state;
    }

    namespace
    {
        // The index (rares, era locks, loot filters) is built once at startup and the addon caches
        // what it sends under the build's data stamp, so a config reload only changes the settings
        // the index doesn't depend on.
        void LoadConfig(bool reload)
        {
            sConfig.enabled = sConfigMgr->GetOption<bool>("Journal.Enable", true);
            sConfig.learnAbilities = sConfigMgr->GetOption<bool>("Journal.LearnAbilities", true);
            sConfig.maxQuestsPerInstance = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Journal.MaxQuestsPerInstance", 60), 5, 200);
            Directory::LoadConfig();
            if (reload)
                return;
            sConfig.collections = sConfigMgr->GetOption<bool>("Journal.Collections.Enable", true);
            sConfig.showUnobtainable = sConfigMgr->GetOption<bool>("Journal.Collections.ShowUnobtainable", false);
            sConfig.quests = sConfigMgr->GetOption<bool>("Journal.Quests.Enable", true);
            sConfig.rares = sConfigMgr->GetOption<bool>("Journal.Rares", true);
            sConfig.eraLocks = sConfigMgr->GetOption<bool>("Journal.EraLocks", true);
            sConfig.worldDropThreshold = std::max<uint32>(sConfigMgr->GetOption<uint32>("Journal.Loot.WorldDropThreshold", 25), 2);
            sConfig.showWorldDrops = sConfigMgr->GetOption<bool>("Journal.Loot.ShowWorldDrops", false);
            sConfig.showGrayLoot = sConfigMgr->GetOption<bool>("Journal.Loot.ShowPoorAndCommon", false);
            sConfig.progressionEnabled = sConfigMgr->GetOption<bool>("IndividualProgression.Enable", false, false);
            sConfig.zulGurubState = uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("IndividualProgression.RequiredZulGurubProgression", 3, false), IP_STATE_MAX));
            sConfig.zulAmanState = uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("IndividualProgression.RequiredZulAmanProgression", 12, false), IP_STATE_MAX));
        }

        bool TakeToken(SessionState& state, float cost)
        {
            uint32 now = getMSTime();
            if (state.lastRefill)
                state.tokens = std::min(REQUEST_BURST, state.tokens + getMSTimeDiff(state.lastRefill, now) * REQUEST_RATE / 1000.0f);
            state.lastRefill = now;
            if (state.tokens < cost)
                return false;
            state.tokens -= cost;
            return true;
        }

        void HandleHello(Player* player, std::string const& req, std::vector<std::string_view> const& args)
        {
            // Older addons send no features; a later HELLO (a UI reload) replaces what was sent. An
            // addon on another protocol refuses the server's data, so it can't open a tab either.
            uint32 protocol = 0;
            uint32 features = 0;
            if (args.size() > 3 && ParseUInt(args[2], protocol) && protocol == PROTOCOL_VERSION)
                ParseUInt(args[3], features);
            {
                std::lock_guard<std::mutex> lock(sFeaturesLock);
                sFeatures[player->GetGUID().GetCounter()] = features;
            }

            uint32 flags = HELLO_JOURNAL | HELLO_MAP_PINS;
            if (sConfig.progressionEnabled && sConfig.eraLocks)
                flags |= HELLO_PROGRESSION;
            if (sConfig.learnAbilities)
                flags |= HELLO_LEARNING;
            if (sConfig.collections)
                flags |= HELLO_COLLECTIONS;
            if (sConfig.quests)
                flags |= HELLO_QUESTS;
            if (Directory::Ready())
                flags |= HELLO_DIRECTORY;
            Send(player, "HELLO:" + req + ":" + std::to_string(PROTOCOL_VERSION) + ":" + std::to_string(flags)
                + ":" + std::to_string(Index::Stamp()) + ":" + std::to_string(ProgressionState(player)));
        }

        std::vector<std::string_view> Split(std::string_view text, char sep)
        {
            std::vector<std::string_view> parts;
            std::size_t start = 0;
            while (true)
            {
                std::size_t pos = text.find(sep, start);
                if (pos == std::string_view::npos)
                {
                    parts.push_back(text.substr(start));
                    break;
                }
                parts.push_back(text.substr(start, pos - start));
                start = pos + 1;
            }
            return parts;
        }

        float CostOf(std::string_view command)
        {
            // The instance list and "every boss" loot are the long answers; the addon keeps them.
            if (command == "D" || command == "C")
                return 6.0f;
            if (command == "O")
                return 2.0f;
            // Every heading, and the longest headings' quests.
            if (command == "G" || command == "Z")
                return 4.0f;
            if (command == "L" || command == "Q" || command == "F" || command == "K" || command == "X" || command == "N")
                return 2.0f;
            return 1.0f;
        }

        void Dispatch(Player* player, std::string_view message)
        {
            std::vector<std::string_view> args = Split(message, ':');
            if (args.size() < 2 || args.size() > 8)
                return;

            std::string_view command = args[0];
            std::string req(args[1].substr(0, 12));

            if (command == "HELLO")
            {
                HandleHello(player, req, args);
                return;
            }

            if (!TakeToken(sSessions[player->GetGUID().GetCounter()], CostOf(command)))
            {
                SendError(player, req, "busy");
                return;
            }

            if (command == "D")
                Index::HandleList(player, req);
            else if (command == "B")
                Index::HandleBosses(player, req, args);
            else if (command == "S")
                Index::HandleStatus(player, req, args);
            else if (command == "A")
                Index::HandleAbilities(player, req, args);
            else if (command == "L")
                Index::HandleLoot(player, req, args);
            else if (command == "K")
                Loot::HandleTokens(player, req, args);
            else if (command == "Q")
                Index::HandleQuests(player, req, args);
            else if (command == "QD")
                Index::HandleQuestDetail(player, req, args);
            else if (command == "H")
                Index::HandleHere(player, req);
            else if (command == "M" && args.size() > 2 && args[2] == "N" && Directory::Ready())
                Directory::HandleMapPin(player, req, args);
            else if (command == "M")
                Index::HandleMapPin(player, req, args);
            else if (command == "F")
                Index::HandleSearch(player, req, args);
            else if (command == "C" && sConfig.collections)
                Collections::HandleCatalog(player, req, args);
            else if (command == "W" && sConfig.collections)
                Collections::HandleSources(player, req, args);
            else if (command == "O" && sConfig.collections)
                Collections::HandleOwnedToys(player, req);
            else if (command == "G" && sConfig.quests)
                Quests::HandleCategories(player, req);
            else if (command == "Z" && sConfig.quests)
                Quests::HandleCategory(player, req, args);
            else if (command == "X" && sConfig.quests)
                Quests::HandleSearch(player, req, args);
            else if (command == "N" && Directory::Ready())
                Directory::HandleList(player, req, args);
            else
                SendError(player, req, "unknown");
        }
    }
}

using namespace Journal;

class JournalPlayerScript : public PlayerScript
{
public:
    JournalPlayerScript() : PlayerScript("JournalPlayerScript",
        {
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
            PLAYERHOOK_ON_LOGOUT
        }) { }

    // The addon whispers itself; swallow those messages so they never show up as chat.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (lang != LANG_ADDON || !receiver || receiver != player)
            return true;

        std::string const prefixTab = std::string(PREFIX) + "\t";
        if (msg.compare(0, prefixTab.size(), prefixTab) != 0)
            return true;

        if (!GetConfig().enabled)
        {
            Send(player, "OFF:0");
            return false;
        }

        Dispatch(player, std::string_view(msg).substr(prefixTab.size()));
        return false;
    }

    void OnPlayerLogout(Player* player) override
    {
        sSessions.erase(player->GetGUID().GetCounter());
        std::lock_guard<std::mutex> lock(sFeaturesLock);
        sFeatures.erase(player->GetGUID().GetCounter());
    }
};

class JournalWorldScript : public WorldScript
{
public:
    JournalWorldScript() : WorldScript("JournalWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig(reload);
    }

    // After every spawn is loaded and before the maps update: entrances are placed in zones from
    // the map files.
    void OnStartup() override
    {
        if (!GetConfig().enabled)
            return;
        Index::Build();
        if (GetConfig().quests)
            Quests::Build();
        if (GetConfig().directory)
            Directory::Build();
    }
};

// Bosses' abilities that live only in C++ or come from spells triggered at run time: whatever a
// journal creature casts on its own map is remembered. Runs on map threads.
class JournalSpellScript : public AllSpellScript
{
public:
    JournalSpellScript() : AllSpellScript("JournalSpellScript", { ALLSPELLHOOK_ON_CAST }) { }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!caster || !spellInfo || caster->GetTypeId() != TYPEID_UNIT || !GetConfig().enabled || !GetConfig().learnAbilities)
            return;
        Index::OnCreatureCast(caster, spellInfo);
    }
};

void AddJournalScripts()
{
    new JournalPlayerScript();
    new JournalWorldScript();
    new JournalSpellScript();
    Directory::AddScripts();
}
