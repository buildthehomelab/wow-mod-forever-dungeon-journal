-- DungeonJournal core: saved settings, the server protocol, caches, item and spell helpers and
-- slash commands. The data itself comes from the mod-dungeon-journal server module.

DungeonJournal = DungeonJournal or {}
local DJ = DungeonJournal

DJ.PREFIX = "DJN"
DJ.PROTOCOL = 1

-- Capability bits in the server's HELLO answer (see src/DungeonJournal.h).
DJ.HELLO_JOURNAL = 1
DJ.HELLO_MAP_PINS = 2
DJ.HELLO_PROGRESSION = 4
DJ.HELLO_LEARNING = 8

-- Ability tags.
DJ.TAG = {
	MAGIC = 0x0001, CURSE = 0x0002, DISEASE = 0x0004, POISON = 0x0008, ENRAGE = 0x0010,
	INTERRUPT = 0x0020, AOE = 0x0040, HEAL = 0x0080, SUMMON = 0x0100, CC = 0x0200,
	KNOCKBACK = 0x0400, BUFF = 0x0800, LEARNED = 0x1000,
}

-- Loot flags.
DJ.LOOT = { QUEST = 0x01, HARD_MODE = 0x02, CONDITION = 0x04, CHEST = 0x08, BOP = 0x10, SHARED = 0x20 }

-- Quest states.
DJ.QUEST = { AVAILABLE = 0, ACTIVE = 1, COMPLETE = 2, DONE = 3, LOW_LEVEL = 4, PREREQ = 5, LOCKED = 6 }

DJ.EXPANSIONS = { [0] = "Classic", [1] = "Burning Crusade", [2] = "Wrath of the Lich King" }

local floor = math.floor

-----------------------------------------
-- tiny event bus

local listeners = {}

function DJ.On(event, fn)
	listeners[event] = listeners[event] or {}
	table.insert(listeners[event], fn)
end

function DJ.Fire(event, ...)
	local list = listeners[event]
	if not list then return end
	for i = 1, #list do list[i](...) end
end

-----------------------------------------
-- timers (3.3.5a has no C_Timer)

local timerFrame = CreateFrame("Frame")
local timers = {}
local debounced = {}

function DJ.After(delay, fn)
	table.insert(timers, { at = GetTime() + delay, fn = fn })
	timerFrame:Show()
end

function DJ.Debounce(key, delay, fn)
	debounced[key] = { at = GetTime() + delay, fn = fn }
	timerFrame:Show()
end

timerFrame:SetScript("OnUpdate", function (self)
	local now = GetTime()
	local i = 1
	while i <= #timers do
		if timers[i].at <= now then
			local t = table.remove(timers, i)
			t.fn()
		else
			i = i + 1
		end
	end
	-- Collected first: a callback may debounce again, and a table must not grow while pairs() walks it.
	local due = {}
	for key, entry in pairs(debounced) do
		if entry.at <= now then table.insert(due, key) end
	end
	for _, key in ipairs(due) do
		local entry = debounced[key]
		if entry and entry.at <= now then
			debounced[key] = nil
			entry.fn()
		end
	end
	if #timers == 0 and not next(debounced) then self:Hide() end
end)

-----------------------------------------
-- formatting

local GOLD = "|TInterface\\MoneyFrame\\UI-GoldIcon:12:12:2:0|t"
local SILVER = "|TInterface\\MoneyFrame\\UI-SilverIcon:12:12:2:0|t"
local COPPER = "|TInterface\\MoneyFrame\\UI-CopperIcon:12:12:2:0|t"

function DJ.Money(copper)
	copper = floor(tonumber(copper) or 0)
	local g, s, c = floor(copper / 10000), floor(copper / 100) % 100, copper % 100
	local parts = {}
	if g > 0 then table.insert(parts, g .. GOLD) end
	if s > 0 then table.insert(parts, s .. SILVER) end
	if c > 0 or #parts == 0 then table.insert(parts, c .. COPPER) end
	return table.concat(parts, " ")
end

function DJ.Duration(seconds)
	seconds = floor(tonumber(seconds) or 0)
	if seconds >= 86400 then
		return string.format("%dd %dh", floor(seconds / 86400), floor(seconds / 3600) % 24)
	elseif seconds >= 3600 then
		return string.format("%dh %dm", floor(seconds / 3600), floor(seconds / 60) % 60)
	elseif seconds >= 60 then
		return string.format("%dm", floor(seconds / 60))
	end
	return string.format("%ds", seconds)
end

-- 1234567 -> "1,234,567"
function DJ.Number(n)
	local s = tostring(floor(tonumber(n) or 0))
	local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
	return (out:gsub("^,", ""))
end

-- Drop chance, as precise as it needs to be: 100%, 12%, 4.5%, 0.35%.
function DJ.Chance(hundredths)
	local pct = (tonumber(hundredths) or 0) / 100
	if pct >= 99.95 then return "100%" end
	if pct >= 10 then return string.format("%d%%", floor(pct + 0.5)) end
	if pct >= 1 then return (string.format("%.1f", pct):gsub("%.0$", "")) .. "%" end
	if pct >= 0.01 then return (string.format("%.2f", pct):gsub("0$", "")) .. "%" end
	return "<0.01%"
end

function DJ.Print(msg)
	DEFAULT_CHAT_FRAME:AddMessage("|cffe6cc80Dungeon Journal:|r " .. msg)
end

-- Text in a colour given as 0-1 floats.
function DJ.Colored(text, r, g, b)
	return string.format("|cff%02x%02x%02x%s|r", floor(r * 255 + 0.5), floor(g * 255 + 0.5), floor(b * 255 + 0.5), text)
end

function DJ.QualityColor(quality)
	local c = ITEM_QUALITY_COLORS[quality or 1] or ITEM_QUALITY_COLORS[1]
	return c.r, c.g, c.b, c.hex
end

-----------------------------------------
-- items and spells

local scanTip = CreateFrame("GameTooltip", "DungeonJournalScanTooltip", nil, "GameTooltipTemplate")
scanTip:SetOwner(WorldFrame, "ANCHOR_NONE")
local waiting, waitingCount = {}, 0

-- Asks the server for an item the client hasn't cached yet; ITEM_INFO fires when it arrives.
function DJ.QueryItem(entry)
	if not entry or entry == 0 or GetItemInfo(entry) then return end
	local key = "item:" .. entry
	if waiting[key] then return end
	waiting[key] = GetTime()
	waitingCount = waitingCount + 1
	scanTip:SetOwner(WorldFrame, "ANCHOR_NONE")
	scanTip:SetHyperlink(key)
end

local pollFrame = CreateFrame("Frame")
local pollElapsed = 0
pollFrame:SetScript("OnUpdate", function (self, elapsed)
	if waitingCount == 0 then return end
	pollElapsed = pollElapsed + elapsed
	if pollElapsed < 0.2 then return end
	pollElapsed = 0
	local now, arrived = GetTime(), false
	for key, since in pairs(waiting) do
		if GetItemInfo(key) then
			waiting[key] = nil
			waitingCount = waitingCount - 1
			arrived = true
		elseif now - since > 20 then
			waiting[key] = nil
			waitingCount = waitingCount - 1
		end
	end
	if arrived then DJ.Debounce("iteminfo", 0.15, function () DJ.Fire("ITEM_INFO") end) end
end)

function DJ.ItemIcon(entry)
	if not entry or entry == 0 then return nil end
	local _, _, _, _, _, _, _, _, _, texture = GetItemInfo(entry)
	return texture or (GetItemIcon and GetItemIcon(entry)) or "Interface\\Icons\\INV_Misc_QuestionMark"
end

-- A chat link for an item, from the client's cache or built from what the server told us.
function DJ.ItemLink(entry, name, quality)
	local _, link = GetItemInfo(entry)
	if link then return link end
	local _, _, _, hex = DJ.QualityColor(quality)
	return string.format("%s|Hitem:%d:0:0:0:0:0:0:0:0|h[%s]|h|r", hex, entry, name or ("item " .. entry))
end

-- A spell's description, as its tooltip shows it (the gold text under name, cost and cast time).
-- nil for spells without one: internal helpers the journal leaves out.
local descriptions = {}
function DJ.SpellDescription(spell)
	local cached = descriptions[spell]
	if cached ~= nil then return cached or nil end
	scanTip:SetOwner(WorldFrame, "ANCHOR_NONE")
	scanTip:ClearLines()
	scanTip:SetHyperlink("spell:" .. spell)
	local parts = {}
	for i = 2, scanTip:NumLines() do
		local line = _G["DungeonJournalScanTooltipTextLeft" .. i]
		local text = line and line:GetText()
		if text and text ~= "" then
			local r, g, b = line:GetTextColor()
			-- NORMAL_FONT_COLOR: the description. White lines are cost, range and cast time.
			if r and r > 0.95 and g > 0.75 and g < 0.9 and b < 0.15 then
				table.insert(parts, text)
			end
		end
	end
	local text = #parts > 0 and table.concat(parts, "\n") or nil
	descriptions[spell] = text or false
	return text
end

-----------------------------------------
-- server protocol: "<command>:<request id>:<field>..." whispered to yourself under the DJN prefix

DJ.serverReady = false
DJ.flags = 0
DJ.state = 18

local nextReq = 0
local pending = {}
-- list answers: first letter of the R/D/E codes per command
local LISTS = { D = true, B = true, S = true, A = true, L = true, Q = true, T = true, F = true }

local function send(msg)
	SendAddonMessage(DJ.PREFIX, msg, "WHISPER", UnitName("player"))
end

-- handler(result, err): result is { rows = {...}, meta = {...} } for list answers and the array of
-- fields for single ones; err is set when the server refused ("busy", "bad", "none").
function DJ.Request(cmd, fields, handler)
	nextReq = nextReq + 1
	local req = tostring(nextReq)
	local msg = cmd .. ":" .. req
	if fields and #fields > 0 then msg = msg .. ":" .. table.concat(fields, ":") end
	pending[req] = { req = req, cmd = cmd, handler = handler, rows = {}, msg = msg, tries = 0, at = GetTime() }
	send(msg)
	return req
end

local function splitFields(text)
	local out = {}
	if text == nil or text == "" then return out end
	for field in (text .. ":"):gmatch("([^:]*):") do table.insert(out, field) end
	return out
end

-- The server escapes ',', ';', ':', '|' and '%' in names as %XX.
function DJ.Unescape(text)
	if type(text) ~= "string" then return text end
	if text == "-" then return "" end
	return (text:gsub("%%(%x%x)", function (h) return string.char(tonumber(h, 16)) end))
end

local function parseRows(text, into)
	for row in text:gmatch("[^;]+") do
		local values = {}
		for value in row:gmatch("[^,]+") do table.insert(values, tonumber(value) or value) end
		table.insert(into, values)
	end
end
DJ.ParseRows = parseRows

local function finish(req, result, err)
	local p = pending[req]
	if not p then return end
	pending[req] = nil
	if p.handler then p.handler(result, err) end
end

local function onHello(fields)
	local version = tonumber(fields[1])
	if version ~= DJ.PROTOCOL then
		DJ.Print("the server speaks a different version of the journal (" .. tostring(version) .. ", this addon "
			.. DJ.PROTOCOL .. "). Update the DungeonJournal addon.")
		DJ.Fire("NO_SERVER")
		return
	end
	DJ.serverReady = true
	DJ.flags = tonumber(fields[2]) or 0
	DJ.stamp = fields[3] or "0"
	DJ.state = tonumber(fields[4]) or 18
	DJ.CheckCache()
	DJ.Fire("READY")
end

function DJ.Has(flag)
	return DJ.serverReady and bit.band(DJ.flags, flag) ~= 0
end

local function onAddonMessage(message)
	local code, req, rest = message:match("^([^:]+):([^:]*):?(.*)$")
	if not code then return end

	-- A whisper to yourself is echoed back. The module swallows ours; on a realm without it the
	-- echo comes back, and must not pass for the answer (it has the request's own fields).
	local p = pending[req]
	if p and p.msg == message then return end

	if code == "HELLO" then
		local fields = splitFields(rest)
		if #fields < 4 then return end
		onHello(fields)
		finish(req, fields)
		return
	elseif code == "OFF" then
		DJ.serverReady = false
		DJ.Fire("NO_SERVER")
		return
	elseif code == "ERR" then
		if rest == "busy" and p and p.tries < 6 then
			p.tries = p.tries + 1
			DJ.After(0.25 * p.tries, function () if pending[req] == p then send(p.msg) end end)
			return
		end
		finish(req, nil, rest)
		return
	end

	local kind, part = code:sub(1, 1), code:sub(2)
	if #code == 2 and LISTS[kind] and (part == "R" or part == "D" or part == "E") then
		if not p then return end
		if part == "R" then
			p.meta = splitFields(rest)
		elseif part == "D" then
			parseRows(rest, p.rows)
		else
			finish(req, { rows = p.rows, meta = p.meta or {} })
		end
		return
	end

	-- Single answers: H and M, to a request of the same kind.
	if p and (code == "H" or code == "M") and p.cmd == code then
		finish(req, splitFields(rest))
	end
end

local helloSerial = 0
function DJ.SayHello()
	helloSerial = helloSerial + 1
	local mine = helloSerial
	DJ.Request("HELLO", { tostring(DJ.PROTOCOL) })
	DJ.After(6, function ()
		if mine == helloSerial and not DJ.serverReady then DJ.Fire("NO_SERVER") end
	end)
end

-- Requests the server never answered (a reload in between, a lost message) are dropped after a
-- while so their handlers don't wait forever.
local sweep = CreateFrame("Frame")
local sweepElapsed = 0
sweep:SetScript("OnUpdate", function (self, elapsed)
	sweepElapsed = sweepElapsed + elapsed
	if sweepElapsed < 2 then return end
	sweepElapsed = 0
	local now = GetTime()
	local expired = {}
	for req, p in pairs(pending) do
		if now - p.at > 30 then table.insert(expired, req) end
	end
	for _, req in ipairs(expired) do finish(req, nil, "timeout") end
end)

-----------------------------------------
-- the cache of server data that only changes with the server's data stamp

function DJ.CheckCache()
	local db = DungeonJournalDB
	local key = tostring(DJ.stamp) .. ":" .. GetLocale()
	if db.cacheKey ~= key then
		db.cacheKey = key
		db.cache = { list = nil, bosses = {}, loot = {} }
		DJ.Fire("CACHE_RESET")
	end
end

function DJ.Cache()
	return DungeonJournalDB.cache
end

-----------------------------------------

local events = CreateFrame("Frame")
events:RegisterEvent("ADDON_LOADED")
events:RegisterEvent("PLAYER_LOGIN")
events:RegisterEvent("CHAT_MSG_ADDON")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("QUEST_LOG_UPDATE")
events:RegisterEvent("UPDATE_INSTANCE_INFO")
events:RegisterEvent("PLAYER_LEVEL_UP")
events:RegisterEvent("PLAYER_REGEN_ENABLED")

events:SetScript("OnEvent", function (self, event, ...)
	if event == "ADDON_LOADED" then
		if ... == "DungeonJournal" then
			DungeonJournalDB = DungeonJournalDB or {}
			DungeonJournalDB.cache = DungeonJournalDB.cache or { bosses = {}, loot = {} }
			DungeonJournalDB.minimap = DungeonJournalDB.minimap or { angle = 200 }
			DungeonJournalCharDB = DungeonJournalCharDB or {}
		end
	elseif event == "PLAYER_LOGIN" then
		DJ.Fire("LOGIN")
		DJ.After(3, DJ.SayHello)
		-- Retail's key for the journal, when nothing else has it.
		if not DungeonJournalDB.boundKey then
			DungeonJournalDB.boundKey = true
			local action = GetBindingAction("SHIFT-J")
			if not action or action == "" then
				SetBinding("SHIFT-J", "DUNGEONJOURNAL_TOGGLE")
				SaveBindings(GetCurrentBindingSet())
			end
		end
	elseif event == "CHAT_MSG_ADDON" then
		local prefix, message, _, sender = ...
		if prefix == DJ.PREFIX and sender == UnitName("player") then onAddonMessage(message) end
	elseif event == "PLAYER_ENTERING_WORLD" then
		DJ.Fire("ZONE")
		-- Lockouts may have changed (a reset, a kill elsewhere): the client asks, UPDATE_INSTANCE_INFO answers.
		if RequestRaidInfo then RequestRaidInfo() end
	elseif event == "PLAYER_REGEN_ENABLED" then
		-- After a fight inside an instance a boss may have died: kill checks are asked again.
		if IsInInstance() then DJ.Debounce("lockouts", 2.0, function () DJ.Fire("LOCKOUTS_CHANGED") end) end
	elseif event == "QUEST_LOG_UPDATE" then
		DJ.Debounce("quests", 1.0, function () DJ.Fire("QUESTS_CHANGED") end)
	elseif event == "UPDATE_INSTANCE_INFO" then
		DJ.Debounce("lockouts", 1.0, function () DJ.Fire("LOCKOUTS_CHANGED") end)
	elseif event == "PLAYER_LEVEL_UP" then
		DJ.Debounce("quests", 1.0, function () DJ.Fire("QUESTS_CHANGED") end)
	end
end)

BINDING_HEADER_DUNGEONJOURNAL = "Dungeon Journal"
BINDING_NAME_DUNGEONJOURNAL_TOGGLE = "Toggle the Dungeon Journal"

SLASH_DUNGEONJOURNAL1 = "/dj"
SLASH_DUNGEONJOURNAL2 = "/journal"
SLASH_DUNGEONJOURNAL3 = "/dungeonjournal"
SlashCmdList.DUNGEONJOURNAL = function (msg)
	msg = (msg or ""):lower():match("^%s*(.-)%s*$")
	if msg == "reset" then
		DJ.Fire("RESET_POSITION")
		DJ.Print("window position reset.")
	elseif msg == "minimap" then
		DungeonJournalDB.minimap.hide = not DungeonJournalDB.minimap.hide or nil
		DJ.Fire("MINIMAP")
	elseif msg == "refresh" then
		DungeonJournalDB.cacheKey = nil
		DJ.CheckCache()
		DJ.SayHello()
		DJ.Print("journal data will be fetched again.")
	elseif msg == "" then
		DJ.Toggle()
	elseif msg == "help" then
		DJ.Print("|cffffd200/dj|r (or Shift-J) opens the journal. |cffffd200/dj <name>|r searches. |cffffd200/dj minimap|r shows "
			.. "or hides the minimap button. |cffffd200/dj reset|r moves the window back. |cffffd200/dj refresh|r fetches the data again.")
	else
		DJ.Open()
		DJ.Fire("SEARCH", msg)
	end
end
