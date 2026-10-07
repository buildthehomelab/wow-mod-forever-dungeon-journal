/*
 * mod-forever-dungeon-journal: the spells a creature uses, for the journal's Abilities tab.
 *
 * Four sources, in this order:
 *   1. its C++ script: mod_forever_dungeon_journal_script_spell, generated from the scripts' source by
 *      tools/extract_script_spells.py (keyed by ScriptName);
 *   2. its SmartAI script, following the timed action lists it calls; events limited to some
 *      difficulties only show on those;
 *   3. the creature's own spell list (creature_template_spell);
 *   4. what it was seen casting (mod_forever_dungeon_journal_learned, filled by OnCreatureCast).
 * A spell with a SpellDifficulty entry is shown as its version for the tab's difficulty.
 * The server only drops passive and hidden spells; the addon also skips spells without a client
 * description, which are the internal helpers (triggers, dummies, visuals).
 */

#include "DungeonJournal.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace DungeonJournal::Abilities
{
    namespace
    {
        constexpr uint8 ALL_DIFFICULTIES = 0x0F;

        struct Known
        {
            uint32 spell = 0;
            uint8 difficulties = ALL_DIFFICULTIES; // bit per map difficulty
            bool learned = false;
        };

        struct CreatureSpells
        {
            std::vector<Known> spells;
            std::unordered_set<uint32> seen;
            std::vector<uint32> summons;
        };

        std::unordered_map<uint32, CreatureSpells> sCreatures;
        std::vector<uint32> const sNoSummons;

        // Learning runs on map threads; requests on the world thread.
        std::mutex sLock;

        struct ScriptData
        {
            std::vector<uint32> spells;
            std::vector<uint32> summons;
        };

        // The base (difficulty 0) version of a spell that has difficulty versions.
        uint32 BaseSpell(uint32 spellId)
        {
            if (uint32 diffId = sSpellMgr->GetSpellDifficultyId(spellId))
                if (SpellDifficultyEntry const* entry = sSpellDifficultyStore.LookupEntry(diffId))
                    if (entry->SpellID[0] > 0)
                        return uint32(entry->SpellID[0]);
            return spellId;
        }

        // The version of a spell a difficulty uses, the way SpellMgr::GetSpellIdForDifficulty
        // picks it: heroic raid modes fall back to their normal mode, anything missing to the spell.
        uint32 SpellForDifficulty(uint32 spellId, uint8 difficulty)
        {
            uint32 diffId = sSpellMgr->GetSpellDifficultyId(spellId);
            if (!diffId || difficulty >= MAX_DIFFICULTY)
                return spellId;
            SpellDifficultyEntry const* entry = sSpellDifficultyStore.LookupEntry(diffId);
            if (!entry)
                return spellId;
            uint8 mode = difficulty;
            if (entry->SpellID[mode] <= 0 && mode > DUNGEON_DIFFICULTY_HEROIC)
                mode -= 2;
            if (entry->SpellID[mode] <= 0)
                return spellId;
            return uint32(entry->SpellID[mode]);
        }

        void AddSpell(CreatureSpells& into, uint32 spellId, uint8 difficulties, bool learned)
        {
            if (!spellId || !difficulties)
                return;
            spellId = BaseSpell(spellId);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || !IsListable(info))
                return;
            if (into.seen.count(spellId))
            {
                // Seen on more difficulties than first thought (another event, or cast for real).
                for (Known& k : into.spells)
                    if (k.spell == spellId)
                    {
                        k.difficulties |= difficulties;
                        if (!learned)
                            k.learned = false;
                    }
                return;
            }
            into.seen.insert(spellId);
            into.spells.push_back({ spellId, difficulties, learned });
        }

        void AddSummon(CreatureSpells& into, uint32 entry)
        {
            if (entry && sObjectMgr->GetCreatureTemplate(entry)
                && std::find(into.summons.begin(), into.summons.end(), entry) == into.summons.end())
                into.summons.push_back(entry);
        }

        std::unordered_map<std::string, ScriptData> ReadScriptTable()
        {
            std::unordered_map<std::string, ScriptData> scripts;
            QueryResult result = WorldDatabase.Query("SELECT ScriptName, Kind, Value FROM mod_forever_dungeon_journal_script_spell "
                "ORDER BY ScriptName, Sort");
            if (!result)
            {
                LOG_WARN("server.loading", ">> mod-forever-dungeon-journal: mod_forever_dungeon_journal_script_spell is empty; "
                    "abilities from C++ scripts are missing until the module's SQL is applied");
                return scripts;
            }
            do
            {
                Field* f = result->Fetch();
                ScriptData& data = scripts[f[0].Get<std::string>()];
                uint32 value = f[2].Get<uint32>();
                if (f[1].Get<uint8>() == 0)
                    data.spells.push_back(value);
                else
                    data.summons.push_back(value);
            } while (result->NextRow());
            return scripts;
        }

        // The difficulties a SmartAI event runs on.
        uint8 EventDifficulties(SmartScriptHolder const& holder)
        {
            uint32 mask = (holder.event.event_flags & SMART_EVENT_FLAG_DIFFICULTY_ALL) >> 1;
            return mask ? uint8(mask & ALL_DIFFICULTIES) : ALL_DIFFICULTIES;
        }

        void ReadSmartActions(CreatureSpells& into, SmartAIEventList const& events, uint8 inherited,
            std::unordered_set<uint32>& visitedLists, uint32 depth)
        {
            auto followList = [&](uint32 listId, uint8 difficulties)
            {
                if (!listId || depth >= 4 || !visitedLists.insert(listId).second)
                    return;
                ReadSmartActions(into, sSmartScriptMgr->GetScript(int32(listId), SMART_SCRIPT_TYPE_TIMED_ACTIONLIST),
                    difficulties, visitedLists, depth + 1);
            };

            for (SmartScriptHolder const& holder : events)
            {
                uint8 difficulties = inherited & EventDifficulties(holder);
                switch (holder.GetActionType())
                {
                    case SMART_ACTION_CAST:
                    case SMART_ACTION_SELF_CAST:
                    case SMART_ACTION_INVOKER_CAST:
                        AddSpell(into, holder.action.cast.spell, difficulties, false);
                        break;
                    case SMART_ACTION_CROSS_CAST:
                        AddSpell(into, holder.action.crossCast.spell, difficulties, false);
                        break;
                    case SMART_ACTION_ADD_AURA:
                        AddSpell(into, holder.action.addAura.spell, difficulties, false);
                        break;
                    case SMART_ACTION_SUMMON_CREATURE:
                        AddSummon(into, holder.action.summonCreature.creature);
                        break;
                    case SMART_ACTION_CALL_TIMED_ACTIONLIST:
                        followList(holder.action.timedActionList.id, difficulties);
                        break;
                    case SMART_ACTION_CALL_RANDOM_TIMED_ACTIONLIST:
                        for (uint32 listId : holder.action.randTimedActionList.actionLists)
                            followList(listId, difficulties);
                        break;
                    case SMART_ACTION_CALL_RANDOM_RANGE_TIMED_ACTIONLIST:
                    {
                        uint32 lo = holder.action.randRangeTimedActionList.idMin;
                        uint32 hi = std::min(holder.action.randRangeTimedActionList.idMax, lo + 20);
                        for (uint32 listId = lo; listId && listId <= hi; ++listId)
                            followList(listId, difficulties);
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        void ReadCreature(uint32 entry, std::unordered_map<std::string, ScriptData> const& scripts)
        {
            CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
            if (!cinfo)
                return;
            CreatureSpells& into = sCreatures[entry];

            if (cinfo->ScriptID)
            {
                auto itr = scripts.find(sObjectMgr->GetScriptName(cinfo->ScriptID));
                if (itr != scripts.end())
                {
                    for (uint32 spell : itr->second.spells)
                        AddSpell(into, spell, ALL_DIFFICULTIES, false);
                    for (uint32 summon : itr->second.summons)
                        AddSummon(into, summon);
                }
            }

            if (cinfo->AIName == "SmartAI")
            {
                std::unordered_set<uint32> visited;
                ReadSmartActions(into, sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE),
                    ALL_DIFFICULTIES, visited, 0);
            }

            for (uint32 spell : cinfo->spells)
                AddSpell(into, spell, ALL_DIFFICULTIES, false);
        }
    }

    bool IsListable(SpellInfo const* spell)
    {
        if (!spell || spell->IsPassive() || spell->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY))
            return false;
        char const* name = spell->SpellName[LOCALE_enUS];
        return name && *name;
    }

    uint32 TagsOf(SpellInfo const* spell)
    {
        uint32 tags = 0;
        switch (spell->Dispel)
        {
            case DISPEL_MAGIC:   tags |= TAG_MAGIC; break;
            case DISPEL_CURSE:   tags |= TAG_CURSE; break;
            case DISPEL_DISEASE: tags |= TAG_DISEASE; break;
            case DISPEL_POISON:  tags |= TAG_POISON; break;
            case DISPEL_ENRAGE:  tags |= TAG_ENRAGE; break;
            default: break;
        }

        int32 castTime = spell->CastTimeEntry ? spell->CastTimeEntry->CastTime : 0;
        if ((castTime > 0 || spell->IsChanneled()) && (spell->PreventionType & SPELL_PREVENTION_TYPE_SILENCE)
            && (spell->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT))
            tags |= TAG_INTERRUPT;

        if (spell->IsTargetingArea() || spell->HasAreaAuraEffect())
            tags |= TAG_AOE;

        bool heals = false;
        for (SpellEffectInfo const& effect : spell->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_HEAL:
                case SPELL_EFFECT_HEAL_PCT:
                case SPELL_EFFECT_HEAL_MAX_HEALTH:
                    heals = true;
                    break;
                case SPELL_EFFECT_SUMMON:
                    tags |= TAG_SUMMON;
                    break;
                case SPELL_EFFECT_KNOCK_BACK:
                case SPELL_EFFECT_KNOCK_BACK_DEST:
                case SPELL_EFFECT_PULL_TOWARDS:
                case SPELL_EFFECT_PULL_TOWARDS_DEST:
                    tags |= TAG_KNOCKBACK;
                    break;
                default:
                    break;
            }
            if (effect.IsAura(SPELL_AURA_PERIODIC_HEAL))
                heals = true;
            if (effect.IsAura(SPELL_AURA_MOD_STUN) || effect.IsAura(SPELL_AURA_MOD_FEAR) || effect.IsAura(SPELL_AURA_MOD_CONFUSE)
                || effect.IsAura(SPELL_AURA_MOD_CHARM) || effect.IsAura(SPELL_AURA_MOD_POSSESS) || effect.IsAura(SPELL_AURA_MOD_PACIFY_SILENCE))
                tags |= TAG_CC;
        }
        if (heals)
            tags |= TAG_HEAL;

        constexpr uint64 CC_MECHANICS = (1ULL << MECHANIC_CHARM) | (1ULL << MECHANIC_DISORIENTED) | (1ULL << MECHANIC_FEAR)
            | (1ULL << MECHANIC_SLEEP) | (1ULL << MECHANIC_STUN) | (1ULL << MECHANIC_FREEZE) | (1ULL << MECHANIC_KNOCKOUT)
            | (1ULL << MECHANIC_POLYMORPH) | (1ULL << MECHANIC_BANISH) | (1ULL << MECHANIC_HORROR) | (1ULL << MECHANIC_SAPPED);
        if (spell->GetAllEffectsMechanicMask() & CC_MECHANICS)
            tags |= TAG_CC;

        if (spell->IsPositive() && !heals)
            tags |= TAG_BUFF;
        return tags;
    }

    void Load(std::vector<uint32> const& baseEntries)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sCreatures.clear();

        std::unordered_map<std::string, ScriptData> scripts = ReadScriptTable();
        for (uint32 entry : baseEntries)
            ReadCreature(entry, scripts);

        uint32 learned = 0;
        if (QueryResult result = WorldDatabase.Query("SELECT Entry, Spell FROM mod_forever_dungeon_journal_learned"))
            do
            {
                Field* f = result->Fetch();
                auto itr = sCreatures.find(f[0].Get<uint32>());
                if (itr == sCreatures.end())
                    continue;
                AddSpell(itr->second, f[1].Get<uint32>(), ALL_DIFFICULTIES, true);
                ++learned;
            } while (result->NextRow());

        LOG_INFO("server.loading", ">> mod-forever-dungeon-journal: abilities of {} creatures ({} script names, {} learned)",
            sCreatures.size(), scripts.size(), learned);
    }

    std::vector<Ability> For(uint32 baseEntry, uint8 difficulty)
    {
        std::vector<Ability> out;
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sCreatures.find(baseEntry);
        if (itr == sCreatures.end())
            return out;

        std::unordered_set<std::string> names;
        for (Known const& k : itr->second.spells)
        {
            if (difficulty < MAX_DIFFICULTY && !(k.difficulties & (1 << difficulty)))
                continue;
            uint32 spellId = SpellForDifficulty(k.spell, difficulty);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || !IsListable(info))
                continue;
            // Ranks and copies of one ability under the same name show once.
            if (!names.insert(info->SpellName[LOCALE_enUS]).second)
                continue;
            Ability a;
            a.spell = spellId;
            a.tags = TagsOf(info) | (k.learned ? uint32(TAG_LEARNED) : 0u);
            if (info->IsChanneled())
                a.castMs = uint32(std::max<int32>(0, info->GetMaxDuration()));
            else if (info->CastTimeEntry && info->CastTimeEntry->CastTime > 0)
                a.castMs = uint32(info->CastTimeEntry->CastTime);
            out.push_back(a);
        }
        return out;
    }

    std::vector<uint32> const& Summons(uint32 baseEntry)
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sCreatures.find(baseEntry);
        return itr != sCreatures.end() ? itr->second.summons : sNoSummons;
    }

    bool Learn(uint32 baseEntry, uint32 spell)
    {
        spell = BaseSpell(spell);
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sCreatures.find(baseEntry);
        if (itr == sCreatures.end() || itr->second.seen.count(spell))
            return false;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info || !IsListable(info))
            return false;
        AddSpell(itr->second, spell, ALL_DIFFICULTIES, true);
        WorldDatabase.Execute("INSERT IGNORE INTO mod_forever_dungeon_journal_learned (Entry, Spell) VALUES ({}, {})", baseEntry, spell);
        return true;
    }

    uint32 Hash(uint32 seed)
    {
        std::lock_guard<std::mutex> guard(sLock);
        uint32 acc = 0;
        for (auto const& [entry, data] : sCreatures)
            for (Known const& k : data.spells)
                acc ^= (entry * 2654435761u) ^ (k.spell * 40503u) ^ (uint32(k.difficulties) << 28) ^ (k.learned ? 0x9E3779B9u : 0u);
        return seed ^ acc;
    }
}
