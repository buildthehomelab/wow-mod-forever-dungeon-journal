/*
 * mod-forever-journal: the spells a creature uses, for the journal's Abilities tab.
 *
 * Four sources, in this order:
 *   1. its C++ script: mod_forever_journal_script_spell, generated from the scripts' source by
 *      tools/extract_script_spells.py (keyed by ScriptName);
 *   2. its SmartAI script, following the timed action lists it calls; events limited to some
 *      difficulties only show on those;
 *   3. the creature's own spell list (creature_template_spell);
 *   4. what it was seen casting (mod_forever_journal_learned, filled by OnCreatureCast), on the
 *      difficulty it was seen on.
 * Then mod_forever_journal_ability hides, adds, orders, flags and annotates spells by hand.
 * A spell with a SpellDifficulty entry is shown as its version for the tab's difficulty.
 *
 * The server drops passive and hidden spells. The client shows a spell's own tooltip text; for a
 * spell whose tooltip has none (Spell.dbc's description, which the core doesn't load, is read
 * here) the server writes a short one from the spell's effects. Such spells named like internal
 * helpers (teleports, visuals, dummies, vehicle rides...) are left out, as are those nothing can
 * be said about.
 *
 * The enrage timer comes from mod_forever_journal_script_enrage (generated with the script
 * spells) or a SmartAI event that casts a Berserk or Enrage spell once, a fixed time into combat.
 */

#include "Journal.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Journal::Abilities
{
    namespace
    {
        constexpr uint8 ALL_DIFFICULTIES = 0x0F;
        constexpr uint8 DIFFICULTY_ANY = 255;

        // Spell.dbc in 3.3.5a: 234 four-byte fields; the description is field 170, one per locale.
        constexpr uint32 DBC_SPELL_FIELDS = 234;
        constexpr uint32 DBC_SPELL_DESCRIPTION = 170;
        constexpr uint32 DBC_LOCALES = 16;

        // A SmartAI event casting Berserk or Enrage once, at least this far into combat, is an
        // enrage timer; earlier ones are soft enrages that come and go.
        constexpr uint32 MIN_ENRAGE_MS = 60000;

        // Server-written descriptions stay short: they travel in one row of an addon message.
        constexpr std::size_t MAX_TEXT = 150;

        struct Known
        {
            uint32 spell = 0;
            uint8 difficulties = ALL_DIFFICULTIES; // bit per map difficulty
            bool learned = false;
            bool forced = false;                   // added by mod_forever_journal_ability
        };

        struct CreatureSpells
        {
            std::vector<Known> spells;
            std::unordered_set<uint32> seen;
            std::vector<uint32> summons;
            uint32 enrageSpell = 0;
            uint32 enrageSeconds = 0;
            bool enrageUnsure = false;             // two different timers: none is shown
        };

        struct Override
        {
            bool hide = false;
            uint16 sort = 0;
            uint32 flags = 0;
            std::string note;
        };

        std::unordered_map<uint32, CreatureSpells> sCreatures;
        std::vector<uint32> const sNoSummons;

        // (creature, base spell) -> the hand-made changes; creature 0 is every creature.
        std::map<std::pair<uint32, uint32>, Override> sOverrides;

        // Spells whose client tooltip has a description. Read once; when Spell.dbc can't be read
        // every spell counts as described and nothing is written for any.
        std::unordered_set<uint32> sDescribed;
        // What the server wrote for the others, as requests ask for them.
        std::unordered_map<uint32, std::string> sTexts;
        bool sDescriptionsRead = false;
        bool sDescriptionsTried = false;

        // Learning runs on map threads; requests on the world thread.
        std::mutex sLock;

        struct ScriptData
        {
            std::vector<uint32> spells;
            std::vector<uint32> summons;
            uint32 enrageSpell = 0;
            uint32 enrageSeconds = 0;
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

        void AddSpell(CreatureSpells& into, uint32 spellId, uint8 difficulties, bool learned, bool forced = false)
        {
            if (!spellId || !difficulties)
                return;
            spellId = BaseSpell(spellId);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || (forced ? !info->SpellName[LOCALE_enUS] || !*info->SpellName[LOCALE_enUS] : !IsListable(info)))
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
                        if (forced)
                            k.forced = true;
                    }
                return;
            }
            into.seen.insert(spellId);
            into.spells.push_back({ spellId, difficulties, learned, forced });
        }

        void AddSummon(CreatureSpells& into, uint32 entry)
        {
            if (entry && sObjectMgr->GetCreatureTemplate(entry)
                && std::find(into.summons.begin(), into.summons.end(), entry) == into.summons.end())
                into.summons.push_back(entry);
        }

        void SetEnrage(CreatureSpells& into, uint32 spell, uint32 seconds)
        {
            if (!spell || !seconds || !sSpellMgr->GetSpellInfo(spell))
                return;
            if (into.enrageSeconds && (into.enrageSeconds != seconds || into.enrageSpell != spell))
                into.enrageUnsure = true;
            into.enrageSpell = spell;
            into.enrageSeconds = seconds;
        }

        std::unordered_map<std::string, ScriptData> ReadScriptTable()
        {
            std::unordered_map<std::string, ScriptData> scripts;
            QueryResult result = WorldDatabase.Query("SELECT ScriptName, Kind, Value FROM mod_forever_journal_script_spell "
                "ORDER BY ScriptName, Sort");
            if (!result)
            {
                LOG_WARN("server.loading", ">> mod-forever-journal: mod_forever_journal_script_spell is empty; "
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

            if (QueryResult enrage = WorldDatabase.Query("SELECT ScriptName, Spell, Seconds FROM mod_forever_journal_script_enrage"))
                do
                {
                    Field* f = enrage->Fetch();
                    auto itr = scripts.find(f[0].Get<std::string>());
                    if (itr == scripts.end())
                        continue;
                    itr->second.enrageSpell = f[1].Get<uint32>();
                    itr->second.enrageSeconds = f[2].Get<uint32>();
                } while (enrage->NextRow());
            return scripts;
        }

        void ReadOverrides()
        {
            sOverrides.clear();
            QueryResult result = WorldDatabase.Query("SELECT Entry, Spell, Hide, Sort, Flags, Note FROM mod_forever_journal_ability");
            if (!result)
                return;
            do
            {
                Field* f = result->Fetch();
                Override& o = sOverrides[{ f[0].Get<uint32>(), BaseSpell(f[1].Get<uint32>()) }];
                o.hide = f[2].Get<uint8>() != 0;
                o.sort = f[3].Get<uint16>();
                o.flags = f[4].Get<uint32>();
                o.note = f[5].Get<std::string>();
            } while (result->NextRow());
        }

        Override const* OverrideOf(uint32 entry, uint32 spell)
        {
            auto itr = sOverrides.find({ entry, spell });
            if (itr == sOverrides.end())
                itr = sOverrides.find({ 0, spell });
            return itr != sOverrides.end() ? &itr->second : nullptr;
        }

        void ReadDescriptions()
        {
            sDescriptionsTried = true;
            std::string path = sWorld->GetDataPath() + "dbc/Spell.dbc";
            std::ifstream in(path, std::ios::binary);
            uint32 header[5] = {};
            if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "WDBC", 4) != 0
                || header[2] != DBC_SPELL_FIELDS || header[3] != DBC_SPELL_FIELDS * 4)
            {
                LOG_WARN("server.loading", ">> mod-forever-journal: can't read {}; abilities without a client description "
                    "stay hidden", path);
                return;
            }
            std::vector<uint32> record(DBC_SPELL_FIELDS);
            for (uint32 i = 0; i < header[1]; ++i)
            {
                if (!in.read(reinterpret_cast<char*>(record.data()), header[3]))
                {
                    LOG_WARN("server.loading", ">> mod-forever-journal: {} is cut short; abilities without a client "
                        "description stay hidden", path);
                    sDescribed.clear();
                    return;
                }
                // An empty string is offset 0 in the string block.
                for (uint32 l = 0; l < DBC_LOCALES; ++l)
                    if (record[DBC_SPELL_DESCRIPTION + l])
                    {
                        sDescribed.insert(record[0]);
                        break;
                    }
            }
            sDescriptionsRead = true;
        }

        // ---- descriptions for spells the client has none for -----------------------------------

        // Internal helpers, by name: spells without a tooltip with one of these words or phrases
        // in their name are never listed.
        char const* const HELPER_NAMES[] = { "teleport", "visual", "dummy", "anchor", "ride vehicle", "trigger", "bunny",
            "clearall", "clearalldebuffs", "clear all", "clear debuffs", "cosmetic", "freeze anim", "credit", "achievement",
            "camera shake", "spawn-in", "despawn", "stalker", "dnd", "submerge", "submerged", "emerge", "transform",
            "synch health", "sync health", "kill self", "suicide" };

        bool IsHelperName(std::string_view name)
        {
            std::string lower(name);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            auto letter = [&](std::size_t at) { return at < lower.size() && std::isalnum(static_cast<unsigned char>(lower[at])); };
            for (std::string_view helper : HELPER_NAMES)
                for (std::size_t at = lower.find(helper); at != std::string::npos; at = lower.find(helper, at + 1))
                    if ((at == 0 || !letter(at - 1)) && !letter(at + helper.size()))
                        return true; // a whole word: "Kill Credit", not "Discredit"
            return false;
        }

        std::string SchoolName(uint32 mask)
        {
            static char const* const NAMES[] = { "Physical", "Holy", "Fire", "Nature", "Frost", "Shadow", "Arcane" };
            for (uint32 i = 0; i < MAX_SPELL_SCHOOL; ++i)
                if (mask == (1u << i))
                    return NAMES[i];
            return "";
        }

        // "1.5 sec", "20 sec", "5 min", "5.5 min".
        std::string TimeText(int32 ms)
        {
            auto trim = [](float v)
            {
                std::string s = Acore::StringFormat("{:.1f}", v);
                return s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0 ? s.substr(0, s.size() - 2) : s;
            };
            if (ms < 60000)
                return trim(ms / 1000.0f) + " sec";
            return trim(ms / 60000.0f) + " min";
        }

        // What an effect's points come to: "500", "6125 to 7875", or nothing for the 1 point a
        // scripted spell keeps in its data. No thousands separator: the text travels inside comma
        // separated rows.
        std::string AmountText(SpellEffectInfo const& effect)
        {
            int32 lo = effect.BasePoints;
            int32 hi = effect.BasePoints;
            if (effect.DieSides)
            {
                lo += std::min(1, effect.DieSides);
                hi += std::max(1, effect.DieSides);
            }
            if (std::max(std::abs(lo), std::abs(hi)) <= 1)
                return "";
            return lo == hi ? std::to_string(lo) : Acore::StringFormat("{} to {}", lo, hi);
        }

        // "Inflicts 500 Fire damage", "Inflicts Fire damage": the words around an amount that may
        // be missing.
        std::string AmountPhrase(std::string_view before, SpellEffectInfo const& effect, std::string_view after)
        {
            std::string amount = AmountText(effect);
            std::string out(before);
            if (!amount.empty())
                out += " " + amount;
            if (!after.empty())
                out += " " + std::string(after);
            return out;
        }

        std::string SchoolDamage(std::string const& school)
        {
            return school.empty() ? "damage" : school + " damage";
        }

        int32 AmountOf(SpellEffectInfo const& effect)
        {
            return effect.BasePoints + (effect.DieSides ? std::max(1, effect.DieSides) : 0);
        }

        std::string AreaText(SpellEffectInfo const& effect)
        {
            if (!effect.HasRadius() || !(effect.IsTargetingArea() || effect.IsAreaAuraEffect()))
                return "";
            float radius = effect.CalcRadius();
            if (radius < 1.0f)
                return "";
            return Acore::StringFormat(" within {} yards", uint32(std::lround(radius)));
        }

        // "Increases damage taken by 50%", "Reduces damage taken by 50%".
        std::string ChangeText(int32 amount, std::string_view what)
        {
            return Acore::StringFormat("{} {} by {}%", amount >= 0 ? "Increases" : "Reduces", what, std::abs(amount));
        }

        std::string Describe(SpellInfo const* spell, uint32 depth);

        // Harm that a spell aimed at the caster alone does to it (a scripted kill or stun between
        // phases) says nothing about the fight; its own buffs do.
        bool IsSelfHarm(SpellEffectInfo const& effect)
        {
            if (effect.TargetA.GetTarget() != TARGET_UNIT_CASTER || effect.IsTargetingArea() || effect.IsAreaAuraEffect())
                return false;
            switch (effect.Effect)
            {
                case SPELL_EFFECT_INSTAKILL:
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
                case SPELL_EFFECT_HEALTH_LEECH:
                case SPELL_EFFECT_POWER_DRAIN:
                case SPELL_EFFECT_POWER_BURN:
                case SPELL_EFFECT_KNOCK_BACK:
                case SPELL_EFFECT_INTERRUPT_CAST:
                    return true;
                default:
                    break;
            }
            switch (effect.IsAura() ? effect.ApplyAuraName : SPELL_AURA_NONE)
            {
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                case SPELL_AURA_PERIODIC_LEECH:
                case SPELL_AURA_PERIODIC_MANA_LEECH:
                case SPELL_AURA_POWER_BURN:
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_MOD_CHARM:
                case SPELL_AURA_MOD_POSSESS:
                case SPELL_AURA_MOD_ROOT:
                case SPELL_AURA_MOD_SILENCE:
                case SPELL_AURA_MOD_PACIFY:
                case SPELL_AURA_MOD_PACIFY_SILENCE:
                case SPELL_AURA_MOD_DISARM:
                case SPELL_AURA_MOD_TAUNT:
                    return true;
                default:
                    return false;
            }
        }

        // What an aura does, without its duration.
        std::string AuraText(SpellInfo const* spell, SpellEffectInfo const& effect, uint32 depth)
        {
            std::string school = SchoolName(spell->SchoolMask);
            std::string area = AreaText(effect);
            std::string every = effect.Amplitude ? " every " + TimeText(int32(effect.Amplitude)) : "";
            int32 amount = AmountOf(effect);
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_PERIODIC_DAMAGE:
                    return AmountPhrase("Inflicts", effect, SchoolDamage(school)) + every + area;
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                    return Acore::StringFormat("Inflicts {}% of maximum health as{} damage{}{}", amount,
                        school.empty() ? "" : " " + school, every, area);
                case SPELL_AURA_PERIODIC_LEECH:
                    return AmountPhrase("Drains", effect, "health") + every + area;
                case SPELL_AURA_PERIODIC_MANA_LEECH:
                case SPELL_AURA_POWER_BURN:
                    return AmountPhrase("Drains", effect, "mana") + every + area;
                case SPELL_AURA_PERIODIC_HEAL:
                    return AmountPhrase("Heals", effect, "") + every + area;
                case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                case SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
                {
                    if (depth > 0 || effect.TriggerSpell == spell->Id)
                        return "";
                    std::string then = Describe(sSpellMgr->GetSpellInfo(effect.TriggerSpell), depth + 1);
                    if (then.empty() || every.empty())
                        return "";
                    then[0] = char(std::tolower(static_cast<unsigned char>(then[0])));
                    return "Every " + TimeText(int32(effect.Amplitude)) + " " + then;
                }
                case SPELL_AURA_MOD_STUN: return "Stuns" + area;
                case SPELL_AURA_MOD_FEAR: return "Fears" + area;
                case SPELL_AURA_MOD_CONFUSE: return "Disorients" + area;
                case SPELL_AURA_MOD_CHARM:
                case SPELL_AURA_MOD_POSSESS: return "Takes control of its target";
                case SPELL_AURA_MOD_ROOT: return "Roots in place" + area;
                case SPELL_AURA_MOD_SILENCE: return "Silences" + area;
                case SPELL_AURA_MOD_PACIFY: return "Pacifies" + area;
                case SPELL_AURA_MOD_PACIFY_SILENCE: return "Pacifies and silences" + area;
                case SPELL_AURA_MOD_DISARM: return "Disarms" + area;
                case SPELL_AURA_MOD_TAUNT: return "Taunts" + area;
                case SPELL_AURA_REFLECT_SPELLS: return "Reflects spells";
                case SPELL_AURA_MOD_DECREASE_SPEED:
                case SPELL_AURA_MOD_SPEED_SLOW_ALL:
                case SPELL_AURA_MOD_INCREASE_SPEED:
                case SPELL_AURA_MOD_SPEED_ALWAYS:
                    return amount ? ChangeText(amount, "movement speed") + area : "";
                case SPELL_AURA_MOD_MELEE_HASTE:
                case SPELL_AURA_MOD_MELEE_RANGED_HASTE:
                case SPELL_AURA_MOD_ATTACKSPEED:
                    return amount ? ChangeText(amount, "attack speed") + area : "";
                case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
                case SPELL_AURA_HASTE_SPELLS:
                    return amount ? ChangeText(amount, "casting speed") + area : "";
                case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
                    return amount ? ChangeText(amount, "damage done") + area : "";
                case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                    return amount ? ChangeText(amount, "damage taken") + area : "";
                case SPELL_AURA_MOD_MELEE_DAMAGE_TAKEN_PCT:
                    return amount ? ChangeText(amount, "physical damage taken") + area : "";
                case SPELL_AURA_MOD_HEALING_PCT:
                    return amount ? ChangeText(amount, "healing received") + area : "";
                case SPELL_AURA_MOD_HEALING_DONE_PERCENT:
                    return amount ? ChangeText(amount, "healing done") + area : "";
                case SPELL_AURA_MOD_INCREASE_HEALTH_PERCENT:
                    return amount ? ChangeText(amount, "maximum health") : "";
                case SPELL_AURA_MOD_HIT_CHANCE:
                    return amount ? ChangeText(amount, "chance to hit") + area : "";
                case SPELL_AURA_SCHOOL_ABSORB:
                    return AmountPhrase("Absorbs", effect, "damage");
                case SPELL_AURA_DAMAGE_SHIELD:
                    return AmountPhrase("Inflicts", effect, SchoolDamage(school) + " on melee attackers");
                case SPELL_AURA_SPLIT_DAMAGE_PCT:
                    return Acore::StringFormat("Redirects {}% of the damage taken", amount);
                case SPELL_AURA_SCHOOL_IMMUNITY:
                {
                    std::string immune = SchoolName(uint32(effect.MiscValue));
                    if ((uint32(effect.MiscValue) & SPELL_SCHOOL_MASK_ALL) == SPELL_SCHOOL_MASK_ALL)
                        return "Immune to all damage";
                    return immune.empty() ? "" : "Immune to " + immune + " damage";
                }
                default:
                    return "";
            }
        }

        // One short description of what a spell does, from its effects; empty when nothing
        // useful can be said. depth > 0 for a spell another one triggers.
        std::string Describe(SpellInfo const* spell, uint32 depth)
        {
            if (!spell)
                return "";
            std::vector<std::string> parts;
            bool aura = false;
            bool periodicTrigger = false;
            auto add = [&](std::string text)
            {
                if (!text.empty() && std::find(parts.begin(), parts.end(), text) == parts.end())
                    parts.push_back(std::move(text));
            };
            std::string school = SchoolName(spell->SchoolMask);
            for (SpellEffectInfo const& effect : spell->GetEffects())
            {
                if (IsSelfHarm(effect))
                    continue;
                std::string area = AreaText(effect);
                switch (effect.Effect)
                {
                    case SPELL_EFFECT_SCHOOL_DAMAGE:
                    case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
                        add(AmountPhrase("Inflicts", effect, SchoolDamage(school)) + area);
                        break;
                    case SPELL_EFFECT_WEAPON_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                    case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                        add(AmountText(effect).empty() ? "Weapon damage" + area : "Weapon damage plus " + AmountText(effect) + area);
                        break;
                    case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                        add(Acore::StringFormat("{}% weapon damage{}", AmountOf(effect), area));
                        break;
                    case SPELL_EFFECT_HEALTH_LEECH:
                        add(AmountPhrase("Drains", effect, "health") + area);
                        break;
                    case SPELL_EFFECT_POWER_DRAIN:
                    case SPELL_EFFECT_POWER_BURN:
                        if (effect.MiscValue == POWER_MANA)
                            add(AmountPhrase("Drains", effect, "mana") + area);
                        break;
                    case SPELL_EFFECT_HEAL:
                        add(AmountPhrase("Heals", effect, "") + area);
                        break;
                    case SPELL_EFFECT_HEAL_PCT:
                        add(Acore::StringFormat("Heals {}% of maximum health{}", AmountOf(effect), area));
                        break;
                    case SPELL_EFFECT_HEAL_MAX_HEALTH:
                        add("Heals to full health");
                        break;
                    case SPELL_EFFECT_INSTAKILL:
                        add("Kills instantly" + area);
                        break;
                    case SPELL_EFFECT_SUMMON:
                        if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(uint32(effect.MiscValue)))
                            add("Summons " + t->Name);
                        break;
                    case SPELL_EFFECT_KNOCK_BACK:
                    case SPELL_EFFECT_KNOCK_BACK_DEST:
                        add("Knocks back" + area);
                        break;
                    case SPELL_EFFECT_PULL_TOWARDS:
                    case SPELL_EFFECT_PULL_TOWARDS_DEST:
                        add("Pulls in" + area);
                        break;
                    case SPELL_EFFECT_CHARGE:
                    case SPELL_EFFECT_CHARGE_DEST:
                        add("Charges");
                        break;
                    case SPELL_EFFECT_JUMP:
                    case SPELL_EFFECT_JUMP_DEST:
                    case SPELL_EFFECT_LEAP:
                    case SPELL_EFFECT_LEAP_BACK:
                        add("Leaps");
                        break;
                    case SPELL_EFFECT_INTERRUPT_CAST:
                        add("Interrupts spellcasting" + area);
                        break;
                    case SPELL_EFFECT_MODIFY_THREAT_PERCENT:
                        if (AmountOf(effect) <= -100)
                            add("Wipes threat" + area);
                        else if (AmountOf(effect) < 0)
                            add(ChangeText(AmountOf(effect), "threat") + area);
                        break;
                    case SPELL_EFFECT_TRIGGER_SPELL:
                    case SPELL_EFFECT_TRIGGER_MISSILE:
                    case SPELL_EFFECT_FORCE_CAST:
                    case SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE:
                        if (depth == 0 && effect.TriggerSpell != spell->Id)
                            add(Describe(sSpellMgr->GetSpellInfo(effect.TriggerSpell), depth + 1));
                        break;
                    default:
                        if (effect.IsAura())
                        {
                            std::string text = AuraText(spell, effect, depth);
                            if (!text.empty())
                            {
                                aura = true;
                                if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL
                                    || effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE)
                                    periodicTrigger = true;
                                add(std::move(text));
                            }
                        }
                        break;
                }
            }

            // Nothing to say about the effects (a scripted or dummy spell): at least what it hits,
            // for one cast at enemies.
            if (parts.empty() && depth == 0 && !spell->IsPositive())
                for (SpellEffectInfo const& effect : spell->GetEffects())
                {
                    if (!effect.IsEffect())
                        continue;
                    std::string area = AreaText(effect);
                    bool enemies = effect.TargetA.GetCheckType() == TARGET_CHECK_ENEMY || effect.TargetB.GetCheckType() == TARGET_CHECK_ENEMY
                        || effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_ENEMY;
                    if (!area.empty() && enemies)
                        add("Affects enemies" + area);
                    else if (effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ENEMY || effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ANY)
                        add("Affects its target");
                    else
                        continue;
                    aura = effect.IsAura();
                    break;
                }

            if (parts.empty())
                return "";

            int32 duration = spell->GetMaxDuration();
            if (aura && duration > 0 && depth == 0)
            {
                if (parts.size() == 1 && !periodicTrigger)
                    parts[0] += " for " + TimeText(duration);
                else
                    parts.push_back("Lasts " + TimeText(duration));
            }

            std::string out;
            for (std::string const& part : parts)
            {
                std::string next = out.empty() ? part : out + " " + part;
                if (!next.empty() && next.back() != '.')
                    next += '.';
                if (next.size() > MAX_TEXT)
                    break;
                out = std::move(next);
            }
            return out;
        }

        // The difficulties a SmartAI event runs on.
        uint8 EventDifficulties(SmartScriptHolder const& holder)
        {
            uint32 mask = (holder.event.event_flags & SMART_EVENT_FLAG_DIFFICULTY_ALL) >> 1;
            return mask ? uint8(mask & ALL_DIFFICULTIES) : ALL_DIFFICULTIES;
        }

        // An in-combat timer of every phase that fires once, at a fixed time, and casts a Berserk
        // or Enrage.
        void ReadSmartEnrage(CreatureSpells& into, SmartScriptHolder const& holder, uint32 spellId)
        {
            // An event of some phases only counts while the boss is in them, not from the pull.
            if (holder.GetEventType() != SMART_EVENT_UPDATE_IC || EventDifficulties(holder) != ALL_DIFFICULTIES
                || holder.event.event_phase_mask)
                return;
            auto const& timer = holder.event.minMaxRepeat;
            if (timer.min != timer.max || timer.min < MIN_ENRAGE_MS || timer.repeatMin || timer.repeatMax)
                return;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            char const* name = info ? info->SpellName[LOCALE_enUS] : nullptr;
            if (!name || (!std::strstr(name, "Berserk") && !std::strstr(name, "Enrage")))
                return;
            SetEnrage(into, spellId, timer.min / 1000);
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
                        if (depth == 0)
                            ReadSmartEnrage(into, holder, holder.action.cast.spell);
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
                    SetEnrage(into, itr->second.enrageSpell, itr->second.enrageSeconds);
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

        uint8 DifficultyMask(uint8 difficulty)
        {
            return difficulty < MAX_DIFFICULTY ? uint8(1 << difficulty) : ALL_DIFFICULTIES;
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
        sTexts.clear();
        if (!sDescriptionsTried)
            ReadDescriptions();

        std::unordered_map<std::string, ScriptData> scripts = ReadScriptTable();
        for (uint32 entry : baseEntries)
            ReadCreature(entry, scripts);

        uint32 learned = 0;
        if (QueryResult result = WorldDatabase.Query("SELECT Entry, Spell, Difficulty FROM mod_forever_journal_learned"))
            do
            {
                Field* f = result->Fetch();
                auto itr = sCreatures.find(f[0].Get<uint32>());
                if (itr == sCreatures.end())
                    continue;
                uint8 difficulty = f[2].Get<uint8>();
                if (difficulty != DIFFICULTY_ANY && difficulty >= MAX_DIFFICULTY)
                    continue;
                AddSpell(itr->second, f[1].Get<uint32>(), DifficultyMask(difficulty), true);
                ++learned;
            } while (result->NextRow());

        // Hand-made additions, after the automatic list. A spell the creature already has keeps
        // its difficulties; the row only changes how it shows.
        ReadOverrides();
        for (auto const& [key, o] : sOverrides)
            if (key.first && !o.hide)
                if (auto itr = sCreatures.find(key.first); itr != sCreatures.end() && !itr->second.seen.count(key.second))
                    AddSpell(itr->second, key.second, ALL_DIFFICULTIES, false, true);

        LOG_INFO("server.loading", ">> mod-forever-journal: abilities of {} creatures ({} script names, {} learned, {} overrides)",
            sCreatures.size(), scripts.size(), learned, sOverrides.size());
    }

    std::vector<Ability> For(uint32 baseEntry, uint8 difficulty)
    {
        std::vector<Ability> out;
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sCreatures.find(baseEntry);
        if (itr == sCreatures.end())
            return out;

        // Each with its place: its Sort, or after every sorted one in list order.
        constexpr uint32 UNSORTED = 0x10000;
        std::vector<std::pair<uint32, Ability>> ranked;
        std::unordered_map<std::string, std::size_t> names; // name -> its entry in ranked
        for (Known const& k : itr->second.spells)
        {
            if (difficulty < MAX_DIFFICULTY && !(k.difficulties & (1 << difficulty)))
                continue;
            Override const* o = OverrideOf(baseEntry, k.spell);
            if (o && o->hide)
                continue;
            uint32 spellId = SpellForDifficulty(k.spell, difficulty);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || (!k.forced && !IsListable(info)))
                continue;
            Ability a;
            a.spell = spellId;
            if (sDescriptionsRead && !sDescribed.count(spellId))
            {
                // The client has nothing to show: a helper, or a mechanic it gets a line for.
                if (!k.forced && IsHelperName(info->SpellName[LOCALE_enUS]))
                    continue;
                auto [text, fresh] = sTexts.try_emplace(spellId);
                if (fresh)
                    text->second = Describe(info, 0);
                a.text = text->second;
                if (a.text.empty() && !k.forced && !(o && !o->note.empty()))
                    continue;
            }
            // Ranks and copies of one ability under the same name show once, with the hand-made
            // changes made to any of them.
            auto [named, added] = names.emplace(info->SpellName[LOCALE_enUS], ranked.size());
            if (!added)
            {
                if (o)
                {
                    auto& [rank, kept] = ranked[named->second];
                    kept.tags |= (o->flags << ABILITY_FLAG_SHIFT) & TAG_OVERRIDE_MASK;
                    if (kept.note.empty())
                        kept.note = o->note;
                    if (o->sort && rank >= UNSORTED)
                        rank = o->sort;
                }
                continue;
            }
            a.tags = TagsOf(info) | (k.learned ? uint32(TAG_LEARNED) : 0u);
            if (o)
            {
                a.tags |= (o->flags << ABILITY_FLAG_SHIFT) & TAG_OVERRIDE_MASK;
                a.note = o->note;
            }
            if (info->IsChanneled())
                a.castMs = uint32(std::max<int32>(0, info->GetMaxDuration()));
            else if (info->CastTimeEntry && info->CastTimeEntry->CastTime > 0)
                a.castMs = uint32(info->CastTimeEntry->CastTime);
            ranked.emplace_back(o && o->sort ? o->sort : UNSORTED + uint32(ranked.size()), std::move(a));
        }

        std::stable_sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        out.reserve(ranked.size());
        for (auto& [rank, a] : ranked)
            out.push_back(std::move(a));
        return out;
    }

    bool EnrageOf(uint32 baseEntry, uint8 difficulty, uint32& spell, uint32& seconds)
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sCreatures.find(baseEntry);
        if (itr == sCreatures.end() || !itr->second.enrageSeconds || itr->second.enrageUnsure)
            return false;
        spell = SpellForDifficulty(BaseSpell(itr->second.enrageSpell), difficulty);
        seconds = itr->second.enrageSeconds;
        return true;
    }

    std::vector<uint32> const& Summons(uint32 baseEntry)
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sCreatures.find(baseEntry);
        return itr != sCreatures.end() ? itr->second.summons : sNoSummons;
    }

    bool Learn(uint32 baseEntry, uint32 spell, uint8 difficulty)
    {
        spell = BaseSpell(spell);
        uint8 mask = DifficultyMask(difficulty);
        std::lock_guard<std::mutex> guard(sLock);
        auto itr = sCreatures.find(baseEntry);
        if (itr == sCreatures.end())
            return false;
        if (itr->second.seen.count(spell))
            for (Known const& k : itr->second.spells)
                if (k.spell == spell && (k.difficulties & mask) == mask)
                    return false;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info || !IsListable(info))
            return false;
        AddSpell(itr->second, spell, mask, true);
        WorldDatabase.Execute("INSERT IGNORE INTO mod_forever_journal_learned (Entry, Spell, Difficulty) VALUES ({}, {}, {})",
            baseEntry, spell, difficulty < MAX_DIFFICULTY ? difficulty : DIFFICULTY_ANY);
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
