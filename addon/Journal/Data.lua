-- Journal data: asks the server for each part of the journal and keeps the answers.
-- Instance lists, boss lists and loot are kept across sessions until the server's data stamp
-- changes; abilities and what tokens turn into for the session (bosses can be seen casting new
-- abilities); a player's status and quests are always asked fresh, with a few seconds' grace for
-- redraws.

local J = Journal

local Unescape = J.Unescape

-- One request per key at a time; everyone asking meanwhile gets the same answer.
local inflight = {}
local function fetch(key, cmd, fields, parse, done, raw)
	if inflight[key] then
		table.insert(inflight[key], done)
		return
	end
	inflight[key] = { done }
	J.Request(cmd, fields, function (result, err)
		local waiting = inflight[key] or {}
		inflight[key] = nil
		local value = result and parse(result) or nil
		for _, fn in ipairs(waiting) do fn(value, err) end
	end, raw)
end
J.Fetch = fetch

-----------------------------------------
-- instances

local instances, instanceList

-- The Dungeon Finder's info per LFG id: { name, typeID, ..., texture (10), ... }. FrameXML's
-- LFGDungeonInfo (what LFGGetDungeonInfoByID reads) is only filled once the Dungeon Finder list
-- has been opened, so ask the client for the same table ourselves.
local LFG_TEXTURE = 10
local lfgInfo

local function lfgTexture(lfgId)
	if not lfgId or lfgId == 0 then return nil end
	if not lfgInfo then
		lfgInfo = _G.LFGDungeonInfo
		if type(lfgInfo) ~= "table" and GetLFDChoiceInfo then
			local ok, info = pcall(GetLFDChoiceInfo, {})
			lfgInfo = ok and type(info) == "table" and info or nil
		end
		if not lfgInfo then return nil end
	end
	local entry = lfgInfo[lfgId]
	local texture = type(entry) == "table" and entry[LFG_TEXTURE]
	if type(texture) == "string" and texture ~= "" then return texture end
end

local function buildInstances(rows)
	local byId, list = {}, {}
	for _, r in ipairs(rows) do
		local kind = r[1]
		if kind == "I" then
			local inst = {
				id = r[2], map = r[3], raid = r[4] == 1, expansion = r[5], minLevel = r[6], maxLevel = r[7],
				targetLevel = r[8], lfgId = r[9], name = Unescape(tostring(r[10])), zone = Unescape(tostring(r[11] or "-")),
				wings = {}, variants = {},
			}
			inst.texture = lfgTexture(inst.lfgId)
			byId[inst.id] = inst
			table.insert(list, inst)
		elseif kind == "W" and byId[r[2]] then
			table.insert(byId[r[2]].wings, { index = r[3], lfgId = r[4], minLevel = r[5], maxLevel = r[6],
				name = Unescape(tostring(r[7])) })
		elseif kind == "V" and byId[r[2]] then
			table.insert(byId[r[2]].variants, { id = r[3], difficulty = r[4], expansion = r[5], players = r[6],
				minLevel = r[7], reqState = r[8], label = Unescape(tostring(r[9])) })
		end
	end
	for _, inst in ipairs(list) do
		table.sort(inst.wings, function (a, b) return a.index < b.index end)
		local tiers = {}
		for _, v in ipairs(inst.variants) do tiers[v.expansion] = true end
		inst.tiers = tiers
	end
	return byId, list
end

-- Calls done(byId, list) once the instance list is here.
function J.LoadInstances(done)
	if instances then done(instances, instanceList) return end
	local cache = J.Cache()
	if cache.list then
		instances, instanceList = buildInstances(cache.list)
		done(instances, instanceList)
		return
	end
	if not J.Has(J.HELLO_JOURNAL) then done(nil) return end
	fetch("D", "D", nil, function (result) return result.rows end, function (rows)
		if not rows then done(nil) return end
		J.Cache().list = rows
		instances, instanceList = buildInstances(rows)
		done(instances, instanceList)
	end)
end

function J.Instance(id)
	return instances and instances[id]
end

J.On("CACHE_RESET", function ()
	instances, instanceList = nil, nil
end)

-- The tab of an instance to show for a tier: the one the era has opened, the later one when
-- several are open (25 player over 10), else the first.
function J.VariantsFor(inst, tier)
	local out = {}
	for _, v in ipairs(inst.variants) do
		if tier == nil or v.expansion == tier then table.insert(out, v) end
	end
	return out
end

function J.Variant(inst, id)
	for _, v in ipairs(inst.variants) do
		if v.id == id then return v end
	end
end

function J.IsLocked(variant)
	return J.Has(J.HELLO_PROGRESSION) and (J.state or 18) < (variant.reqState or 0)
end

-- An instance is locked for a tier when every tab it has there is.
function J.InstanceLocked(inst, tier)
	local any = false
	for _, v in ipairs(J.VariantsFor(inst, tier)) do
		any = true
		if not J.IsLocked(v) then return false end
	end
	return any
end

-----------------------------------------
-- bosses

local function buildBosses(rows)
	local list = {}
	for _, r in ipairs(rows) do
		table.insert(list, { key = r[1], wing = r[2], display = r[3], rare = bit.band(r[4], 1) ~= 0,
			chest = bit.band(r[4], 2) ~= 0, difficulties = r[5], name = Unescape(tostring(r[6])) })
	end
	return list
end

local bosses = {}
function J.LoadBosses(inst, done)
	if bosses[inst.id] then done(bosses[inst.id]) return end
	local cache = J.Cache()
	if cache.bosses[inst.id] then
		bosses[inst.id] = buildBosses(cache.bosses[inst.id])
		done(bosses[inst.id])
		return
	end
	fetch("B" .. inst.id, "B", { tostring(inst.id) }, function (result) return result.rows end, function (rows)
		if not rows then done(nil) return end
		J.Cache().bosses[inst.id] = rows
		bosses[inst.id] = buildBosses(rows)
		done(bosses[inst.id])
	end)
end

J.On("CACHE_RESET", function () bosses = {} end)

-- Is the boss fought on this tab? Seeded tabs (40 player) follow the normal encounter list.
function J.BossOnVariant(boss, variant)
	local d = variant.id >= 100 and 0 or variant.difficulty
	return bit.band(boss.difficulties, 2 ^ d) ~= 0
		or (variant.id >= 100 and bit.band(boss.difficulties, 2 ^ variant.difficulty) ~= 0)
end

-----------------------------------------
-- the player's status: lockouts, era locks, requirements

local status, statusAt = {}, {}
function J.LoadStatus(inst, done, force)
	if not force and status[inst.id] and GetTime() - statusAt[inst.id] < 5 then done(status[inst.id]) return end
	fetch("S" .. inst.id, "S", { tostring(inst.id) }, function (result)
		local out = { state = tonumber(result.meta[2]) or 18, variants = {}, requirements = {} }
		for _, r in ipairs(result.rows) do
			if r[1] == "V" then
				out.variants[r[2]] = { locked = r[3] == 1, reqState = r[4], saved = r[5] == 1, resetIn = r[6], killed = r[7] or 0,
					at = GetTime() }
			elseif r[1] == "R" then
				out.requirements[r[2]] = out.requirements[r[2]] or {}
				table.insert(out.requirements[r[2]], { kind = r[3], id = r[4], have = r[5] == 1, name = Unescape(tostring(r[6] or "-")) })
			end
		end
		return out
	end, function (value)
		if value then
			status[inst.id], statusAt[inst.id] = value, GetTime()
			J.state = value.state
		end
		done(value)
	end)
end

J.On("LOCKOUTS_CHANGED", function ()
	for k in pairs(statusAt) do statusAt[k] = 0 end
	J.Fire("STATUS_STALE")
end)

function J.Killed(st, variant, boss)
	local v = st and st.variants[variant.id]
	if not v or not v.saved or boss.key >= 32 then return false end
	return bit.band(v.killed, 2 ^ boss.key) ~= 0
end

-----------------------------------------
-- abilities (this session)

local abilities = {}
function J.LoadAbilities(inst, boss, variant, done)
	local key = inst.id .. ":" .. boss.key .. ":" .. variant.id
	if abilities[key] then done(abilities[key]) return end
	fetch("A" .. key, "A", { tostring(inst.id), tostring(boss.key), tostring(variant.id) }, function (result)
		-- units.rewards: lines on what the boss gives besides its loot.
		local units, byEntry = { rewards = {} }, {}
		for _, r in ipairs(result.rows) do
			if r[1] == "U" then
				local u = { entry = r[2], level = r[3], rank = r[4], health = r[5], add = r[6] == 1,
					name = Unescape(tostring(r[7])), spells = {} }
				byEntry[u.entry] = u
				table.insert(units, u)
			elseif r[1] == "S" and byEntry[r[2]] then
				-- r[6]: the server's description, for a spell whose tooltip has none
				table.insert(byEntry[r[2]].spells, { spell = r[3], tags = r[4], castMs = r[5],
					text = J.NonEmpty(r[6] and Unescape(tostring(r[6]))) })
			elseif r[1] == "N" and byEntry[r[2]] then
				for _, s in ipairs(byEntry[r[2]].spells) do
					if s.spell == r[3] then s.note = J.NonEmpty(Unescape(tostring(r[4] or "-"))) end
				end
			elseif r[1] == "E" and byEntry[r[2]] then
				byEntry[r[2]].enrage = { spell = r[3], seconds = r[4] }
			elseif r[1] == "R" and J.NonEmpty(Unescape(tostring(r[2] or "-"))) then
				table.insert(units.rewards, Unescape(tostring(r[2])))
			end
		end
		return units
	end, function (units)
		if units then abilities[key] = units end
		done(units)
	end)
end

-----------------------------------------
-- loot (kept with the stamp)

local function buildLoot(rows)
	local list = {}
	for _, r in ipairs(rows) do
		table.insert(list, { item = r[1], chance = r[2], flags = r[3], minCount = r[4], maxCount = r[5], quality = r[6],
			class = r[7], subclass = r[8], invType = r[9], ilvl = r[10], reqLevel = r[11], boss = r[12],
			name = Unescape(tostring(r[13])), classes = r[14] or 0 })
	end
	return list
end

local loot = {}
function J.LoadLoot(inst, bossKey, variant, done)
	local key = inst.id .. ":" .. bossKey .. ":" .. variant.id
	if loot[key] then done(loot[key]) return end
	local cache = J.Cache()
	if cache.loot[key] then
		loot[key] = buildLoot(cache.loot[key])
		done(loot[key])
		return
	end
	fetch("L" .. key, "L", { tostring(inst.id), tostring(bossKey), tostring(variant.id) }, function (result)
		return result.rows
	end, function (rows)
		if not rows then done(nil) return end
		J.Cache().loot[key] = rows
		loot[key] = buildLoot(rows)
		done(loot[key])
	end)
end

J.On("CACHE_RESET", function () loot = {} ; abilities = {} end)

-----------------------------------------
-- tokens: what one turns into (this session)

local tokens = {}
J.On("CACHE_RESET", function () tokens = {} end)

function J.TokenResults(item)
	return tokens[item]
end

function J.LoadTokens(item, done)
	if tokens[item] then done(tokens[item]) return end
	fetch("K" .. item, "K", { tostring(item) }, function (result)
		local list = {}
		for _, r in ipairs(result.rows) do
			table.insert(list, { item = r[1], classes = r[2], kind = r[3], quality = r[4], ilvl = r[5],
				name = Unescape(tostring(r[6] or "-")) })
		end
		return list
	end, function (list)
		if list then tokens[item] = list end
		done(list)
	end)
end

-----------------------------------------
-- quests (fresh)

-- Q and R rows (and the Quests tab's C rows, the heading a search result is under) into quests,
-- in the order the server sent them.
local function parseQuests(rows)
	local list, byId, headingOf = {}, {}, {}
	local function items(text, into)
		if type(text) ~= "string" then return end
		for id, n in text:gmatch("(%d+)%*(%d+)") do table.insert(into, { id = tonumber(id), n = tonumber(n) }) end
	end
	for _, r in ipairs(rows) do
		if r[1] == "Q" then
			local q = { id = r[2], state = r[3], level = r[4], minLevel = r[5], flags = r[6], xp = r[7], money = r[8],
				giverKind = r[9], giverEntry = r[10], name = Unescape(tostring(r[11])), giver = Unescape(tostring(r[12] or "-")),
				where = Unescape(tostring(r[13] or "-")), step = tonumber(r[14]) or 0, era = tonumber(r[15]) or 0,
				heading = headingOf[r[2]], rewards = {}, choices = {} }
			byId[q.id] = q
			table.insert(list, q)
		elseif r[1] == "R" and byId[r[2]] then
			items(r[3], byId[r[2]].rewards)
			items(r[4], byId[r[2]].choices)
		elseif r[1] == "C" then
			headingOf[r[2]] = r[3]
		end
	end
	return list
end

local quests, questsAt = {}, {}
function J.LoadQuests(inst, done, force)
	if not force and quests[inst.id] and GetTime() - questsAt[inst.id] < 5 then done(quests[inst.id]) return end
	fetch("Q" .. inst.id, "Q", { tostring(inst.id) }, function (result)
		return parseQuests(result.rows)
	end, function (list)
		if list then quests[inst.id], questsAt[inst.id] = list, GetTime() end
		done(list)
	end)
end

-- The Quests tab's headings: { list = { { group, id, total, countable, done, era, low, high, name } }, here = id }.
local headings, headingsAt
function J.LoadQuestHeadings(done, force)
	if not J.Has(J.HELLO_QUESTS) then done(nil) return end
	if not force and headings and GetTime() - headingsAt < 5 then done(headings) return end
	fetch("G", "G", nil, function (result)
		local out = { list = {}, byId = {}, here = tonumber(result.meta[2]) or 0 }
		for _, r in ipairs(result.rows) do
			if r[1] == "G" then
				local h = { group = tostring(r[2]), id = r[3], total = r[4], countable = r[5], done = r[6], era = r[7],
					low = r[8], high = r[9], name = Unescape(tostring(r[10] or "-")) }
				table.insert(out.list, h)
				out.byId[h.id] = h
			end
		end
		return out
	end, function (out)
		if out then headings, headingsAt = out, GetTime() end
		done(out)
	end)
end

-- Every quest under a heading (list.total: how many there are, when the server sent fewer).
local headingQuests, headingQuestsAt = {}, {}
function J.LoadHeadingQuests(id, done, force)
	if not J.Has(J.HELLO_QUESTS) then done(nil) return end
	if not force and headingQuests[id] and GetTime() - headingQuestsAt[id] < 5 then done(headingQuests[id]) return end
	fetch("Z" .. id, "Z", { tostring(id) }, function (result)
		local list = parseQuests(result.rows)
		-- A very long heading is cut short: how many it has.
		list.total = tonumber(result.meta[3]) or #list
		return list
	end, function (list)
		if list then headingQuests[id], headingQuestsAt[id] = list, GetTime() end
		done(list)
	end)
end

-- Quests by name, each with the heading it's under.
function J.SearchQuests(text, done)
	text = (text or ""):gsub("[:;|]", " "):match("^%s*(.-)%s*$")
	if #text < 3 or not J.Has(J.HELLO_QUESTS) then done(nil) return end
	fetch("X" .. text:lower(), "X", { text }, function (result)
		return parseQuests(result.rows)
	end, done)
end

J.On("QUESTS_CHANGED", function ()
	for k in pairs(questsAt) do questsAt[k] = 0 end
	for k in pairs(headingQuestsAt) do headingQuestsAt[k] = 0 end
	headingsAt = 0
	J.Fire("QUESTS_STALE")
end)

local questDetail = {}
function J.LoadQuestDetail(quest, done)
	if questDetail[quest] then done(questDetail[quest]) return end
	fetch("T" .. quest, "QD", { tostring(quest) }, function (result)
		local d = { text = {}, kills = {}, items = {} }
		for _, r in ipairs(result.rows) do
			if r[1] == "O" then
				table.insert(d.text, Unescape(tostring(r[2] or "")))
			elseif r[1] == "K" then
				table.insert(d.kills, { target = r[2], count = r[3], name = Unescape(tostring(r[4] or "-")) })
			elseif r[1] == "I" then
				table.insert(d.items, { item = r[2], count = r[3], name = Unescape(tostring(r[4] or "-")) })
			elseif r[1] == "P" then
				d.prev = { id = r[2], done = r[3] == 1, name = Unescape(tostring(r[4] or "-")) }
			end
		end
		local _, class = UnitClass("player")
		d.objectives = J.FillPlaceholders(table.concat(d.text))
		d.class = class
		return d
	end, function (d)
		if d then questDetail[quest] = d end
		done(d)
	end)
end

-- Quest and book text keeps the client's placeholders: fills in the usual ones ($N name, $C class,
-- $R race, $B line break, $G male:female;).
function J.FillPlaceholders(text)
	local name = UnitName("player") or ""
	local race = UnitRace("player") or ""
	return (text:gsub("%$[Nn]", name):gsub("%$[Cc]", (UnitClass("player"))):gsub("%$[Rr]", race)
		:gsub("%$[Bb]", "\n"):gsub("%$[Gg]([^:;]*):([^;]*);", function (male, female)
			return UnitSex("player") == 3 and female or male
		end))
end

-----------------------------------------
-- where am I, map flags, search

function J.Here(done)
	if not J.Has(J.HELLO_JOURNAL) then done(nil) return end
	J.Request("H", nil, function (result, err)
		if not result then done(nil) return end
		done(tonumber(result[1]), tonumber(result[2]))
	end)
end

function J.ShowOnMap(kind, id)
	if not J.Has(J.HELLO_MAP_PINS) then return end
	J.Request("M", { kind, tostring(id) }, function (result, err)
		if result and result[1] == "ok" then
			J.Print(Unescape(result[2] or ""))
		else
			J.Print("that place isn't on any map the journal knows.")
		end
	end)
end

function J.Search(text, done)
	text = (text or ""):gsub("[:;|]", " "):match("^%s*(.-)%s*$")
	if #text < 3 or not J.Has(J.HELLO_JOURNAL) then done(nil) return end
	fetch("F" .. text:lower(), "F", { text }, function (result)
		local out = {}
		for _, r in ipairs(result.rows) do
			if r[1] == "I" then
				table.insert(out, { kind = "I", instance = r[2] })
			elseif r[1] == "B" then
				table.insert(out, { kind = "B", instance = r[2], boss = r[3] })
			elseif r[1] == "L" then
				table.insert(out, { kind = "L", instance = r[2], boss = r[3], variant = r[4], item = r[5], chance = r[6], quality = r[7] })
			end
		end
		return out
	end, done)
end
