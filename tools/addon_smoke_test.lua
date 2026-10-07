-- Smoke test for the DungeonJournal addon outside the game: stubs just enough of the 3.3.5a API,
-- loads the addon in .toc order and answers its requests with a fake mod-dungeon-journal server,
-- then walks every page and tab.
-- Usage (any Lua 5.3+): lua tools/addon_smoke_test.lua addon/DungeonJournal
-- DRAGON=1 runs it again with a DragonUI stand-in, for the skinned branches.

local dir = arg[1] or "addon/DungeonJournal"
unpack = table.unpack
bit = {
	band = function (a, b) return math.floor(a) & math.floor(b) end,
	bor = function (a, b) return math.floor(a) | math.floor(b) end,
}
function wipe(t) for k in pairs(t) do t[k] = nil end return t end

local now = 0
function GetTime() return now end

local errors = 0
local function report(where, err)
	errors = errors + 1
	print("ERROR in " .. where .. ": " .. tostring(err))
end

-----------------------------------------
-- frames: tables with scripts; unknown methods are no-ops returning nil

local frameMeta = {}
local allFrames = {}
local function newObject(kind, name, parent)
	local o = { __kind = kind, __name = name, __scripts = {}, __shown = true, __text = "", __width = 100, __height = 20,
		__parent = parent, __enabled = true }
	setmetatable(o, frameMeta)
	if name then _G[name] = o end
	table.insert(allFrames, o)
	return o
end

local methods = {}
function methods:SetScript(event, fn) self.__scripts[event] = fn end
function methods:GetScript(event) return self.__scripts[event] end
function methods:HookScript(event, fn)
	local old = self.__scripts[event]
	self.__scripts[event] = function (...) if old then old(...) end fn(...) end
end
function methods:Show() local was = self.__shown; self.__shown = true; if not was and self.__scripts.OnShow then self.__scripts.OnShow(self) end end
function methods:Hide() local was = self.__shown; self.__shown = false; if was and self.__scripts.OnHide then self.__scripts.OnHide(self) end end
function methods:IsShown() return self.__shown end
function methods:IsVisible()
	local f = self
	while f do
		if not rawget(f, "__shown") then return false end
		f = rawget(f, "__parent")
	end
	return true
end
function methods:SetText(t) self.__text = t == nil and "" or tostring(t) end
function methods:GetText() return self.__text end
function methods:SetChecked(c) self.__checked = c and 1 or nil end
function methods:GetChecked() return rawget(self, "__checked") end
function methods:GetWidth() return self.__width end
function methods:GetHeight() return self.__height end
function methods:SetWidth(w) self.__width = w end
function methods:SetHeight(h) self.__height = h end
function methods:SetSize(w, h) self.__width, self.__height = w, h end
function methods:GetName() return self.__name end
function methods:GetFrameLevel() return 1 end
function methods:GetObjectType() return self.__kind end
function methods:GetRegions() return end
function methods:CreateTexture() return newObject("Texture", nil, self) end
function methods:CreateFontString() return newObject("FontString", nil, self) end
function methods:Enable() self.__enabled = true end
function methods:Disable() self.__enabled = false end
function methods:IsEnabled() return self.__enabled and 1 or nil end
function methods:GetParent() return rawget(self, "__parent") end
function methods:SetParent(p) self.__parent = p end
function methods:GetEffectiveScale() return 1 end
function methods:GetPoint()
	if rawget(self, "__point") then return table.unpack(self.__point) end
	return "TOPLEFT", UIParent, "TOPLEFT", 10, -10
end
function methods:SetPoint(...) self.__point = { ... } end
function methods:GetStringHeight() return 14 * math.max(1, math.ceil(#(self.__text or "") / 60)) end
function methods:GetStringWidth() return 6 * #(self.__text or "") end
function methods:GetVerticalScroll() return self.__vscroll or 0 end
function methods:SetVerticalScroll(v) self.__vscroll = v end
-- A texture path "loads" unless it names something the test says is missing.
function methods:SetTexture(path) self.__texture = path; return not (type(path) == "string" and path:find("MISSING")) and 1 or nil end
function methods:GetTexture() return self.__texture end
function methods:GetFontString()
	if not rawget(self, "__fs") then self.__fs = newObject("FontString", nil, self) end
	return self.__fs
end
function methods:SetTextColor(r, g, b) self.__color = { r, g, b } end
function methods:GetTextColor() local c = rawget(self, "__color") or { 1, 1, 1 } return c[1], c[2], c[3] end
function methods:GetCenter() return 500, 400 end
function methods:HasFocus() return rawget(self, "__focus") end
function methods:SetFocus() self.__focus = true end
function methods:ClearFocus() self.__focus = nil end
frameMeta.__index = function (t, k)
	if methods[k] then return methods[k] end
	if type(k) == "string" and k:match("^%u") then return function () end end
	return nil
end

function CreateFrame(kind, name, parent, template)
	local f = newObject(kind, name, parent)
	if name and template then
		if template:find("Check") then _G[name .. "Text"] = newObject("FontString", nil, f) end
		if template:find("ScrollFrame") then _G[name .. "ScrollBar"] = newObject("Slider", nil, f) end
	end
	return f
end

function InCombatLockdown() return false end
function PlaySound() end
local modified = {}
function IsModifiedClick(what) return modified[what] or false end
function IsShiftKeyDown() return modified.SHIFT or false end
local inserted
function ChatEdit_InsertLink(link) inserted = link return true end
function ChatFrame_OpenChat() end
local dressedUp
function DressUpItemLink(link) dressedUp = link end
local lastMenu
function EasyMenu(items) lastMenu = items end
function CloseDropDownMenus() end
function UnitName() return "Tester" end
function UnitRace() return "Human", "Human" end
function UnitClass() return "Warrior", "WARRIOR" end
function UnitSex() return 2 end
function GetLocale() return "enUS" end
local inInstance = false
function IsInInstance() return inInstance end
function GetItemIcon() return "Interface\\Icons\\X" end
function GetCursorPosition() return 0, 0 end
function GetQuestDifficultyColor() return { r = 1, g = 1, b = 0 } end
function PanelTemplates_TabResize() end
function PanelTemplates_SelectTab(t) t.__selected = true end
function PanelTemplates_DeselectTab(t) t.__selected = false end
function SetPortraitToTexture(tex, path) tex.__portrait = path end
function GameTooltip_ShowCompareItem() end
function LFGGetDungeonInfoByID(id)
	local tex = { [6] = "DEADMINES", [18] = "SCARLETMONASTERY", [159] = "NAXXRAMAS" }
	return "x", 1, 1, 1, 1, 1, 1, 0, 1, tex[id] or "MISSING", 0, 5
end
local bindings = {}
function GetBindingAction(key) return bindings[key] or "" end
function SetBinding(key, action) bindings[key] = action end
function SaveBindings() end
function GetCurrentBindingSet() return 1 end
function GetBindingKey(action) for k, a in pairs(bindings) do if a == action then return k end end end
function select(n, ...) if n == "#" then return #{ ... } end return table.unpack({ ... }, n) end

ITEM_QUALITY_COLORS = {}
for i = 0, 7 do ITEM_QUALITY_COLORS[i] = { r = 1, g = 1, b = 1, hex = "|cffffffff" } end
INVTYPE_CHEST, INVTYPE_HEAD, INVTYPE_WEAPON = "Chest", "Head", "One-Hand"
LOCALIZED_CLASS_NAMES_MALE = { WARRIOR = "Warrior", MAGE = "Mage" }
RAID_CLASS_COLORS = { WARRIOR = { r = 0.78, g = 0.61, b = 0.43 }, MAGE = { r = 0.41, g = 0.8, b = 0.94 } }
UIParent = newObject("Frame", "UIParent")
WorldFrame = newObject("Frame", "WorldFrame")
Minimap = newObject("Frame", "Minimap")
GameTooltip = newObject("GameTooltip", "GameTooltip")
local tipLines = {}
function GameTooltip:SetOwner(owner) tipLines = {} self.__owner = owner end
function GameTooltip:IsOwned(f) return self.__owner == f end
function GameTooltip:AddLine(text) table.insert(tipLines, tostring(text)) end
function GameTooltip:AddDoubleLine(a, b) table.insert(tipLines, tostring(a) .. " | " .. tostring(b)) end
DEFAULT_CHAT_FRAME = { AddMessage = function (_, m) print("  [chat] " .. m) end }
GameFontHighlightSmall, GameFontNormal, GameFontHighlight = {}, {}, {}
SlashCmdList, UISpecialFrames = {}, {}

-- The scan tooltip: spell descriptions come back as gold lines, like the real one.
local spellText = {
	[5145] = { "Arcane Explosion", "Sends a wave of arcane energy." },
	[6432] = { "Smite Stomp", "Knocks down nearby enemies." },
	[9999] = { "Hidden Trigger" }, -- no description: left out
}
local origCreate = CreateFrame
function CreateFrame(kind, name, parent, template)
	local f = origCreate(kind, name, parent, template)
	if kind == "GameTooltip" and name then
		local lines = {}
		for i = 1, 4 do lines[i] = newObject("FontString", name .. "TextLeft" .. i, f) end
		function f:SetHyperlink(link)
			for i = 1, 4 do lines[i]:SetText(""); lines[i].__color = { 1, 1, 1 } end
			self.__n = 0
			local spell = tonumber(link:match("spell:(%d+)"))
			local s = spell and spellText[spell]
			if s then
				lines[1]:SetText(s[1])
				lines[2]:SetText("Instant")
				self.__n = 2
				if s[2] then
					lines[3]:SetText(s[2])
					lines[3].__color = { 1, 0.82, 0 }
					self.__n = 3
				end
			end
		end
		function f:NumLines() return self.__n or 0 end
	end
	return f
end

-- With DRAGON=1, a DragonUI stand-in, so the skinned branches run too.
if os.getenv("DRAGON") == "1" then
	NineSliceUtils = { GetLayout = function () return {} end, ApplyLayout = function () end }
	DragonUI = {
		_dir = "Interface\\AddOns\\DragonUI\\Textures\\", atlasinfo = {},
		SafeSetAtlas = function () return true end, SkinRedButton = function () end,
		CharacterPanel = { ReskinTab = function () end, ModernizeCloseButton = function () end,
			SkinCheckbox = function () end, ReskinScrollBar = function () end },
	}
end

-----------------------------------------
-- the game's items and spells

local items = { [5191] = { "Cruel Barb", 3 }, [5193] = { "Cape of the Brotherhood", 3 }, [2874] = { "Unsent Letter", 1 } }
function GetItemInfo(item)
	local id = type(item) == "number" and item or tonumber(tostring(item):match("item:(%d+)"))
	local d = id and items[id]
	if not d then return nil end
	return d[1], "|cff0070dd|Hitem:" .. id .. ":0:0:0:0:0:0:0:0|h[" .. d[1] .. "]|h|r", d[2], 20, 15, "Weapon", "Swords", 1, "", "Interface\\Icons\\X", 100
end
local spellNames = { [5145] = "Arcane Explosion", [6432] = "Smite Stomp", [9999] = "Hidden Trigger", [6603] = "Attack" }
function GetSpellInfo(id) return spellNames[id], nil, "Interface\\Icons\\S" end
function GetSpellLink(id) return "|Hspell:" .. id .. "|h[" .. tostring(spellNames[id]) .. "]|h" end

-----------------------------------------
-- events and the fake server

local outbox = {}
function SendAddonMessage(prefix, msg, channel)
	assert(prefix == "DJN" and channel == "WHISPER", "bad addon message")
	assert(#prefix + 1 + #msg <= 254, "addon message too long")
	table.insert(outbox, msg)
end

local eventFrames = {}
function methods:RegisterEvent(e) eventFrames[e] = eventFrames[e] or {}; table.insert(eventFrames[e], self) end
function methods:UnregisterEvent() end
local function fire(event, ...)
	for _, f in ipairs(eventFrames[event] or {}) do
		local ok, err = pcall(f.__scripts.OnEvent, f, event, ...)
		if not ok then report(event, err) end
	end
end

local function reply(msg)
	assert(#msg <= 250, "server message too long: " .. #msg)
	fire("CHAT_MSG_ADDON", "DJN", msg, "WHISPER", "Tester")
end

-- Like the server's SendRows: as many rows per message as fit, never splitting one.
local function list(code, req, meta, rows)
	reply(code .. "R:" .. req .. (meta and (":" .. meta) or ""))
	local header = code .. "D:" .. req .. ":"
	local line = ""
	for _, row in ipairs(rows or {}) do
		if line ~= "" and #header + #line + 1 + #row > 240 then
			reply(header .. line)
			line = ""
		end
		line = line == "" and row or (line .. ";" .. row)
	end
	if line ~= "" then reply(header .. line) end
	reply(code .. "E:" .. req)
end

local counts = {}
local sent = {}
local echoOnly = false -- a realm without the module: the whisper to yourself just comes back
local function serve(msg)
	if echoOnly then
		fire("CHAT_MSG_ADDON", "DJN", msg, "WHISPER", "Tester")
		return
	end
	local args = {}
	for part in (msg .. ":"):gmatch("([^:]*):") do table.insert(args, part) end
	local cmd, req = args[1], args[2]
	counts[cmd] = (counts[cmd] or 0) + 1
	table.insert(sent, msg)
	print("  -> " .. msg)
	if cmd == "HELLO" then
		reply("HELLO:" .. req .. ":1:15:4242:3")
	elseif cmd == "D" then
		list("D", req, "3", {
			"I,6,36,0,0,15,25,19,6,Deadmines,Westfall",
			"V,6,0,0,0,5,15,0,Normal",
			"I,18,189,0,0,27,45,31,18,Scarlet Monastery,Tirisfal Glades",
			"W,18,0,18,27,37,Graveyard", "W,18,1,165,30,40,Library",
			"V,18,0,0,0,5,27,0,Normal",
			"I,159,533,1,2,80,83,80,159,Naxxramas,Dragonblight",
			"V,159,0,0,2,10,80,13,10 Player", "V,159,1,1,2,25,80,13,25 Player", "V,159,101,2,0,40,60,6,40 Player",
		})
	elseif cmd == "B" then
		if args[3] == "6" then
			list("B", req, "6:3", { "3,0,646,0,1,Mr. Smite", "6,0,639,0,1,Edwin VanCleef", "100,255,596,1,1,Miner Johnson" })
		elseif args[3] == "18" then
			list("B", req, "18:3", { "0,0,3983,0,1,Interrogator Vishas", "1,0,4543,0,1,Bloodmage Thalnos", "2,1,3974,0,1,Houndmaster Loksey" })
		else
			list("B", req, args[3] .. ":2", { "0,0,15956,0,7,Anub'Rekhan", "8,0,16063,2,7,The Four Horsemen" })
		end
	elseif cmd == "S" then
		list("S", req, args[3] .. ":3", { "V,0,0,0,1,7200,64", "R,0,L,10,1,-", "R,0,I,2874,0,Unsent Letter",
			"V,1,1,13,0,0,0", "V,101,1,6,0,0,0" })
	elseif cmd == "A" then
		list("A", req, args[3] .. ":" .. args[4] .. ":" .. args[5], {
			"U,639,20,3,3500,0,Edwin VanCleef", "S,639,5145,97,1500", "S,639,9999,0,0",
			"U,646,19,1,2000,1,Defias Companion", "S,646,6432,1024,0",
		})
	elseif cmd == "L" then
		list("L", req, args[3] .. ":" .. args[4] .. ":" .. args[5] .. ":3", {
			"5191,1650,16,1,1,3,2,7,13,24,19,6,Cruel Barb",
			"5193,2500,16,1,1,3,4,1,16,24,19,6,Cape of the Brotherhood",
			"2874,10000,1,1,1,1,12,0,0,1,0,6,Unsent Letter",
			"7230,800,0,1,1,3,4,4,5,24,19,6,Smite's Plate",
		})
	elseif cmd == "Q" then
		list("Q", req, args[3] .. ":3", {
			"Q,166,0,22,14,8,2650,1800,C,234,The Defias Brotherhood,Gryan Stoutmantle,Westfall",
			"R,166,-,5193*1/2874*1",
			"Q,373,4,22,22,0,1200,0,I,2874,The Unsent Letter,Unsent Letter,Drops in the dungeon",
			"R,373,-,-",
			"Q,214,3,20,15,0,0,0,C,235,Red Silk Bandanas,Scout Riell,Westfall",
			"R,214,5191*1,-",
		})
	elseif cmd == "QD" then
		list("T", req, args[3], { "O,Kill Edwin VanCleef and bring his head to $N.", "K,639,1,Edwin VanCleef",
			"I,3637,1,Head of VanCleef", "P,155,1,The Defias Brotherhood" })
	elseif cmd == "H" then
		reply("H:" .. req .. ":159:101")
	elseif cmd == "M" then
		reply("M:" .. req .. ":ok:Westfall (42%2C 72)%2C 120 yards away")
	elseif cmd == "F" then
		list("F", req, "3", { "I,6", "B,6,6", "L,6,6,0,5191,1650,3" })
	else
		print("  !! unknown command " .. cmd)
	end
end

local function tick(seconds)
	for _ = 1, math.ceil(seconds / 0.05) do
		now = now + 0.05
		for _, f in ipairs(allFrames) do
			if f.__shown and f.__scripts.OnUpdate then
				local ok, err = pcall(f.__scripts.OnUpdate, f, 0.05)
				if not ok then report("OnUpdate", err) end
			end
		end
		while #outbox > 0 do serve(table.remove(outbox, 1)) end
	end
end

local function step(name, fn)
	print("== " .. name)
	local ok, err = xpcall(fn, debug.traceback)
	if not ok then errors = errors + 1; print("ERROR: " .. err) end
	tick(0.5)
end

for line in io.lines(dir .. "/DungeonJournal.toc") do
	if line:match("%.lua$") then
		local chunk, err = loadfile(dir .. "/" .. line)
		if not chunk then report("load " .. line, err) else
			local ok, e = pcall(chunk, "DungeonJournal", {})
			if not ok then report("run " .. line, e) end
		end
	end
end

local DJ = DungeonJournal
local function find(pred)
	for _, f in ipairs(allFrames) do if pred(f) then return f end end
end
local function findAll(pred)
	local out = {}
	for _, f in ipairs(allFrames) do if pred(f) then table.insert(out, f) end end
	return out
end
local function textShown(pattern)
	return find(function (f) return f.__kind == "FontString" and f.__text:find(pattern) and f:IsVisible() end)
end
local function buttonShown(text)
	return find(function (f) return f.__kind == "Button" and f.__text == text and f:IsVisible() end)
		or find(function (f) return f.__kind == "Button" and rawget(f, "__fs") and f.__fs.__text == text and f:IsVisible() end)
end

step("login, hello, key binding", function ()
	fire("ADDON_LOADED", "DungeonJournal")
	fire("PLAYER_LOGIN")
	tick(4)
	assert(DJ.serverReady, "no HELLO answer")
	assert(DJ.state == 3, "era state not read")
	assert(DJ.Has(DJ.HELLO_PROGRESSION), "progression flag not read")
	assert(bindings["SHIFT-J"] == "DUNGEONJOURNAL_TOGGLE", "Shift-J was not bound")
end)

step("open the journal: classic dungeons", function ()
	DJ.Toggle()
	tick(1)
	assert(DungeonJournalFrame:IsShown(), "window not shown")
	assert(counts.D == 1, "instance list not asked once")
	assert(textShown("^Deadmines$") and textShown("^Scarlet Monastery$"), "classic dungeon tiles missing")
	assert(not textShown("^Naxxramas$"), "a raid shows among the dungeons")
end)

step("raids tab: 40 player Naxxramas under classic, locked by era", function ()
	DJ.Show({ raids = true })
	tick(0.5)
	assert(textShown("^Naxxramas$"), "Naxxramas missing from classic raids")
	assert(textShown("Progression stage 6"), "era lock not shown")
	DJ.Show({ tier = 2 })
	tick(0.5)
	assert(textShown("^Naxxramas$"), "Naxxramas missing from Wrath raids")
	assert(textShown("Progression stage 13"), "Wrath era lock not shown")
end)

step("open Deadmines: bosses, rares, overview, lockout and requirements", function ()
	DJ.OpenInstance(6)
	tick(1)
	assert(DJ.view.page == "instance" and DJ.view.tier == 0, "didn't move to the instance under its tier")
	assert(textShown("^Edwin VanCleef$") and textShown("^Mr. Smite$"), "boss rows missing")
	assert(textShown("^Rares$") and textShown("^Miner Johnson$"), "rares section missing")
	assert(textShown("Difficulties"), "instance overview missing")
	assert(textShown("saved, 1/2 defeated"), "lockout not shown: bit 6 is VanCleef")
	assert(textShown("Carry Unsent Letter"), "requirement missing")
	assert(textShown("1 to pick up"), "quest summary missing")
end)

step("entrance on the map", function ()
	local b = buttonShown("Show on map")
	assert(b, "no entrance button")
	b.__scripts.OnClick(b)
	tick(0.5)
	assert(sent[#sent]:match("^M:%d+:E:6$"), "entrance flag not asked: " .. sent[#sent])
end)

step("boss: overview, abilities", function ()
	DJ.Show({ boss = 6, tab = "overview" })
	tick(1)
	assert(textShown("3,500 health"), "boss health missing")
	assert(textShown("Defeated this lockout"), "kill not shown")
	DJ.Show({ tab = "abilities" })
	tick(0.5)
	assert(textShown("^Arcane Explosion$"), "ability missing")
	assert(textShown("Interruptible") and textShown("Magic"), "ability tags missing")
	assert(not textShown("^Hidden Trigger$"), "a spell without description was listed")
	assert(textShown("^Add: Defias Companion$") and textShown("Knockback"), "add missing")
	assert(textShown("1.5 sec"), "cast time missing")
end)

step("loot: warrior filter, slots, links", function ()
	DJ.Show({ tab = "loot" })
	tick(0.5)
	assert(textShown("^Cruel Barb$"), "weapon missing")
	assert(textShown("^Smite's Plate$") == nil, "plate below level 40 shown to a warrior (wants mail)")
	assert(textShown("^Cape of the Brotherhood$"), "cloak missing")
	assert(textShown("^17%%$") and textShown("^100%%$"), "drop chance missing")
	assert(textShown("of 4 items"), "filtered count missing")
	DungeonJournalCharDB.lootClass = false
	DJ.ctx.inst = DJ.Instance(6)
	DJ.RefreshPage()
	tick(0.5)
	assert(textShown("^Smite's Plate$"), "All classes didn't show everything")
	local row = find(function (f) return f.__kind == "Button" and rawget(f, "data") and f.data.item == 5191 and f:IsVisible() end)
	modified.CHATLINK = true
	row.__scripts.OnClick(row)
	modified.CHATLINK = nil
	assert(inserted and inserted:find("Cruel Barb"), "Shift-click didn't link")
	row.__scripts.OnEnter(row)
	DungeonJournalCharDB.lootSlot = "back"
	DJ.RefreshPage()
	tick(0.5)
	assert(textShown("^Cape of the Brotherhood$") and not textShown("^Cruel Barb$"), "slot filter")
	DungeonJournalCharDB.lootSlot = nil
end)

step("every boss's loot", function ()
	local from = #sent
	DJ.Show({ boss = false, tab = "loot" })
	tick(0.5)
	local asked = false
	for i = from + 1, #sent do if sent[i]:match("^L:%d+:6:all:0$") then asked = true end end
	assert(asked, "every-boss loot not asked")
	assert(textShown("Edwin VanCleef|r"), "boss name missing from every-boss rows")
end)

step("quests: status, rewards, objectives, map flag", function ()
	DJ.Show({ tab = "quests" })
	tick(0.5)
	assert(textShown("The Defias Brotherhood"), "quest missing")
	assert(textShown("Needs level 22"), "low-level state missing")
	assert(textShown("Starts from Unsent Letter"), "item starter missing")
	assert(textShown("2,650 XP"), "XP missing")
	local row = find(function (f) return f.__kind == "Button" and rawget(f, "quest") and f.quest.id == 166 and f:IsVisible() end)
	row.__scripts.OnClick(row)
	tick(0.5)
	assert(textShown("bring his head to Tester"), "objectives missing or $N not filled")
	assert(textShown("Slay Edwin VanCleef"), "kill objective missing")
	local map = buttonShown("Show giver on map")
	assert(map, "no giver map button")
	map.__scripts.OnClick(map)
	tick(0.5)
	assert(sent[#sent]:match("^M:%d+:Q:166$"), "giver flag not asked")
	DungeonJournalCharDB.hideDoneQuests = true
	DJ.RefreshPage()
	tick(0.5)
	assert(not textShown("Red Silk Bandanas"), "done quest not hidden")
end)

step("search: instances, bosses, loot", function ()
	DJ.searchBox:SetFocus()
	DJ.searchBox:SetText("vanc")
	DJ.searchBox.__scripts.OnTextChanged(DJ.searchBox)
	tick(1)
	assert(sent[#sent]:match("^F:%d+:vanc$"), "search not sent: " .. sent[#sent])
	local item = find(function (f) return f.__kind == "Button" and rawget(f, "item") == 5191 and f:IsVisible() end)
	assert(item, "loot result missing")
	item.__scripts.OnClick(item)
	tick(0.5)
	assert(DJ.view.tab == "loot" and DJ.view.boss == 6, "result didn't open the boss's loot")
end)

step("Naxxramas: 40 player tab under classic", function ()
	DJ.OpenInstance(159, { variant = 101 })
	tick(0.5)
	assert(DJ.view.tier == 0 and DJ.view.variant == 101, "40 player tab not picked")
	assert(textShown("locked: progression stage 6"), "40 player lock missing")
	local menu = buttonShown("40 Player")
	assert(menu, "difficulty menu doesn't show the 40 player tab")
	menu.__scripts.OnClick(menu)
	assert(lastMenu and #lastMenu == 1 and lastMenu[1].text:find("40 Player"), "classic tier offers non-classic tabs")
end)

step("open inside an instance jumps to it", function ()
	DungeonJournalFrame:Hide()
	DJ.Show({ page = "home" })
	inInstance = true
	DJ.Toggle()
	tick(1)
	assert(DJ.view.page == "instance" and DJ.view.instance == 159 and DJ.view.variant == 101, "didn't open on the current instance")
	inInstance = false
end)

step("cache: no second instance list with the same stamp", function ()
	local before = counts.D
	DungeonJournalFrame:Hide()
	fire("PLAYER_LOGIN")
	tick(4)
	DJ.Toggle()
	tick(1)
	assert(counts.D == before, "instance list asked again with an unchanged stamp")
end)

step("realm without the module: the echo isn't an answer", function ()
	local noServer = false
	DJ.On("NO_SERVER", function () noServer = true end)
	DJ.serverReady = false
	echoOnly = true
	DJ.SayHello()
	tick(7)
	assert(not DJ.serverReady, "the echoed HELLO passed for the server's answer")
	assert(noServer, "NO_SERVER didn't fire")
	echoOnly = false
	DJ.SayHello()
	tick(1)
	assert(DJ.serverReady, "HELLO after the module came back")
end)

step("slash commands", function ()
	SlashCmdList.DUNGEONJOURNAL("help")
	SlashCmdList.DUNGEONJOURNAL("minimap")
	assert(DungeonJournalDB.minimap.hide, "minimap toggle")
	SlashCmdList.DUNGEONJOURNAL("minimap")
	SlashCmdList.DUNGEONJOURNAL("reset")
	SlashCmdList.DUNGEONJOURNAL("smite")
	tick(1)
end)

print(errors == 0 and "OK" or (errors .. " error(s)"))
os.exit(errors == 0 and 0 or 1)
