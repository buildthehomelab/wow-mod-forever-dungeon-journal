-- The journal window: one tab along the bottom per part of the journal, as retail's Collections
-- window has. Dungeons and Raids are laid out like retail's Adventure Guide: a navigation bar
-- across the top (Home > tier > instance > boss) with a search box, and below it either the
-- instance list (InstanceList.lua) or an instance's page (Instance.lua). Mounts, Pets and Toys
-- are Collections.lua's; other addons can add tabs of their own (Journal.RegisterTab, below).
-- A stock Blizzard dialog frame, skinned by DragonUI when it's loaded. Drag it by the title bar;
-- it remembers where it was put.

local J = Journal

local WIDTH, HEIGHT = 900, 580

-- 3.3.5 has no SetShown.
local function setShown(region, shown) if shown then region:Show() else region:Hide() end end
J.SetShown = setShown

local frame = CreateFrame("Frame", "JournalFrame", UIParent)
frame:SetSize(WIDTH, HEIGHT)
frame:EnableMouse(true)
frame:SetToplevel(true)
frame:SetMovable(true)
frame:SetClampedToScreen(true)
frame:SetFrameStrata("HIGH")
frame:Hide()
table.insert(UISpecialFrames, "JournalFrame")
J.frame = frame

local function placeFrame()
	frame:ClearAllPoints()
	local p = JournalDB and JournalDB.position
	if p then
		frame:SetPoint(p[1], UIParent, p[2], p[3], p[4])
	else
		frame:SetPoint("CENTER", UIParent, "CENTER", 0, 30)
	end
end

J.On("RESET_POSITION", function ()
	JournalDB.position = nil
	placeFrame()
end)

local dragon, contentTop = J.DressWindow(frame, {
	portrait = true,
	closeName = "JournalFrameCloseButton",
	onMoved = function ()
		local point, _, relativePoint, x, y = frame:GetPoint(1)
		JournalDB.position = { point, relativePoint, x, y }
	end,
})
frame.title:SetText("Journal")
J.dragon = dragon
if frame.portrait then J.SetPortrait(frame.portrait, "Interface\\Icons\\INV_Misc_Book_09") end

local content = CreateFrame("Frame", nil, frame)
content:SetPoint("TOPLEFT", frame, "TOPLEFT", dragon and 10 or 16, contentTop - (dragon and 8 or 0))
content:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", dragon and -10 or -16, dragon and 10 or 16)
J.content = content

-- Dungeons and Raids share this panel: the navigation bar and the pages under it.
local dungeonPanel = CreateFrame("Frame", nil, content)
dungeonPanel:SetAllPoints(content)
dungeonPanel:Hide()
J.dungeonPanel = dungeonPanel

-----------------------------------------
-- view state: which window tab (section), and in Dungeons and Raids which page, tier, instance,
-- boss, tab and difficulty

J.view = { section = "dungeons", page = "home", tier = 0, raids = false, instance = nil, boss = nil, variant = nil,
	tab = "overview" }

local function saveView()
	if JournalCharDB then
		local v = J.view
		JournalCharDB.view = { section = v.section, page = v.page, tier = v.tier, raids = v.raids, instance = v.instance,
			boss = v.boss, variant = v.variant, tab = v.tab }
	end
end

-- Moves the journal somewhere: J.Show({ page = "instance", instance = 6, boss = 3, tab = "loot" }).
-- Fields left out keep their value, except that a new instance clears the boss and difficulty.
function J.Show(changes)
	local v = J.view
	if changes.instance and changes.instance ~= v.instance then
		v.boss = nil
		v.variant = nil
		v.lootFilterBoss = nil
	end
	for k, val in pairs(changes) do v[k] = val end
	if changes.boss == false then v.boss = nil end
	-- Raids and dungeons are tabs of their own.
	if changes.raids ~= nil and (v.section == "dungeons" or v.section == "raids") then
		v.section = v.raids and "raids" or "dungeons"
	end
	saveView()
	J.Fire("VIEW")
end

-----------------------------------------
-- navigation bar

local nav = CreateFrame("Frame", nil, dungeonPanel)
nav:SetPoint("TOPLEFT", dungeonPanel, "TOPLEFT", 0, 0)
nav:SetPoint("TOPRIGHT", dungeonPanel, "TOPRIGHT", 0, 0)
nav:SetHeight(26)
J.nav = nav

local navBg = J.CreateInset(nav)
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

local homeButton = J.CreateButton(nav, "Home", 64, 20)
homeButton:SetPoint("LEFT", nav, "LEFT", 4, 0)
homeButton:SetScript("OnClick", function () J.Show({ page = "home" }) end)

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

function J.UpdateCrumbs()
	local v = J.view
	local list = {}
	if v.page == "home" or v.page == "instance" then
		local tier = v.tier or 0
		table.insert(list, { text = J.EXPANSIONS[tier] or "", go = function () J.Show({ page = "home" }) end })
	end
	if v.page == "instance" then
		local inst = J.Instance(v.instance)
		if inst then
			table.insert(list, { text = inst.name, go = function () J.Show({ boss = false, tab = "overview" }) end })
			if v.boss and J.currentBosses then
				for _, b in ipairs(J.currentBosses) do
					if b.key == v.boss then table.insert(list, { text = b.name }) break end
				end
			end
		end
	end
	setCrumbs(list)
end

-----------------------------------------
-- search: instances, bosses and loot by name, answered by the server

local search = J.CreateEditBox(nav, 190)
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
J.searchBox = search

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
		if self.item then J.ShowItemTooltip(self, self.item) end
	end)
	b:SetScript("OnLeave", function () GameTooltip:Hide() end)
	resultRows[i] = b
end
local noResults = results:CreateFontString(nil, "OVERLAY", "GameFontDisable")
noResults:SetPoint("TOP", results, "TOP", 0, -12)

local function showResults(list, text)
	if not search:HasFocus() and search:GetText() == "" then results:Hide() return end
	list = list or {}
	J.LoadInstances(function (byId)
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
					b.go = function () J.OpenInstance(inst.id) end
				elseif r.kind == "B" then
					b.icon:SetTexture("Interface\\TargetingFrame\\UI-TargetingFrame-Skull")
					b.icon:SetTexCoord(0, 1, 0, 1)
					b.text:SetText("Boss")
					b.text:SetTextColor(1, 1, 1)
					b.sub:SetText(inst.name)
					b.go = function () J.OpenInstance(inst.id, { boss = r.boss }) end
					J.LoadBosses(inst, function (bosses)
						for _, boss in ipairs(bosses or {}) do
							if boss.key == r.boss then b.text:SetText(boss.name) end
						end
					end)
				else
					b.item = r.item
					b.icon:SetTexture(J.ItemIcon(r.item))
					b.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)
					local name = GetItemInfo(r.item)
					local cr, cg, cb = J.QualityColor(r.quality)
					b.text:SetText(name or ("item " .. r.item))
					b.text:SetTextColor(cr, cg, cb)
					b.sub:SetText(J.Chance(r.chance) .. " - " .. inst.name)
					if not name then J.QueryItem(r.item) end
					b.go = function () J.OpenInstance(inst.id, { boss = r.boss, variant = r.variant, tab = "loot" }) end
					J.LoadBosses(inst, function (bosses)
						for _, boss in ipairs(bosses or {}) do
							if boss.key == r.boss then b.sub:SetText(J.Chance(r.chance) .. " - " .. boss.name .. ", " .. inst.name) end
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
	J.Debounce("search", 0.35, function ()
		if search:GetText() ~= text then return end
		if #text < 3 then showResults({}, text) return end
		J.Search(text, function (list) if search:GetText() == text then showResults(list, text) end end)
	end)
end)
search:SetScript("OnEditFocusGained", function (self) self.hint:Hide() end)
search:SetScript("OnEditFocusLost", function (self)
	setShown(self.hint, (self:GetText() or "") == "")
	J.After(0.2, function () if not search:HasFocus() then results:Hide() end end)
end)
search:SetScript("OnEscapePressed", function (self)
	self:SetText("")
	self:ClearFocus()
	results:Hide()
end)

J.On("SEARCH", function (text)
	search:SetText(text or "")
	search:SetFocus()
end)

-----------------------------------------
-- pages

local pageArea = CreateFrame("Frame", nil, dungeonPanel)
pageArea:SetPoint("TOPLEFT", nav, "BOTTOMLEFT", 0, -6)
pageArea:SetPoint("BOTTOMRIGHT", dungeonPanel, "BOTTOMRIGHT", 0, 0)
J.pageArea = pageArea

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
J.ShowStatus = showStatus

-- What a tab shows while the server hasn't answered, or never will: nil once it has.
function J.ServerStatus()
	if J.serverReady then return nil end
	if J.noServer then
		return "The journal needs the realm's journal module",
			"This realm doesn't answer the Journal yet (mod-forever-journal). Ask the realm's admin, or try again with /journal refresh."
	end
	return "Opening the journal..."
end

local pages = {}
function J.RegisterPage(name, page)
	pages[name] = page
	page:Hide()
end

local function refreshDungeons()
	if not frame:IsShown() or not dungeonPanel:IsShown() then return end
	local title, sub = J.ServerStatus()
	if title then
		for _, p in pairs(pages) do p:Hide() end
		showStatus(title, sub)
		return
	end
	showStatus(nil)
	J.LoadInstances(function (byId)
		if not byId then
			showStatus("The journal couldn't be loaded", "Try again in a moment with /journal refresh.")
			return
		end
		local v = J.view
		if v.page == "instance" and not byId[v.instance] then v.page = "home" end
		for name, p in pairs(pages) do
			if name == v.page then p:Show() ; if p.Refresh then p:Refresh() end else p:Hide() end
		end
		J.UpdateCrumbs()
	end)
end

-----------------------------------------
-- the window's tabs (sections)
--
-- Every part of the journal is a section with a tab along the bottom of the window. Built-in
-- sections come first; other addons add theirs with Journal.RegisterTab:
--
--   Journal.RegisterTab({
--       key = "beasts",            -- unique; Journal.OpenTab("beasts") opens the journal on it
--       name = "Beasts",           -- the tab's text, and the window title while it's shown
--       order = 60,                -- place among the tabs (built-in: 10-50)
--       icon = "Interface\\Icons\\Ability_Hunter_BeastTaming",   -- the window's portrait
--       build = function (panel) end,    -- once, the first time the tab is shown
--       onShow = function (panel) end,   -- every time it's shown (optional)
--       onHide = function (panel) end,   -- (optional)
--   })
--
-- panel fills the window's content area. Journal.API tells registrants which version of this
-- interface they talk to.

Journal.API = 1

local sections, ordered = {}, {}
local tabCount = 0

local function layoutTabs()
	table.sort(ordered, function (a, b)
		if a.order ~= b.order then return a.order < b.order end
		return a.key < b.key
	end)
	local prev
	for _, s in ipairs(ordered) do
		s.tab:ClearAllPoints()
		if prev then
			s.tab:SetPoint("TOPLEFT", prev.tab, "TOPRIGHT", dragon and 1 or -15, 0)
		else
			s.tab:SetPoint("TOPLEFT", frame, "BOTTOMLEFT", 11, dragon and 2 or 4)
		end
		prev = s
	end
end

local function updateTabs()
	for _, s in ipairs(ordered) do
		local selected = s.key == J.view.section
		if selected then PanelTemplates_SelectTab(s.tab) else PanelTemplates_DeselectTab(s.tab) end
	end
end

local function addSection(spec)
	assert(type(spec) == "table" and type(spec.key) == "string" and type(spec.name) == "string",
		"Journal.RegisterTab needs a key and a name")
	if sections[spec.key] then return sections[spec.key].panel end
	local s = {
		key = spec.key, name = spec.name, order = tonumber(spec.order) or 100, icon = spec.icon, build = spec.build,
		onShow = spec.onShow, onHide = spec.onHide, panel = spec.panel,
	}
	if not s.panel then
		s.panel = CreateFrame("Frame", nil, content)
		s.panel:SetAllPoints(content)
	end
	s.panel:Hide()
	tabCount = tabCount + 1
	local tab = CreateFrame("Button", "JournalFrameTab" .. tabCount, frame, "CharacterFrameTabButtonTemplate")
	tab:SetID(tabCount)
	tab:SetText(s.name)
	PanelTemplates_TabResize(tab, 0)
	local _, CP = J.Dragon()
	if CP and CP.ReskinTab then
		pcall(CP.ReskinTab, tab)
		-- Every tab's OnShow runs PanelTemplates_TabResize, which sizes for Blizzard's 20px caps,
		-- not DragonUI's 35px ones: each tab grew by 30px and the strip ran past the window's
		-- edge. DragonUI's resize hook calls this to size and chain a strip that isn't its own.
		tab._duiRelayout = function ()
			for _, o in ipairs(ordered) do pcall(CP.ReskinTab, o.tab) end
			layoutTabs()
		end
	end
	tab:SetScript("OnClick", function ()
		PlaySound("igCharacterInfoTab")
		J.OpenSection(s.key, true)
	end)
	s.tab = tab
	sections[s.key] = s
	table.insert(ordered, s)
	layoutTabs()
	updateTabs()
	return s.panel
end
J.AddSection = addSection

local shownSection

local function showSection()
	local s = sections[J.view.section]
	if not s then
		-- A tab whose addon is gone.
		J.view.section = J.view.raids and "raids" or "dungeons"
		s = sections[J.view.section]
	end
	if shownSection and shownSection ~= s then
		shownSection.panel:Hide()
		if shownSection.onHide then pcall(shownSection.onHide, shownSection.panel) end
	end
	frame.title:SetText(s.name)
	if frame.portrait then J.SetPortrait(frame.portrait, s.icon or "Interface\\Icons\\INV_Misc_Book_09") end
	if s.build then
		local build = s.build
		s.build = nil
		local ok, err = pcall(build, s.panel)
		if not ok then J.Print("the " .. s.name .. " tab failed to load: " .. tostring(err)) end
	end
	local was = shownSection == s and s.panel:IsShown()
	shownSection = s
	s.panel:Show()
	if s.onShow and not was then pcall(s.onShow, s.panel) end
	updateTabs()
end

local function refresh()
	if not frame:IsShown() then return end
	showSection()
	refreshDungeons()
end
J.RefreshPage = refresh

-- Opens the journal on a section. fromTab: the tab itself was clicked, which takes Dungeons and
-- Raids back to their home page.
function J.OpenSection(key, fromTab)
	local s = sections[key]
	if not s then return false end
	-- Clicking the tab that's already shown runs its onShow again (the Appearances tab reopens
	-- the transmog window that way).
	if fromTab and frame:IsShown() and shownSection == s and s.onShow then pcall(s.onShow, s.panel) end
	local changes = { section = key }
	if key == "dungeons" or key == "raids" then
		changes.raids = key == "raids"
		if fromTab or J.view.raids ~= changes.raids then changes.page = "home" end
	end
	if not frame:IsShown() then
		placeFrame()
		frame:Show()
	end
	J.Show(changes)
	return true
end

function Journal.RegisterTab(spec)
	return addSection(spec)
end

function Journal.OpenTab(key)
	return J.OpenSection(key)
end

function Journal.IsTabShown(key)
	return frame:IsShown() and J.view.section == key
end

addSection({ key = "dungeons", name = "Dungeons", order = 10, icon = "Interface\\Icons\\INV_Misc_Book_09", panel = dungeonPanel })
addSection({ key = "raids", name = "Raids", order = 20, icon = "Interface\\Icons\\INV_Misc_Book_09", panel = dungeonPanel })

J.On("VIEW", refresh)
J.On("READY", function ()
	J.noServer = nil
	refresh()
	J.Fire("SECTION_SERVER")
end)
J.On("NO_SERVER", function ()
	J.noServer = true
	refresh()
	J.Fire("SECTION_SERVER")
end)
J.On("CACHE_RESET", refresh)

frame:SetScript("OnShow", function ()
	PlaySound("igCharacterInfoOpen")
	search:ClearFocus()
	refresh()
end)
frame:SetScript("OnHide", function ()
	PlaySound("igCharacterInfoClose")
	results:Hide()
	search:ClearFocus()
	if shownSection and shownSection.onHide then pcall(shownSection.onHide, shownSection.panel) end
	if shownSection then shownSection.panel:Hide() end
	shownSection = nil
end)

-----------------------------------------
-- opening

-- Opens an instance's page; extra = { boss =, variant =, tab = }.
function J.OpenInstance(id, extra)
	J.LoadInstances(function (byId)
		local inst = byId and byId[id]
		if not inst then return end
		local changes = { section = inst.raid and "raids" or "dungeons", raids = inst.raid, page = "instance",
			instance = id, tab = "overview" }
		-- Show it under the tier of the tab asked for, or the one it was listed under.
		if extra and extra.variant then
			local v = J.Variant(inst, extra.variant)
			if v then changes.tier = v.expansion end
		elseif not inst.tiers[J.view.tier or 0] then
			changes.tier = inst.expansion
			for _, v in ipairs(inst.variants) do
				if not J.IsLocked(v) then changes.tier = v.expansion break end
			end
		end
		if extra then for k, val in pairs(extra) do changes[k] = val end end
		if not frame:IsShown() then
			placeFrame()
			frame:Show()
		end
		J.Show(changes)
	end)
end

local openedHere = false
function J.Open()
	if not frame:IsShown() then
		placeFrame()
		frame:Show()
	end
	-- Inside an instance the journal opens on it, like retail's.
	if IsInInstance() and J.serverReady then
		J.Here(function (id, variant)
			if id then
				if J.view.page == "instance" and J.view.instance == id and openedHere then return end
				openedHere = true
				J.OpenInstance(id, { variant = variant })
			end
		end)
	end
end

function J.Toggle()
	if frame:IsShown() then frame:Hide() else J.Open() end
end

-- Opens on a section, or closes the journal when it's already showing it (/mounts twice).
function J.ToggleSection(key)
	if frame:IsShown() and J.view.section == key then frame:Hide() return end
	J.OpenSection(key)
end

J.On("ZONE", function () openedHere = false end)

J.On("LOGIN", function ()
	placeFrame()
	local saved = JournalCharDB and JournalCharDB.view
	if saved then for k, val in pairs(saved) do J.view[k] = val end end
	J.view.section = J.view.section or (J.view.raids and "raids" or "dungeons")
end)
