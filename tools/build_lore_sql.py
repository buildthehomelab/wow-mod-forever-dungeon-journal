#!/usr/bin/env python3
"""Builds the Lore tab's SQL from data/lore/*.json.

Each JSON file is one zone:

    {
      "zone": 12,                         AreaTable zone id
      "name": "Elwynn Forest",            for the file name and the header only
      "entries": [
        {
          "id": 12000,                    zone * 1000 + n
          "kind": "zone",                 zone | place | person | history
          "title": "Elwynn Forest",
          "triggers": [                   what unlocks it, any of:
            {"area": "Goldshire"},        an area of this zone, by its name in AreaTable.dbc
            {"areaId": 87},               an area by id
            {"talk": 240},                talking to a creature (gossip or quest window)
            {"kill": 448},                killing one (with the group's kill credit)
            {"accept": 176},              taking a quest
            {"complete": 176}             turning one in
          ],
          "body": ["paragraph", "..."],
          "sources": ["what it was written from"],
          optional: "faction" ("alliance"/"horde"), "races" / "classes" (masks), "era" (stage)
        }
      ]
    }

Entries sort by kind, then by their order in the file. Writes one update per zone,
data/sql/db-world/updates/mod_forever_journal_lore_<name>.sql, which replaces that zone's
lore every time it changes (the server's updater applies a changed file again).

    python3 tools/build_lore_sql.py             builds every zone
    python3 tools/build_lore_sql.py --check     only checks the JSON

With --world-sql DIR (the core's data/sql/base/db_world) it also checks that every creature
and quest a trigger names exists. With --areas FILE (the server's dbc/AreaTable.dbc, or an
AreaTable csv export such as wago.tools') it also checks that every area a trigger names is in
its zone.
"""

import argparse
import json
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LORE = ROOT / "data" / "lore"
OUT = ROOT / "data" / "sql" / "db-world" / "updates"

KINDS = {"zone": 0, "place": 1, "person": 2, "history": 3}
FACTIONS = {None: 0, "alliance": 1, "horde": 2}
TRIGGERS = {"area": 1, "areaId": 1, "talk": 2, "kill": 3, "accept": 4, "complete": 5}


def sql_text(text):
    return "'" + text.replace("\\", "\\\\").replace("'", "''").replace("\n", "\\n") + "'"


def slug(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def ids_in(path):
    """The first column of every row of a mysqldump INSERT file."""
    if not path.exists():
        return None
    return set(int(m) for m in re.findall(r"\((\d+),", path.read_text(encoding="utf-8", errors="replace")))


def read_areas(path):
    """{area id: (name, parent zone id)} from AreaTable.dbc (3.3.5) or an AreaTable csv."""
    raw = path.read_bytes()
    areas = {}
    if raw[:4] == b"WDBC":
        count, fields, size, _ = struct.unpack_from("<4I", raw, 4)
        strings = 20 + count * size
        for r in range(count):
            row = struct.unpack_from(f"<{fields}I", raw, 20 + r * size)
            name = raw[strings + row[11]:raw.index(b"\0", strings + row[11])].decode("utf-8", "replace")
            areas[row[0]] = (name, row[2])
    else:
        import csv
        import io
        for row in csv.DictReader(io.StringIO(raw.decode("utf-8-sig"))):
            areas[int(row["ID"])] = (row.get("AreaName_lang") or row.get("AreaName"), int(row["ParentAreaID"]))
    return areas


def zone_areas(areas, zone):
    """Lower-case name -> id of the zone and every area inside it."""
    inside = {zone}
    grew = True
    while grew:
        grew = False
        for aid, (_, parent) in areas.items():
            if parent in inside and aid not in inside:
                inside.add(aid)
                grew = True
    return {areas[a][0].lower(): a for a in inside if a in areas}


def check(zone_file, data, creatures, quests, areas=None):
    problems = []
    zone = data.get("zone")
    if not isinstance(zone, int) or zone <= 0:
        problems.append("zone must be an AreaTable zone id")
        return problems
    if not data.get("entries"):
        problems.append("no entries")
    known_areas = zone_areas(areas, zone) if areas is not None else None
    seen = set()
    for i, e in enumerate(data.get("entries", [])):
        where = f"entry {e.get('id', '#' + str(i))}"
        eid = e.get("id")
        if not isinstance(eid, int) or not zone * 1000 <= eid < zone * 1000 + 1000:
            problems.append(f"{where}: id must be between {zone * 1000} and {zone * 1000 + 999}")
        if eid in seen:
            problems.append(f"{where}: id used twice")
        seen.add(eid)
        if e.get("kind") not in KINDS:
            problems.append(f"{where}: kind must be one of {', '.join(KINDS)}")
        title = e.get("title", "")
        if not title or len(title) > 80:
            problems.append(f"{where}: title must be 1-80 characters")
        body = e.get("body")
        if not isinstance(body, list) or not body or not all(isinstance(p, str) and p.strip() for p in body):
            problems.append(f"{where}: body must be a list of paragraphs")
        if e.get("faction") not in FACTIONS:
            problems.append(f"{where}: faction must be alliance, horde or left out")
        if not isinstance(e.get("era", 0), int) or not 0 <= e.get("era", 0) <= 18:
            problems.append(f"{where}: era must be a progression stage 0-18")
        triggers = e.get("triggers") or []
        if not triggers:
            problems.append(f"{where}: nothing unlocks it")
        keys = [json.dumps(t, sort_keys=True) for t in triggers]
        if len(set(keys)) != len(keys):
            problems.append(f"{where}: the same trigger twice")
        for t in triggers:
            if len(t) != 1 or next(iter(t)) not in TRIGGERS:
                problems.append(f"{where}: trigger {t} must be one of {', '.join(TRIGGERS)}")
                continue
            kind, value = next(iter(t.items()))
            if kind == "area":
                if not isinstance(value, str) or not value or len(value) > 80:
                    problems.append(f"{where}: area must be an area name")
                elif known_areas is not None and value.lower() not in known_areas:
                    problems.append(f"{where}: no area \"{value}\" in zone {zone}")
            elif not isinstance(value, int) or value <= 0:
                problems.append(f"{where}: {kind} must be an id")
            elif kind == "areaId" and known_areas is not None and value not in known_areas.values():
                problems.append(f"{where}: area {value} isn't in zone {zone}")
            elif kind in ("talk", "kill") and creatures is not None and value not in creatures:
                problems.append(f"{where}: creature {value} doesn't exist")
            elif kind in ("accept", "complete") and quests is not None and value not in quests:
                problems.append(f"{where}: quest {value} doesn't exist")
    return problems


def build(data):
    zone = data["zone"]
    lines = [
        f"-- mod-forever-journal: lore of {data.get('name', zone)} (zone {zone}).",
        "-- Generated from data/lore/*.json by tools/build_lore_sql.py; edit the JSON, not this file.",
        "",
        f"DELETE FROM `mod_forever_journal_lore_trigger` WHERE `Entry` BETWEEN {zone * 1000} AND {zone * 1000 + 999};",
        f"DELETE FROM `mod_forever_journal_lore` WHERE `ID` BETWEEN {zone * 1000} AND {zone * 1000 + 999};",
        "",
    ]
    order = {k: 0 for k in KINDS}
    entries, triggers = [], []
    for e in data["entries"]:
        order[e["kind"]] += 1
        body = "\n\n".join(p.strip() for p in e["body"])
        entries.append("({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})".format(
            e["id"], zone, KINDS[e["kind"]], order[e["kind"]], FACTIONS[e.get("faction")], e.get("races", 0),
            e.get("classes", 0), e.get("era", 0), sql_text(e["title"]), sql_text(body), sql_text("; ".join(e.get("sources", [])))))
        for t in e["triggers"]:
            kind, value = next(iter(t.items()))
            if kind == "area":
                triggers.append(f"({e['id']}, 1, 0, {sql_text(value)})")
            else:
                triggers.append(f"({e['id']}, {TRIGGERS[kind]}, {value}, '')")
    lines.append("INSERT INTO `mod_forever_journal_lore` (`ID`, `Zone`, `Kind`, `SortOrder`, `Faction`, `Races`, `Classes`, `Era`, "
                 "`Title`, `Body`, `Sources`) VALUES")
    lines.append(",\n".join(entries) + ";")
    lines.append("")
    lines.append("INSERT INTO `mod_forever_journal_lore_trigger` (`Entry`, `Type`, `Value`, `Area`) VALUES")
    lines.append(",\n".join(triggers) + ";")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="only check the JSON")
    ap.add_argument("--world-sql", type=pathlib.Path, help="the core's data/sql/base/db_world, to check creature and quest ids")
    ap.add_argument("--areas", type=pathlib.Path, help="AreaTable.dbc or an AreaTable csv, to check area names")
    a = ap.parse_args()

    creatures = quests = None
    if a.world_sql:
        creatures = ids_in(a.world_sql / "creature_template.sql")
        quests = ids_in(a.world_sql / "quest_template.sql")
    areas = read_areas(a.areas) if a.areas else None

    failed = False
    zones = {}
    for path in sorted(LORE.glob("*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        problems = check(path, data, creatures, quests, areas)
        if data.get("zone") in zones:
            problems.append(f"zone {data['zone']} is also in {zones[data['zone']]}")
        zones[data.get("zone")] = path.name
        for p in problems:
            print(f"{path.relative_to(ROOT)}: {p}", file=sys.stderr)
        if problems:
            failed = True
            continue
        out = OUT / f"mod_forever_journal_lore_{slug(data.get('name') or str(data['zone']))}.sql"
        if not a.check:
            out.write_text(build(data), encoding="utf-8")
        print(f"{path.relative_to(ROOT)}: {len(data['entries'])} entries" + ("" if a.check else f" -> {out.relative_to(ROOT)}"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
