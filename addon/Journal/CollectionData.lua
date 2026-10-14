-- Collections data: the server's catalog of mounts, pets and toys, where each comes from, and
-- which ones this character has. The catalog is kept across sessions until the server's data
-- stamp changes; sources for the session. Mounts and pets the character knows come from the
-- client (GetCompanionInfo); toys in the bags or bank from the server.

local J = Journal

local Unescape = J.Unescape

J.KINDS = {
	mounts = { letter = "M", companion = "MOUNT", name = "Mounts", noun = "mount" },
	pets = { letter = "P", companion = "CRITTER", name = "Pets", noun = "pet" },
	toys = { letter = "T", name = "Toys", noun = "toy" },
}

-- Catalog row flags (EntryFlags in JournalCollections.cpp).
J.ENTRY = { ALLIANCE = 0x01, HORDE = 0x02, FLYING = 0x04, CLASS = 0x08, UNOBTAINABLE = 0x10 }

-- Source bits (SourceBits), with the names the filter menu shows.
J.SOURCE = {
	{ bit = 0x001, name = "Vendor" },
	{ bit = 0x002, name = "Drop" },
	{ bit = 0x004, name = "Quest" },
	{ bit = 0x008, name = "Achievement" },
	{ bit = 0x010, name = "Profession" },
	{ bit = 0x020, name = "Trainer" },
	{ bit = 0x040, name = "Container" },
	{ bit = 0x080, name = "Fishing" },
	{ bit = 0x100, name = "World Event" },
	{ bit = 0x200, name = "Other" },
}

local band = bit.band

-----------------------------------------
-- the catalog

local catalogs = {}
local inflight = {}

local function buildCatalog(rows)
	local list, byId = {}, {}
	for _, r in ipairs(rows) do
		local e = { id = r[1], creature = r[2], item = r[3], quality = r[4] or 1, flags = r[5] or 0, classMask = r[6] or 0,
			sources = r[7] or 0, reqState = r[8] or 0 }
		table.insert(list, e)
		byId[e.id] = e
	end
	return { list = list, byId = byId }
end

-- Calls done(catalog) with { list = { entry, ... }, byId = { [id] = entry } }, or nil.
function J.LoadCatalog(kind, done)
	if catalogs[kind] then done(catalogs[kind]) return end
	local cache = J.Cache()
	cache.collections = cache.collections or {}
	if cache.collections[kind] then
		catalogs[kind] = buildCatalog(cache.collections[kind])
		done(catalogs[kind])
		return
	end
	if inflight[kind] then table.insert(inflight[kind], done) return end
	inflight[kind] = { done }
	J.Request("C", { J.KINDS[kind].letter }, function (result)
		local waiting = inflight[kind] or {}
		inflight[kind] = nil
		if result then
			-- Kept as the server's rows: a SavedVariable of plain arrays.
			J.Cache().collections = J.Cache().collections or {}
			J.Cache().collections[kind] = result.rows
			catalogs[kind] = buildCatalog(result.rows)
		end
		for _, fn in ipairs(waiting) do fn(catalogs[kind]) end
	end)
end

J.On("CACHE_RESET", function () catalogs = {} end)

-----------------------------------------
-- names and icons: mounts and pets from their spell, toys from their item

function J.EntryName(kind, e)
	if kind == "toys" then
		local name = GetItemInfo(e.id)
		if not name then J.QueryItem(e.id) end
		return name
	end
	return (GetSpellInfo(e.id))
end

function J.EntryIcon(kind, e)
	if kind == "toys" then return J.ItemIcon(e.id) end
	local _, _, icon = GetSpellInfo(e.id)
	return icon or "Interface\\Icons\\INV_Misc_QuestionMark"
end

-----------------------------------------
-- what this character has

local owned = { mounts = {}, pets = {}, toys = {} }
local ownedToysAt = nil

-- Mounts and pets: spell -> companion index, read from the client.
local function readCompanions()
	for kind, info in pairs(J.KINDS) do
		if info.companion and GetNumCompanions then
			local have = {}
			for i = 1, GetNumCompanions(info.companion) do
				local _, _, spell, _, active = GetCompanionInfo(info.companion, i)
				if spell then have[spell] = { index = i, active = active and true or false } end
			end
			owned[kind] = have
		end
	end
end

function J.Owned(kind, e)
	return owned[kind][e.id] ~= nil
end

function J.Companion(kind, e)
	return owned[kind][e.id]
end

-- Asks the server which toys are in the bags or bank, at most every few seconds.
function J.RefreshToys(done)
	if not J.Has(J.HELLO_COLLECTIONS) then if done then done() end return end
	if ownedToysAt and GetTime() - ownedToysAt < 3 then if done then done() end return end
	ownedToysAt = GetTime()
	J.Request("O", nil, function (result)
		if result then
			local have = {}
			for _, r in ipairs(result.rows) do have[r[1]] = true end
			owned.toys = have
			J.Fire("COLLECTION_OWNED", "toys")
		end
		if done then done() end
	end)
end

J.On("COMPANIONS", function ()
	readCompanions()
	J.Fire("COLLECTION_OWNED", "mounts")
	J.Fire("COLLECTION_OWNED", "pets")
end)

J.On("BAGS", function ()
	ownedToysAt = nil
	if J.frame:IsShown() and J.view.section == "toys" then J.RefreshToys() end
end)

-----------------------------------------
-- filters, kept per character and kind

local DEFAULT_FILTERS = { collected = true, missing = true, usable = false, hideLocked = false, sources = {} }

function J.Filters(kind)
	JournalCharDB.collectionFilters = JournalCharDB.collectionFilters or {}
	local f = JournalCharDB.collectionFilters[kind]
	if not f then
		f = {}
		for k, v in pairs(DEFAULT_FILTERS) do f[k] = type(v) == "table" and {} or v end
		JournalCharDB.collectionFilters[kind] = f
	end
	f.sources = f.sources or {}
	return f
end

local _, playerClass = UnitClass("player")
local CLASS_BITS = { WARRIOR = 1, PALADIN = 2, HUNTER = 3, ROGUE = 4, PRIEST = 5, DEATHKNIGHT = 6, SHAMAN = 7, MAGE = 8,
	WARLOCK = 9, DRUID = 11 }
local CLASS_TOKENS = {}
for token, id in pairs(CLASS_BITS) do CLASS_TOKENS[id] = token end
J.CLASS_TOKENS = CLASS_TOKENS

local function playerFaction()
	local faction = UnitFactionGroup and UnitFactionGroup("player")
	return faction
end

-- Whether this character could use it: its faction and class.
function J.Usable(e)
	local faction = playerFaction()
	if band(e.flags, J.ENTRY.ALLIANCE) ~= 0 and faction == "Horde" then return false end
	if band(e.flags, J.ENTRY.HORDE) ~= 0 and faction == "Alliance" then return false end
	if band(e.flags, J.ENTRY.CLASS) ~= 0 and e.classMask ~= 0 then
		local _, token = UnitClass("player")
		local id = CLASS_BITS[token or playerClass or ""]
		if not id or band(e.classMask, 2 ^ (id - 1)) == 0 then return false end
	end
	return true
end

function J.EntryLocked(e)
	return J.Has(J.HELLO_PROGRESSION) and (e.reqState or 0) > (J.state or 18)
end

-- The catalog's entries the filters and the search text let through, collected ones first,
-- then by name. Entries the client can't name (spells it doesn't have) are left out.
function J.FilteredEntries(kind, catalog, text)
	local f = J.Filters(kind)
	local anySource = next(f.sources) ~= nil
	local out, total, have = {}, 0, 0
	text = text and text ~= "" and text:lower() or nil
	for _, e in ipairs(catalog.list) do
		local name = J.EntryName(kind, e)
		if name or kind == "toys" then
			total = total + 1
			local mine = J.Owned(kind, e)
			if mine then have = have + 1 end
			local ok = (mine and f.collected) or (not mine and f.missing)
			if ok and f.usable and not J.Usable(e) then ok = false end
			if ok and f.hideLocked and J.EntryLocked(e) then ok = false end
			if ok and kind == "mounts" and f.mountType then
				local flying = band(e.flags, J.ENTRY.FLYING) ~= 0
				if (f.mountType == "flying") ~= flying then ok = false end
			end
			if ok and anySource then
				local hit = false
				for b in pairs(f.sources) do if band(e.sources, b) ~= 0 then hit = true break end end
				ok = hit
			end
			if ok and text and not (name or ""):lower():find(text, 1, true) then ok = false end
			if ok then
				e.sortName = (name or ("item " .. e.id)):lower()
				e.mine = mine
				table.insert(out, e)
			end
		end
	end
	table.sort(out, function (a, b)
		if a.mine ~= b.mine then return a.mine end
		if a.sortName ~= b.sortName then return a.sortName < b.sortName end
		return a.id < b.id
	end)
	return out, have, total
end

-----------------------------------------
-- where one comes from

local sources = {}

local function buildSources(rows)
	local list = {}
	for _, r in ipairs(rows) do
		local kind = r[1]
		local s = { kind = kind, index = r[2] }
		if kind == "V" then
			s.npc, s.copper, s.honor, s.arena, s.items, s.reqState, s.placed = r[3], r[4], r[5], r[6], r[7], r[8], r[9] == 1
			s.name, s.where, s.event = Unescape(tostring(r[10])), Unescape(tostring(r[11] or "-")), Unescape(tostring(r[12] or "-"))
		elseif kind == "D" then
			s.creature, s.chance, s.instance, s.boss, s.reqState, s.placed, s.rare = r[3], r[4], r[5], r[6], r[7], r[8] == 1, r[9] == 1
			s.name, s.where = Unescape(tostring(r[10])), Unescape(tostring(r[11] or "-"))
		elseif kind == "G" then
			s.object, s.chance, s.instance, s.boss, s.reqState, s.placed = r[3], r[4], r[5], r[6], r[7], r[8] == 1
			s.name, s.where = Unescape(tostring(r[9])), Unescape(tostring(r[10] or "-"))
		elseif kind == "P" then
			s.creature, s.chance, s.reqState, s.placed = r[3], r[4], r[5], r[6] == 1
			s.name, s.where = Unescape(tostring(r[7])), Unescape(tostring(r[8] or "-"))
		elseif kind == "F" then
			s.area, s.chance, s.reqState, s.name = r[3], r[4], r[5], Unescape(tostring(r[6]))
		elseif kind == "I" then
			s.item, s.chance = r[3], r[4]
		elseif kind == "Q" then
			s.quest, s.level, s.reqState, s.placed, s.faction = r[3], r[4], r[5], r[6] == 1, r[7]
			s.name, s.giver, s.where = Unescape(tostring(r[8])), Unescape(tostring(r[9] or "-")), Unescape(tostring(r[10] or "-"))
		elseif kind == "A" then
			s.achievement = r[3]
		elseif kind == "C" then
			s.spell, s.skill, s.rank, s.name = r[3], r[4], r[5], Unescape(tostring(r[6]))
		elseif kind == "T" then
			s.class, s.level, s.copper, s.reqState, s.placed, s.trainers = r[3], r[4], r[5], r[6], r[7] == 1, r[8]
			s.name, s.where = Unescape(tostring(r[9])), Unescape(tostring(r[10] or "-"))
		elseif kind == "N" then
			s.text = Unescape(tostring(r[3]))
		elseif kind == "+" then
			s.of, s.more = r[3], r[4]
		end
		s.reqState = s.reqState or 0
		table.insert(list, s)
	end
	return list
end

-- Calls done(sources) with the entry's sources, in the server's order.
function J.LoadSources(kind, e, done)
	local key = kind .. ":" .. e.id
	if sources[key] then done(sources[key]) return end
	J.Request("W", { J.KINDS[kind].letter, tostring(e.id) }, function (result)
		if result then sources[key] = buildSources(result.rows) end
		done(sources[key])
	end)
end

-- A red flag on the map for a source; done(where text) when the server answers.
function J.ShowSourceOnMap(kind, e, s, done)
	J.Request("M", { "S", J.KINDS[kind].letter, tostring(e.id), tostring(s.index) }, function (result, err)
		local where = result and result[2] and Unescape(result[2])
		if done then done(where, err) end
	end)
end

J.On("CACHE_RESET", function () sources = {} end)

-----------------------------------------

local events = CreateFrame("Frame")
events:RegisterEvent("PLAYER_LOGIN")
events:RegisterEvent("COMPANION_LEARNED")
events:RegisterEvent("COMPANION_UPDATE")
events:RegisterEvent("BAG_UPDATE")
events:RegisterEvent("PLAYERBANKSLOTS_CHANGED")
events:SetScript("OnEvent", function (self, event)
	if event == "BAG_UPDATE" or event == "PLAYERBANKSLOTS_CHANGED" then
		J.Debounce("collection-bags", 1.0, function () J.Fire("BAGS") end)
	else
		J.Debounce("collection-companions", 0.3, function () J.Fire("COMPANIONS") end)
	end
end)
