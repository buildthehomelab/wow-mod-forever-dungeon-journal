#!/usr/bin/env python3
"""
Reads AzerothCore creature scripts (C++) and writes, per ScriptName, the spells the script casts
and the creatures it summons, as SQL for mod_dungeon_journal_script_spell.

Boss abilities written in C++ live only in the script source, not in the database, so the journal
can't find them at run time. Run this against the core's scripts (and modules with boss scripts,
like mod-individual-progression) whenever they change:

    python3 tools/extract_script_spells.py \
        --out data/sql/db-world/updates/mod_dungeon_journal_script_spells.sql \
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
    script.
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
        spells, summons = scan_body(body, constants_for(decls[type_name], positioned, header_consts))
        entry = found.setdefault(script, ([], []))
        entry[0].extend(spells)
        entry[1].extend(summons)

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
        for script, (spells, summons) in scripts_in(path, headers).items():
            entry = result.setdefault(script, ([], []))
            entry[0].extend(spells)
            entry[1].extend(summons)

    rows = []
    for script in sorted(result):
        spells, summons = result[script]
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
        out.write("-- mod-dungeon-journal: spells cast and creatures summoned by C++ creature scripts.\n")
        out.write("-- Generated by tools/extract_script_spells.py; do not edit by hand.\n")
        out.write("-- Kind 0 = spell, 1 = summoned creature entry. Sort = order in the source.\n\n")
        out.write("CREATE TABLE IF NOT EXISTS `mod_dungeon_journal_script_spell` (\n"
                  "  `ScriptName` VARCHAR(64) NOT NULL,\n"
                  "  `Kind` TINYINT UNSIGNED NOT NULL DEFAULT 0,\n"
                  "  `Value` INT UNSIGNED NOT NULL,\n"
                  "  `Sort` SMALLINT UNSIGNED NOT NULL DEFAULT 0,\n"
                  "  PRIMARY KEY (`ScriptName`, `Kind`, `Value`)\n"
                  ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dungeon-journal: abilities from C++ scripts';\n\n")
        out.write("DELETE FROM `mod_dungeon_journal_script_spell`;\n")
        for i in range(0, len(rows), 500):
            chunk = rows[i:i + 500]
            out.write("INSERT INTO `mod_dungeon_journal_script_spell` (`ScriptName`, `Kind`, `Value`, `Sort`) VALUES\n")
            out.write(",\n".join("('%s', %d, %d, %d)" % r for r in chunk))
            out.write(";\n")
    print("%d scripts, %d rows -> %s" % (len(result), len(rows), args.out), file=sys.stderr)


if __name__ == "__main__":
    main()
