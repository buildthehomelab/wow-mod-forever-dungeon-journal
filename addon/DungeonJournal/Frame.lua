-- The journal window, laid out like retail's Adventure Guide: a navigation bar across the top
-- (Home > tier > instance > boss) with a search box, and below it either the instance list
-- (InstanceList.lua) or an instance's page (Instance.lua). A stock Blizzard dialog frame, skinned
-- by DragonUI when it's loaded. Drag it by the title bar; it remembers where it was put.

local DJ = DungeonJournal

local WIDTH, HEIGHT = 820, 560

-- 3.3.5 has no SetShown.
local function setShown(region, shown) if shown then region:Show() else region:Hide() end end
DJ.SetShown = setShown

local frame = CreateFrame("Frame", "DungeonJournalFrame", UIParent)
frame:SetSize(WIDTH, HEIGHT)
frame:EnableMouse(true)
frame:SetToplevel(true)
frame:SetMovable(true)
frame:SetClampedToScreen(true)
frame:SetFrameStrata("HIGH")
frame:Hide()
table.insert(UISpecialFrames, "DungeonJournalFrame")
DJ.frame = frame

local function placeFrame()
	frame:ClearAllPoints()
	local p = DungeonJournalDB and DungeonJournalDB.position
	if p then
		frame:SetPoint(p[1], UIParent, p[2], p[3], p[4])
	else
		frame:SetPoint("CENTER", UIParent, "CENTER", 0, 30)
	end
end

DJ.On("RESET_POSITION", function ()
	DungeonJournalDB.position = nil
	placeFrame()
end)

local dragon, contentTop = DJ.DressWindow(frame, {
	portrait = true,
	closeName = "DungeonJournalFrameCloseButton",
	onMoved = function ()
		local point, _, relativePoint, x, y = frame:GetPoint(1)
		DungeonJournalDB.position = { point, relativePoint, x, y }
	end,
})
frame.title:SetText("Dungeon Journal")
DJ.dragon = dragon
if frame.portrait then DJ.SetPortrait(frame.portrait, "Interface\\Icons\\INV_Misc_Book_09") end

local content = CreateFrame("Frame", nil, frame)
content:SetPoint("TOPLEFT", frame, "TOPLEFT", dragon and 10 or 16, contentTop - (dragon and 8 or 0))
content:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", dragon and -10 or -16, dragon and 10 or 16)
DJ.content = content

-----------------------------------------
-- view state: which page, tier, instance, boss, tab and difficulty

DJ.view = { page = "home", tier = 0, raids = false, instance = nil, boss = nil, variant = nil, tab = "overview" }

local function saveView()
	if DungeonJournalCharDB then
		local v = DJ.view
		DungeonJournalCharDB.view = { page = v.page, tier = v.tier, raids = v.raids, instance = v.instance, boss = v.boss,
			variant = v.variant, tab = v.tab }
	end
end

-- Moves the journal somewhere: DJ.Show({ page = "instance", instance = 6, boss = 3, tab = "loot" }).
-- Fields left out keep their value, except that a new instance clears the boss and difficulty.
function DJ.Show(changes)
	local v = DJ.view
	if changes.instance and changes.instance ~= v.instance then
		v.boss = nil
		v.variant = nil
		v.lootFilterBoss = nil
	end
	for k, val in pairs(changes) do v[k] = val end
	if changes.boss == false then v.boss = nil end
	saveView()
	DJ.Fire("VIEW")
end

-----------------------------------------
-- navigation bar

local nav = CreateFrame("Frame", nil, content)
nav:SetPoint("TOPLEFT", content, "TOPLEFT", 0, 0)
nav:SetPoint("TOPRIGHT", content, "TOPRIGHT", 0, 0)
nav:SetHeight(26)
DJ.nav = nav

local navBg = DJ.CreateInset(nav)
navBg:SetAllPoints(nav)

local crumbs = {}
local function crumb(i)
	if crumbs[i] then return crumbs[i] end
	local b = CreateFrame("Button", nil, nav)
	b:SetHeight(22)
	b.text = b:CreateFontString(nil, "OVERLAY", "GameFontNormal")
	b.text:SetPoint("LEFT", b, "LEFT", 6, 0)
	b.arrow = b:CreateTexture(nil, "OVERLAY")
	b.arrow:SetTexture("Interface\\ChatFrame\\ChatFrameExpandArrow")
	b.arrow:SetSize(10, 12)
	b.arrow:SetPoint("LEFT", b.text, "RIGHT", 6, 0)
	local hl = b:CreateTexture(nil, "HIGHLIGHT")
	hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
	hl:SetBlendMode("ADD")
	hl:SetAllPoints(b)
	b:SetScript("OnClick", function (self) if self.go then self.go() end end)
	crumbs[i] = b
	return b
end

local homeButton = DJ.CreateButton(nav, "Home", 64, 20)
homeButton:SetPoint("LEFT", nav, "LEFT", 4, 0)
homeButton:SetScript("OnClick", function () DJ.Show({ page = "home" }) end)

local function setCrumbs(list)
	local anchor = homeButton
	for i, c in ipairs(list) do
		local b = crumb(i)
		b.text:SetText(c.text)
		b.go = c.go
		b:SetWidth(b.text:GetStringWidth() + 26)
		b:ClearAllPoints()
		b:SetPoint("LEFT", anchor, "RIGHT", i == 1 and 6 or 0, 0)
		setShown(b.arrow, i < #list)
		if i == #list then b.text:SetTextColor(1, 1, 1) else b.text:SetTextColor(1, 0.82, 0) end
		b:Show()
		anchor = b
	end
	for i = #list + 1, #crumbs do crumbs[i]:Hide() end
end

function DJ.UpdateCrumbs()
	local v = DJ.view
	local list = {}
	if v.page == "home" or v.page == "instance" then
		local tier = v.tier or 0
		table.insert(list, { text = DJ.EXPANSIONS[tier] or "", go = function () DJ.Show({ page = "home" }) end })
	end
	if v.page == "instance" then
		local inst = DJ.Instance(v.instance)
		if inst then
			table.insert(list, { text = inst.name, go = function () DJ.Show({ boss = false, tab = "overview" }) end })
			if v.boss and DJ.currentBosses then
				for _, b in ipairs(DJ.currentBosses) do
					if b.key == v.boss then table.insert(list, { text = b.name }) break end
				end
			end
		end
	end
	setCrumbs(list)
end

-----------------------------------------
-- search: instances, bosses and loot by name, answered by the server

local search = DJ.CreateEditBox(nav, 190)
search:SetPoint("RIGHT", nav, "RIGHT", -8, 0)
search:SetTextInsets(16, 4, 0, 0)
do
	local glass = search:CreateTexture(nil, "OVERLAY")
	glass:SetTexture("Interface\\Minimap\\Tracking\\None")
	glass:SetSize(14, 14)
	glass:SetPoint("LEFT", search, "LEFT", 0, 0)
	glass:SetVertexColor(0.7, 0.7, 0.7)
	search.hint = search:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	search.hint:SetPoint("LEFT", search, "LEFT", 17, 0)
	search.hint:SetText("Search bosses, dungeons, loot")
	search.hint:SetTextColor(0.6, 0.6, 0.6)
	if dragon then
		local edge = CreateFrame("Frame", nil, search)
		edge:SetPoint("TOPLEFT", search, "TOPLEFT", -6, 3)
		edge:SetPoint("BOTTOMRIGHT", search, "BOTTOMRIGHT", 2, -3)
		edge:SetBackdrop({ edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border", edgeSize = 12 })
		edge:SetBackdropBorderColor(0.75, 0.6, 0.25, 1)
		edge:EnableMouse(false)
	end
end
DJ.searchBox = search

local results = CreateFrame("Frame", nil, frame)
results:SetPoint("TOPRIGHT", search, "BOTTOMRIGHT", 4, -2)
results:SetWidth(330)
results:SetFrameLevel(frame:GetFrameLevel() + 60)
results:SetBackdrop({
	bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
	edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
	tile = true, tileSize = 16, edgeSize = 16,
	insets = { left = 4, right = 4, top = 4, bottom = 4 },
})
results:SetBackdropColor(0, 0, 0, 0.92)
results:EnableMouse(true)
results:Hide()

local MAX_RESULTS = 12
local resultRows = {}
for i = 1, MAX_RESULTS do
	local b = CreateFrame("Button", nil, results)
	b:SetHeight(28)
	b:SetPoint("TOPLEFT", results, "TOPLEFT", 6, -6 - (i - 1) * 28)
	b:SetPoint("RIGHT", results, "RIGHT", -6, 0)
	b.icon = b:CreateTexture(nil, "ARTWORK")
	b.icon:SetSize(22, 22)
	b.icon:SetPoint("LEFT", b, "LEFT", 2, 0)
	b.text = b:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	b.text:SetPoint("TOPLEFT", b.icon, "TOPRIGHT", 6, 1)
	b.text:SetPoint("RIGHT", b, "RIGHT", -4, 0)
	b.text:SetJustifyH("LEFT")
	b.sub = b:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
	b.sub:SetPoint("TOPLEFT", b.text, "BOTTOMLEFT", 0, -1)
	b.sub:SetPoint("RIGHT", b, "RIGHT", -4, 0)
	b.sub:SetJustifyH("LEFT")
	local hl = b:CreateTexture(nil, "HIGHLIGHT")
	hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
	hl:SetBlendMode("ADD")
	hl:SetAllPoints(b)
	b:SetScript("OnClick", function (self)
		results:Hide()
		search:ClearFocus()
		if self.go then self.go() end
	end)
	b:SetScript("OnEnter", function (self)
		if self.item then DJ.ShowItemTooltip(self, self.item) end
	end)
	b:SetScript("OnLeave", function () GameTooltip:Hide() end)
	resultRows[i] = b
end
local noResults = results:CreateFontString(nil, "OVERLAY", "GameFontDisable")
noResults:SetPoint("TOP", results, "TOP", 0, -12)

local function showResults(list, text)
	if not search:HasFocus() and search:GetText() == "" then results:Hide() return end
	list = list or {}
	DJ.LoadInstances(function (byId)
		local shown = 0
		for _, r in ipairs(list) do
			if shown >= MAX_RESULTS then break end
			local inst = byId and byId[r.instance]
			if inst then
				shown = shown + 1
				local b = resultRows[shown]
				b.item = nil
				if r.kind == "I" then
					b.icon:SetTexture(inst.texture and ("Interface\\LFGFrame\\LFGIcon-" .. inst.texture) or "Interface\\Icons\\INV_Misc_Map_01")
					b.icon:SetTexCoord(0, 1, 0, 1)
					b.text:SetText(inst.name)
					b.text:SetTextColor(1, 0.82, 0)
					b.sub:SetText((inst.raid and "Raid" or "Dungeon") .. ", level " .. inst.minLevel .. "-" .. inst.maxLevel)
					b.go = function () DJ.OpenInstance(inst.id) end
				elseif r.kind == "B" then
					b.icon:SetTexture("Interface\\TargetingFrame\\UI-TargetingFrame-Skull")
					b.icon:SetTexCoord(0, 1, 0, 1)
					b.text:SetText("Boss")
					b.text:SetTextColor(1, 1, 1)
					b.sub:SetText(inst.name)
					b.go = function () DJ.OpenInstance(inst.id, { boss = r.boss }) end
					DJ.LoadBosses(inst, function (bosses)
						for _, boss in ipairs(bosses or {}) do
							if boss.key == r.boss then b.text:SetText(boss.name) end
						end
					end)
				else
					b.item = r.item
					b.icon:SetTexture(DJ.ItemIcon(r.item))
					b.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)
					local name = GetItemInfo(r.item)
					local cr, cg, cb = DJ.QualityColor(r.quality)
					b.text:SetText(name or ("item " .. r.item))
					b.text:SetTextColor(cr, cg, cb)
					b.sub:SetText(DJ.Chance(r.chance) .. " - " .. inst.name)
					if not name then DJ.QueryItem(r.item) end
					b.go = function () DJ.OpenInstance(inst.id, { boss = r.boss, variant = r.variant, tab = "loot" }) end
					DJ.LoadBosses(inst, function (bosses)
						for _, boss in ipairs(bosses or {}) do
							if boss.key == r.boss then b.sub:SetText(DJ.Chance(r.chance) .. " - " .. boss.name .. ", " .. inst.name) end
						end
					end)
				end
				b:Show()
			end
		end
		for i = shown + 1, MAX_RESULTS do resultRows[i]:Hide() end
		if shown == 0 then
			noResults:SetText(#text < 3 and "Type at least 3 letters" or "Nothing found")
			noResults:Show()
			results:SetHeight(36)
		else
			noResults:Hide()
			results:SetHeight(12 + shown * 28)
		end
		results:Show()
	end)
end

local lastText = ""
search:SetScript("OnTextChanged", function (self)
	local text = self:GetText() or ""
	setShown(self.hint, text == "" and not self:HasFocus())
	if text == lastText then return end
	lastText = text
	if text == "" then results:Hide() return end
	DJ.Debounce("search", 0.35, function ()
		if search:GetText() ~= text then return end
		if #text < 3 then showResults({}, text) return end
		DJ.Search(text, function (list) if search:GetText() == text then showResults(list, text) end end)
	end)
end)
search:SetScript("OnEditFocusGained", function (self) self.hint:Hide() end)
search:SetScript("OnEditFocusLost", function (self)
	setShown(self.hint, (self:GetText() or "") == "")
	DJ.After(0.2, function () if not search:HasFocus() then results:Hide() end end)
end)
search:SetScript("OnEscapePressed", function (self)
	self:SetText("")
	self:ClearFocus()
	results:Hide()
end)

DJ.On("SEARCH", function (text)
	search:SetText(text or "")
	search:SetFocus()
end)

-----------------------------------------
-- pages

local pageArea = CreateFrame("Frame", nil, content)
pageArea:SetPoint("TOPLEFT", nav, "BOTTOMLEFT", 0, -6)
pageArea:SetPoint("BOTTOMRIGHT", content, "BOTTOMRIGHT", 0, 0)
DJ.pageArea = pageArea

local status = pageArea:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
status:SetPoint("CENTER", pageArea, "CENTER", 0, 20)
status:SetWidth(520)
local statusSub = pageArea:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
statusSub:SetPoint("TOP", status, "BOTTOM", 0, -10)
statusSub:SetWidth(520)

local function showStatus(title, sub)
	status:SetText(title or "")
	statusSub:SetText(sub or "")
	setShown(status, title ~= nil)
	setShown(statusSub, title ~= nil)
end
DJ.ShowStatus = showStatus

local pages = {}
function DJ.RegisterPage(name, page)
	pages[name] = page
	page:Hide()
end

local function refresh()
	if not frame:IsShown() then return end
	if not DJ.serverReady then
		for _, p in pairs(pages) do p:Hide() end
		if DJ.noServer then
			showStatus("The journal needs the realm's journal module",
				"This realm doesn't answer the Dungeon Journal yet (mod-forever-dungeon-journal). Ask the realm's admin, or try again with /dj refresh.")
		else
			showStatus("Opening the journal...")
		end
		return
	end
	showStatus(nil)
	DJ.LoadInstances(function (byId)
		if not byId then
			showStatus("The journal couldn't be loaded", "Try again in a moment with /dj refresh.")
			return
		end
		local v = DJ.view
		if v.page == "instance" and not byId[v.instance] then v.page = "home" end
		for name, p in pairs(pages) do
			if name == v.page then p:Show() ; if p.Refresh then p:Refresh() end else p:Hide() end
		end
		DJ.UpdateCrumbs()
	end)
end
DJ.RefreshPage = refresh

DJ.On("VIEW", refresh)
DJ.On("READY", function ()
	DJ.noServer = nil
	refresh()
end)
DJ.On("NO_SERVER", function ()
	DJ.noServer = true
	refresh()
end)
DJ.On("CACHE_RESET", refresh)

frame:SetScript("OnShow", function ()
	PlaySound("igCharacterInfoOpen")
	search:ClearFocus()
	refresh()
end)
frame:SetScript("OnHide", function ()
	PlaySound("igCharacterInfoClose")
	results:Hide()
	search:ClearFocus()
end)

-----------------------------------------
-- opening

-- Opens an instance's page; extra = { boss =, variant =, tab = }.
function DJ.OpenInstance(id, extra)
	DJ.LoadInstances(function (byId)
		local inst = byId and byId[id]
		if not inst then return end
		local changes = { page = "instance", instance = id, tab = "overview" }
		-- Show it under the tier of the tab asked for, or the one it was listed under.
		if extra and extra.variant then
			local v = DJ.Variant(inst, extra.variant)
			if v then changes.tier = v.expansion end
		elseif not inst.tiers[DJ.view.tier or 0] then
			changes.tier = inst.expansion
			for _, v in ipairs(inst.variants) do
				if not DJ.IsLocked(v) then changes.tier = v.expansion break end
			end
		end
		if extra then for k, val in pairs(extra) do changes[k] = val end end
		if not frame:IsShown() then frame:Show() end
		DJ.Show(changes)
	end)
end

local openedHere = false
function DJ.Open()
	if not frame:IsShown() then
		placeFrame()
		frame:Show()
	end
	-- Inside an instance the journal opens on it, like retail's.
	if IsInInstance() and DJ.serverReady then
		DJ.Here(function (id, variant)
			if id then
				if DJ.view.page == "instance" and DJ.view.instance == id and openedHere then return end
				openedHere = true
				DJ.OpenInstance(id, { variant = variant })
			end
		end)
	end
end

function DJ.Toggle()
	if frame:IsShown() then frame:Hide() else DJ.Open() end
end

DJ.On("ZONE", function () openedHere = false end)

DJ.On("LOGIN", function ()
	placeFrame()
	local saved = DungeonJournalCharDB and DungeonJournalCharDB.view
	if saved then for k, val in pairs(saved) do DJ.view[k] = val end end
end)
