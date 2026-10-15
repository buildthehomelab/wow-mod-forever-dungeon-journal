#!/usr/bin/env python3
"""
Reads AzerothCore creature scripts (C++) and writes, per ScriptName, the spells the script casts
and the creatures it summons, as SQL for mod_forever_journal_script_spell, and the script's enrage
timer, as SQL for mod_forever_journal_script_enrage.

Boss abilities written in C++ live only in the script source, not in the database, so the journal
can't find them at run time. Run this against the core's scripts (and modules with boss scripts,
like mod-individual-progression) whenever they change:

    python3 tools/extract_script_spells.py \
        --out data/sql/db-world/updates/mod_forever_journal_script_spells.sql \
        /path/to/azerothcore/src/server/scripts \
        /path/to/mod-individual-progression/src

It is a heuristic reader, not a C++ parser:
  * constants come from every enum and #define in the file and the headers beside it;
  * a script's body is the struct/class registered under the script name (RegisterCreatureAI and
    its per-instance variants, or a CreatureScript class with its nested AI);
  * a spell counts when a SPELL_ constant is used on a line that casts (DoCast..., CastSpell,
    AddAura...) inside that body, or a literal number stands where the spell argument goes; a
    creature counts when an NPC_ constant is used on a line that summons;
  * a constant name reused in several enums of one file takes the definition nearest before the
    script;
  * an enrage timer is a ScheduleEnrageTimer(spell, time), a ScheduleEvent(EVENT_..., time) whose
    case casts a spell, or a scheduler Schedule(time, ...) whose task casts one, where the event or
    the spell is called BERSERK or ENRAGE, the time is one fixed value of a minute or more, the
    call is made when the boss is engaged (JustEngagedWith, EnterCombat, Reset or a function they
    call) and not under an if (heroic only, a hard mode), the event doesn't repeat, and nothing
    outside those functions (or the evade, death and reset hooks) resets, cancels, delays or
    reschedules it. A script with two different timers (a hard mode, a phase) gets
    none: the journal leaves out what it isn't sure of.
Spells the server finds to be passive or hidden are dropped again at load time, and the addon
skips spells without a client description, so a stray hit costs little.
"""

import argparse
import os
import re
import sys

CAST_LINE = re.compile(r"\b(DoCast\w*|CastSpell|CastCustomSpell|AddAura|DoCastSpell|CastAura)\b")
SUMMON_LINE = re.compile(r"\b(SummonCreature|DoSpawnCreature|DoSummon\w*|SummonTrigger)\b")
CONST = re.compile(r"\b([A-Z][A-Z0-9_]*)\s*=\s*(\d+)\b")
DEFINE = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+(\d+)\b", re.M)
SPELL_IDENT = re.compile(r"\b(SPELL_[A-Z0-9_]+)\b")
NPC_IDENT = re.compile(r"\b((?:NPC|CREATURE)_[A-Z0-9_]+)\b")
# A literal spell id, only where a spell goes: the first argument of DoCast<Self/AOE/Victim...>(id)
# or AddAura(id), or the second of DoCast(target, id) / CastSpell(target, id). Other numbers in a cast
# (coordinates, ranges, SelectTarget and irand arguments) are left alone.
CAST_NUMBER_FIRST = re.compile(r"\b(?:DoCast\w*|AddAura)\s*\(\s*(\d{2,6})\s*[,)]")
CAST_NUMBER_SECOND = re.compile(r"\b(?:DoCast|CastSpell|AddAura)\s*\(\s*[A-Za-z_][\w>.:\-]*(?:\(\))?\s*,\s*(\d{2,6})\s*[,)]")
SUMMON_NUMBER = re.compile(r"\b(?:SummonCreature|DoSpawnCreature)\s*\(\s*(\d{3,7})\b")

ENRAGE_NAME = re.compile(r"BERSERK|ENRAGE")
DURATION = re.compile(r"^(\d+)\s*(ms|s|min|h)?$|^(Milliseconds|Seconds|Minutes)\s*\(\s*(\d+)\s*\)$")
DURATION_UNITS = {None: 0.001, "ms": 0.001, "s": 1, "min": 60, "h": 3600,
                  "Milliseconds": 0.001, "Seconds": 1, "Minutes": 60}
ENRAGE_CALL = re.compile(r"\b(ScheduleEnrageTimer|ScheduleEvent|Schedule)\s*\(")
FUNCTION = re.compile(r"\b(\w+)\s*\([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*\)\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?\{")
NOT_FUNCTIONS = {"if", "for", "while", "switch", "catch", "return", "sizeof"}
# Events scheduled in Reset only start counting in combat, like those of JustEngagedWith.
ENGAGE = {"JustEngagedWith", "EnterCombat", "JustEngaged", "Reset"}
# Where clearing the events can't touch a running enrage timer.
OUT_OF_FIGHT = {"EnterEvadeMode", "JustDied", "JustReachedHome", "InitializeAI", "Initialize"}
EVENTS_CLEARED = re.compile(r"\b\w*[eE]vents\s*\.\s*Reset\s*\(\s*\)|\bscheduler\s*\.\s*(?:CancelAll|DelayAll)\s*\(")
DELAY_EVENTS = re.compile(r"\bDelayEvents\s*\(")
# Code that ends the fight (the boss is beaten or plays its outro): clearing events there is fine.
FIGHT_OVER = re.compile(r"damage\s*>=\s*me->GetHealth\(\)|BossState\s*\(\s*\w+\s*,\s*DONE\s*\)|PHASE_OUTRO|"
                        r"CombatStop\s*\(|FACTION_FRIENDLY|SetIsCombatDisallowed\s*\(\s*true")
MIN_ENRAGE_SECONDS = 60

REGISTER_AI = re.compile(r"\bRegister\w*CreatureAI\s*\(\s*(\w+)")
REGISTER_FACTORY = re.compile(r"\bRegister\w*CreatureAIWithFactory\s*\(\s*(\w+)\s*,\s*(\w+)")
CREATURE_SCRIPT = re.compile(r"(?:class|struct)\s+(\w+)\s*(?:final\s*)?:\s*public\s+CreatureScript\b")
SCRIPT_NAME_CTOR = re.compile(r"CreatureScript\s*\(\s*\"(\w+)\"")
TYPE_DECL = re.compile(r"(?:struct|class)\s+(\w+)\s*(?:final\s*)?:\s*(?:public\s+)?\w")
# new npc_x_script() / new CreatureAILoader<x>("name") style registrations
NEW_SCRIPT = re.compile(r"\bnew\s+(\w+)\s*\(\s*\)")
LOADER = re.compile(r"\bnew\s+\w*CreatureAILoader\s*<\s*(\w+)\s*>\s*\(\s*\"(\w+)\"")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body_of(text, start):
    """The text between the first '{' at or after start and its matching '}'."""
    open_at = text.find("{", start)
    if open_at < 0:
        return ""
    depth = 0
    for i in range(open_at, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_at:i + 1]
    return text[open_at:]


def span_of(text, start):
    """(index of the first '{' at or after start, index of its matching '}')."""
    open_at = text.find("{", start)
    if open_at < 0:
        return None
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return open_at, i
    return open_at, len(text)


def call_args(text, open_paren):
    """The top-level arguments of the call whose '(' is at open_paren, and the index after ')'."""
    args, depth, start = [], 0, open_paren + 1
    for i in range(open_paren, len(text)):
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i].strip())
                return args, i + 1
        elif c == "," and depth == 1:
            args.append(text[start:i].strip())
            start = i + 1
    return args, len(text)


def seconds_of(arg):
    """A fixed duration ("10min", "600000", "Seconds(90)") in seconds, else None."""
    m = DURATION.match(arg.strip())
    if not m:
        return None
    if m.group(1):
        return int(m.group(1)) * DURATION_UNITS[m.group(2)]
    return int(m.group(4)) * DURATION_UNITS[m.group(3)]


def constants(paths):
    out = {}
    for path in paths:
        try:
            text = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        except OSError:
            continue
        for name, value in CONST.findall(text):
            out.setdefault(name, int(value))
        for name, value in DEFINE.findall(text):
            out.setdefault(name, int(value))
    return out


def positioned_constants(text):
    """name -> [(position, value)] for a file whose scripts may reuse a name in different enums."""
    out = {}
    for m in CONST.finditer(text):
        out.setdefault(m.group(1), []).append((m.start(), int(m.group(2))))
    for m in DEFINE.finditer(text):
        out.setdefault(m.group(1), []).append((m.start(), int(m.group(2))))
    return out


def constants_for(at, positioned, headers):
    """The constants a script at position `at` sees: per name, the last definition before it in the
    file (else the first after it), then the headers'."""
    out = dict(headers)
    for name, defs in positioned.items():
        before = [v for pos, v in defs if pos < at]
        out[name] = before[-1] if before else defs[0][1]
    return out


def scan_body(body, consts):
    spells, summons = [], []
    for line in body.split("\n"):
        if CAST_LINE.search(line):
            for ident in SPELL_IDENT.findall(line):
                if ident in consts:
                    spells.append(consts[ident])
            for number in CAST_NUMBER_FIRST.findall(line) + CAST_NUMBER_SECOND.findall(line):
                spells.append(int(number))
        if SUMMON_LINE.search(line):
            for ident in NPC_IDENT.findall(line):
                if ident in consts:
                    summons.append(consts[ident])
            for number in SUMMON_NUMBER.findall(line):
                summons.append(int(number))
    return spells, summons


def scan_enrage(body, consts):
    """The script's enrage timers, as a set of (spell, seconds)."""
    functions = []
    for m in FUNCTION.finditer(body):
        if m.group(1) not in NOT_FUNCTIONS:
            span = span_of(body, m.end() - 1)
            if span:
                functions.append((m.group(1), span))

    engage = set(ENGAGE)
    for name, (lo, hi) in functions:
        if name in ENGAGE:
            for other, _ in functions:
                if re.search(r"\b" + other + r"\s*\(", body[lo:hi]):
                    engage.add(other)

    def conditional(pos, lo):
        """Is pos under an if, else or switch inside the function body starting at lo?"""
        def statement_before(at):
            start = max(body.rfind(c, lo, at) for c in ";{}")
            return body[start + 1:at]
        if re.search(r"\b(if|else|switch)\b", statement_before(pos)):
            return True
        depth = 0
        for i in range(pos - 1, lo, -1):
            if body[i] == "}":
                depth += 1
            elif body[i] == "{":
                if depth == 0:
                    if re.search(r"\b(if|else|switch)\b", statement_before(i)):
                        return True
                else:
                    depth -= 1
        return False

    def function_at(pos):
        inner = None
        for name, (lo, hi) in functions:
            if lo < pos < hi and (inner is None or lo > inner[1]):
                inner = (name, lo)
        return inner

    def on_engage(pos):
        inner = function_at(pos)
        return inner is not None and inner[0] in engage and not conditional(pos, inner[1])

    def in_fight(pos):
        """Is pos somewhere that runs during the fight (UpdateAI, DoAction, a hook...) and not
        where it ends?"""
        inner = function_at(pos)
        if inner is None:
            return True
        return inner[0] not in engage | OUT_OF_FIGHT and not FIGHT_OVER.search(body[inner[1]:pos])

    # Event and task timers go when the fight clears or delays them all: events.Reset() at a phase
    # change cancels an enrage that never fires (Kel'Thuzad's). ScheduleEnrageTimer's runs on the
    # creature's own event list, which neither touches.
    cleared = any(in_fight(m.start()) for m in EVENTS_CLEARED.finditer(body))
    for m in DELAY_EVENTS.finditer(body):
        args, _ = call_args(body, m.end() - 1)
        if in_fight(m.start()) and (len(args) < 2 or args[1] == "0"):
            cleared = True

    def touched(name, calls):
        """Does the fight cancel, delay or reschedule this event, task group or event group?"""
        pattern = r"\b(?:" + calls + r")\s*\(\s*" + re.escape(name) + r"\b"
        return any(in_fight(m.start()) for m in re.finditer(pattern, body))

    def spell_of(arg):
        arg = arg.strip()
        if arg.isdigit():
            return arg, int(arg)
        return arg, consts.get(arg)

    def case_spells(event):
        """Spells cast where the event runs, or None when the event repeats."""
        out = []
        for m in re.finditer(r"\bcase\s+" + event + r"\s*:", body):
            rest = body[m.end():]
            stop = re.search(r"\bcase\s+\w+\s*:|\bdefault\s*:|\bbreak\s*;|\breturn\b", rest)
            segment = rest[:stop.start()] if stop else rest
            if re.search(r"\bScheduleEvent\s*\(\s*" + event + r"\b|\bRepeat(?:Event)?\s*\(", segment):
                return None
            for line in segment.split("\n"):
                if CAST_LINE.search(line):
                    out.extend((ident, consts[ident]) for ident in SPELL_IDENT.findall(line) if ident in consts)
        return out

    found = set()
    events = {}
    for m in ENRAGE_CALL.finditer(body):
        args, end = call_args(body, m.end() - 1)
        kind = m.group(1)
        if kind == "ScheduleEnrageTimer" and len(args) >= 2:
            name, spell = spell_of(args[0])
            seconds = seconds_of(args[1])
            if spell and seconds and on_engage(m.start()):
                found.add((spell, seconds))
        elif cleared:
            continue
        elif kind == "ScheduleEvent" and len(args) >= 2 and re.match(r"^\w+$", args[0]):
            # A second time is a random range: not one fixed timer.
            fixed = len(args) < 3 or seconds_of(args[2]) is None
            group = args[2] if len(args) >= 3 and fixed else ""
            if group and touched(group, "CancelEventGroup|DelayEvents|DelayEventsToMax"):
                fixed = False
            events.setdefault(args[0], []).append((seconds_of(args[1]) if fixed else None, on_engage(m.start())))
        elif kind == "Schedule" and len(args) >= 2:
            seconds = seconds_of(args[0])
            task = args[-1]
            if seconds is None or "{" not in task or re.search(r"\bRepeat\s*\(", task):
                continue
            if len(args) >= 3 and re.match(r"^\w+$", args[1]) and seconds_of(args[1]) is None \
                    and touched(args[1], "CancelGroup|DelayGroup|RescheduleGroup"):
                continue
            spells = []
            for line in task.split("\n"):
                if CAST_LINE.search(line):
                    spells.extend((ident, consts[ident]) for ident in SPELL_IDENT.findall(line) if ident in consts)
            named = [s for s in spells if ENRAGE_NAME.search(s[0])]
            if len(set(named)) == 1 and on_engage(m.start()):
                found.add((named[0][1], seconds))

    for event, sites in events.items():
        delays = set(seconds for seconds, _ in sites)
        if len(delays) != 1 or None in delays or not any(engaged for _, engaged in sites):
            continue
        if touched(event, "CancelEvent|RescheduleEvent|DelayEvent"):
            continue
        spells = case_spells(event)
        if not spells:
            continue
        named = [s for s in spells if ENRAGE_NAME.search(s[0])] if not ENRAGE_NAME.search(event) else spells
        if len(set(spell for _, spell in named)) == 1:
            found.add((named[0][1], delays.pop()))

    return set((spell, int(seconds)) for spell, seconds in found if seconds >= MIN_ENRAGE_SECONDS)


def scripts_in(path, headers):
    raw = open(path, encoding="utf-8", errors="replace").read()
    if "Creature" not in raw:
        return {}
    text = strip_comments(raw)
    header_consts = constants(headers)
    positioned = positioned_constants(text)

    decls = {}
    for m in TYPE_DECL.finditer(text):
        decls.setdefault(m.group(1), m.start())

    found = {}

    def add(script, type_name):
        if type_name not in decls:
            return
        body = body_of(text, decls[type_name])
        consts = constants_for(decls[type_name], positioned, header_consts)
        spells, summons = scan_body(body, consts)
        entry = found.setdefault(script, ([], [], set()))
        entry[0].extend(spells)
        entry[1].extend(summons)
        entry[2].update(scan_enrage(body, consts))

    for m in REGISTER_AI.finditer(text):
        add(m.group(1), m.group(1))
    for m in REGISTER_FACTORY.finditer(text):
        add(m.group(1), m.group(1))
    for m in LOADER.finditer(text):
        add(m.group(2), m.group(1))
    for m in CREATURE_SCRIPT.finditer(text):
        cls = m.group(1)
        body = body_of(text, m.start())
        name = SCRIPT_NAME_CTOR.search(body)
        if name:
            add(name.group(1), cls)
    return found


def walk(roots):
    for root in roots:
        for dirpath, _, files in os.walk(root):
            headers = [os.path.join(dirpath, f) for f in files if f.endswith(".h")]
            for f in sorted(files):
                if f.endswith(".cpp"):
                    yield os.path.join(dirpath, f), headers


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("roots", nargs="+", help="directories with creature scripts")
    ap.add_argument("--out", required=True, help="SQL file to write")
    args = ap.parse_args()

    result = {}
    for path, headers in walk(args.roots):
        for script, (spells, summons, enrage) in scripts_in(path, headers).items():
            entry = result.setdefault(script, ([], [], set()))
            entry[0].extend(spells)
            entry[1].extend(summons)
            entry[2].update(enrage)

    rows = []
    enrage_rows = []
    for script in sorted(result):
        spells, summons, enrage = result[script]
        if len(enrage) == 1:
            enrage_rows.append((script,) + next(iter(enrage)))
        seen = set()
        order = 0
        for kind, values in ((0, spells), (1, summons)):
            for value in values:
                if value <= 0 or (kind, value) in seen:
                    continue
                seen.add((kind, value))
                rows.append((script, kind, value, order))
                order += 1

    with open(args.out, "w", encoding="utf-8") as out:
        out.write("-- mod-forever-journal: spells cast and creatures summoned by C++ creature scripts.\n")
        out.write("-- Generated by tools/extract_script_spells.py; do not edit by hand.\n")
        out.write("-- Kind 0 = spell, 1 = summoned creature entry. Sort = order in the source.\n\n")
        out.write("CREATE TABLE IF NOT EXISTS `mod_forever_journal_script_spell` (\n"
                  "  `ScriptName` VARCHAR(64) NOT NULL,\n"
                  "  `Kind` TINYINT UNSIGNED NOT NULL DEFAULT 0,\n"
                  "  `Value` INT UNSIGNED NOT NULL,\n"
                  "  `Sort` SMALLINT UNSIGNED NOT NULL DEFAULT 0,\n"
                  "  PRIMARY KEY (`ScriptName`, `Kind`, `Value`)\n"
                  ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: abilities from C++ scripts';\n\n")
        out.write("DELETE FROM `mod_forever_journal_script_spell`;\n")
        for i in range(0, len(rows), 500):
            chunk = rows[i:i + 500]
            out.write("INSERT INTO `mod_forever_journal_script_spell` (`ScriptName`, `Kind`, `Value`, `Sort`) VALUES\n")
            out.write(",\n".join("('%s', %d, %d, %d)" % r for r in chunk))
            out.write(";\n")

        out.write("\n-- Enrage timers: the spell a script casts after Seconds of combat.\n\n")
        out.write("CREATE TABLE IF NOT EXISTS `mod_forever_journal_script_enrage` (\n"
                  "  `ScriptName` VARCHAR(64) NOT NULL,\n"
                  "  `Spell` INT UNSIGNED NOT NULL,\n"
                  "  `Seconds` INT UNSIGNED NOT NULL,\n"
                  "  PRIMARY KEY (`ScriptName`)\n"
                  ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-forever-journal: enrage timers from C++ scripts';\n\n")
        out.write("DELETE FROM `mod_forever_journal_script_enrage`;\n")
        if enrage_rows:
            out.write("INSERT INTO `mod_forever_journal_script_enrage` (`ScriptName`, `Spell`, `Seconds`) VALUES\n")
            out.write(",\n".join("('%s', %d, %d)" % r for r in enrage_rows))
            out.write(";\n")
    print("%d scripts, %d rows, %d enrage timers -> %s" % (len(result), len(rows), len(enrage_rows), args.out),
          file=sys.stderr)


if __name__ == "__main__":
    main()
