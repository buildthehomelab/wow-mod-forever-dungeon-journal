-- An instance's page: on the left its art, name and levels over the boss list (split into the
-- Dungeon Finder's wings, rares last, a check on bosses killed this lockout); on the right the
-- tabs (Overview, Abilities, Loot, Quests) and the difficulty menu. The tabs' contents live in
-- Overview.lua, Abilities.lua, Loot.lua and Quests.lua.

local DJ = DungeonJournal
local setShown = DJ.SetShown

local LEFT_WIDTH = 286
local HEADER_HEIGHT = 112
local BOSS_ROW = 30
local WING_ROW = 22

local page = CreateFrame("Frame", nil, DJ.pageArea)
page:SetAllPoints(DJ.pageArea)

-----------------------------------------
-- left: header and boss list

local left = DJ.CreateInset(page)
left:SetPoint("TOPLEFT", page, "TOPLEFT", 0, 0)
left:SetPoint("BOTTOMLEFT", page, "BOTTOMLEFT", 0, 0)
left:SetWidth(LEFT_WIDTH)

local header = CreateFrame("Button", nil, left)
header:SetPoint("TOPLEFT", left, "TOPLEFT", 5, -5)
header:SetPoint("TOPRIGHT", left, "TOPRIGHT", -5, -5)
header:SetHeight(HEADER_HEIGHT)
header.art = header:CreateTexture(nil, "BACKGROUND")
header.art:SetAllPoints(header)
header.shade = header:CreateTexture(nil, "BORDER")
header.shade:SetPoint("BOTTOMLEFT", header, "BOTTOMLEFT")
header.shade:SetPoint("BOTTOMRIGHT", header, "BOTTOMRIGHT")
header.shade:SetHeight(58)
header.shade:SetTexture(0, 0, 0, 0.72)
header.name = header:CreateFontString(nil, "OVERLAY", "GameFontNormalHuge")
header.name:SetPoint("BOTTOMLEFT", header, "BOTTOMLEFT", 10, 28)
header.name:SetPoint("RIGHT", header, "RIGHT", -8, 0)
header.name:SetJustifyH("LEFT")
header.name:SetShadowOffset(1, -1)
header.sub = header:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
header.sub:SetPoint("TOPLEFT", header.name, "BOTTOMLEFT", 0, -4)
header.sub:SetPoint("RIGHT", header, "RIGHT", -8, 0)
header.sub:SetJustifyH("LEFT")
header.lock = header:CreateTexture(nil, "OVERLAY")
header.lock:SetTexture("Interface\\LFGFrame\\UI-LFG-ICON-LOCK")
header.lock:SetSize(26, 26)
header.lock:SetPoint("TOPRIGHT", header, "TOPRIGHT", -6, -6)
header:SetScript("OnClick", function () DJ.Show({ boss = false, tab = "overview" }) end)
do
	local edge = CreateFrame("Frame", nil, header)
	edge:SetPoint("TOPLEFT", header, "TOPLEFT", -2, 2)
	edge:SetPoint("BOTTOMRIGHT", header, "BOTTOMRIGHT", 2, -2)
	edge:SetBackdrop({ edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border", edgeSize = 12 })
	edge:SetBackdropBorderColor(0.7, 0.6, 0.35, 1)
	edge:EnableMouse(false)
end

local list = DJ.CreateScrollArea(left)
list:SetPoint("TOPLEFT", header, "BOTTOMLEFT", 0, -6)
list:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", -26, 6)
list:SetScript("OnMouseWheel", function (self, delta)
	local max = math.max(0, self.content:GetHeight() - self:GetHeight())
	self:SetVerticalScroll(math.max(0, math.min(max, self:GetVerticalScroll() - delta * BOSS_ROW * 2)))
end)

local dragon = DJ.dragon

local function makeWingRow()
	local r = CreateFrame("Frame", nil, list.content)
	r:SetHeight(WING_ROW)
	r.band = r:CreateTexture(nil, "BACKGROUND")
	r.band:SetAllPoints(r)
	r.band:SetTexture(0.2, 0.14, 0.04, dragon and 0.9 or 0.55)
	r.rule = r:CreateTexture(nil, "BORDER")
	r.rule:SetHeight(1)
	r.rule:SetPoint("BOTTOMLEFT", r, "BOTTOMLEFT")
	r.rule:SetPoint("BOTTOMRIGHT", r, "BOTTOMRIGHT")
	r.rule:SetTexture(1, 0.82, 0, 0.55)
	r.text = r:CreateFontString(nil, "OVERLAY", "GameFontNormal")
	r.text:SetPoint("LEFT", r, "LEFT", 6, 0)
	r.extra = r:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
	r.extra:SetPoint("RIGHT", r, "RIGHT", -6, 0)
	return r
end

local function makeBossRow()
	local b = CreateFrame("Button", nil, list.content)
	b:SetHeight(BOSS_ROW)
	b.badge = b:CreateTexture(nil, "ARTWORK")
	b.badge:SetSize(22, 22)
	b.badge:SetPoint("LEFT", b, "LEFT", 4, 0)
	b.num = b:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
	b.num:SetPoint("CENTER", b.badge, "CENTER", 0, 0)
	b.check = b:CreateTexture(nil, "OVERLAY")
	b.check:SetTexture("Interface\\RaidFrame\\ReadyCheck-Ready")
	b.check:SetSize(16, 16)
	b.check:SetPoint("RIGHT", b, "RIGHT", -6, 0)
	b.text = b:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	b.text:SetPoint("LEFT", b.badge, "RIGHT", 6, 0)
	b.text:SetPoint("RIGHT", b.check, "LEFT", -4, 0)
	b.text:SetJustifyH("LEFT")
	b.selected = b:CreateTexture(nil, "BORDER")
	b.selected:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
	b.selected:SetBlendMode("ADD")
	b.selected:SetVertexColor(1, 0.82, 0, 0.8)
	b.selected:SetAllPoints(b)
	local hl = b:CreateTexture(nil, "HIGHLIGHT")
	hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
	hl:SetBlendMode("ADD")
	hl:SetVertexColor(1, 1, 1, 0.35)
	hl:SetAllPoints(b)
	b:SetScript("OnClick", function (self)
		local tab = DJ.view.tab
		if tab == "quests" or tab == "overview" or not DJ.view.boss then tab = "overview" end
		DJ.Show({ boss = self.boss.key, tab = tab })
	end)
	return b
end

local wingRows = DJ.CreatePool(makeWingRow)
local bossRows = DJ.CreatePool(makeBossRow)

-----------------------------------------
-- right: tabs, difficulty menu and the panes

local right = CreateFrame("Frame", nil, page)
right:SetPoint("TOPLEFT", left, "TOPRIGHT", 8, 0)
right:SetPoint("BOTTOMRIGHT", page, "BOTTOMRIGHT", 0, 0)

local tabs = {}
local TAB_ORDER = { "overview", "abilities", "loot", "quests" }
local TAB_TEXT = { overview = "Overview", abilities = "Abilities", loot = "Loot", quests = "Quests" }
for i, key in ipairs(TAB_ORDER) do
	local tab = DJ.CreateTab(right, TAB_TEXT[key], function () DJ.Show({ tab = key }) end)
	if i == 1 then
		tab:SetPoint("TOPLEFT", right, "TOPLEFT", 2, -2)
	else
		tab:SetPoint("LEFT", tabs[TAB_ORDER[i - 1]], "RIGHT", dragon and 4 or -14, 0)
	end
	tabs[key] = tab
end

local variantMenu = DJ.CreateDropdown(right, 150, function ()
	local items = {}
	local inst = DJ.Instance(DJ.view.instance)
	if not inst then return items end
	for _, v in ipairs(DJ.VariantsFor(inst, DJ.view.tier)) do
		local locked = DJ.IsLocked(v)
		table.insert(items, {
			text = v.label .. (locked and " |cffff5555(locked)|r" or ""),
			checked = DJ.view.variant == v.id,
			func = function ()
				DungeonJournalCharDB.lastVariant = DungeonJournalCharDB.lastVariant or {}
				DungeonJournalCharDB.lastVariant[inst.raid and "raid" or "dungeon"] = v.difficulty
				DJ.Show({ variant = v.id })
			end,
		})
	end
	return items
end)
variantMenu:SetPoint("TOPRIGHT", right, "TOPRIGHT", -2, -2)

local paneArea = DJ.CreateInset(right)
paneArea:SetPoint("TOPLEFT", right, "TOPLEFT", 0, -30)
paneArea:SetPoint("BOTTOMRIGHT", right, "BOTTOMRIGHT", 0, 0)
DJ.paneArea = paneArea

local panes = {}
function DJ.RegisterPane(key, pane)
	pane:SetParent(paneArea)
	pane:ClearAllPoints()
	pane:SetPoint("TOPLEFT", paneArea, "TOPLEFT", 6, -6)
	pane:SetPoint("BOTTOMRIGHT", paneArea, "BOTTOMRIGHT", -6, 6)
	pane:Hide()
	panes[key] = pane
end

-----------------------------------------
-- refresh

local function pickVariant(inst)
	local v = DJ.view
	local options = DJ.VariantsFor(inst, v.tier)
	if #options == 0 then options = inst.variants end
	for _, o in ipairs(options) do
		if o.id == v.variant then return o end
	end
	-- The difficulty picked last time for this kind of instance, if open; else the first open one.
	local last = DungeonJournalCharDB.lastVariant and DungeonJournalCharDB.lastVariant[inst.raid and "raid" or "dungeon"]
	if last then
		for _, o in ipairs(options) do
			if o.difficulty == last and not DJ.IsLocked(o) then return o end
		end
	end
	for _, o in ipairs(options) do
		if not DJ.IsLocked(o) then return o end
	end
	return options[1]
end

local function setHeaderArt(inst)
	local ok = inst.texture and header.art:SetTexture("Interface\\LFGFrame\\UI-LFG-BACKGROUND-" .. inst.texture)
	if ok then
		header.art:SetTexCoord(0.03, 0.62, 0.05, 0.55)
		header.art:SetVertexColor(1, 1, 1)
	else
		header.art:SetTexture("Interface\\LFGFrame\\UI-LFG-BACKGROUND-QUESTPAPER")
		header.art:SetTexCoord(0, 0.6, 0, 0.5)
		header.art:SetVertexColor(0.5, 0.45, 0.4)
	end
end

local function layoutBosses(inst, bosses, variant, st)
	wingRows:Reset()
	bossRows:Reset()
	local y = 0
	local width = math.max(100, list:GetWidth() - 4)
	local currentWing
	local number = 0
	local killedCount, total = 0, 0
	for _, boss in ipairs(bosses) do
		if DJ.BossOnVariant(boss, variant) then
			local wingKey = boss.rare and "rares" or boss.wing
			if wingKey ~= currentWing and (boss.rare or #inst.wings > 0) then
				currentWing = wingKey
				local w = wingRows:Get()
				w:ClearAllPoints()
				w:SetPoint("TOPLEFT", list.content, "TOPLEFT", 0, -y)
				w:SetWidth(width)
				if boss.rare then
					w.text:SetText("Rares")
					w.extra:SetText("")
				else
					local wing = inst.wings[boss.wing + 1]
					w.text:SetText(wing and wing.name or "")
					w.extra:SetText(wing and (wing.minLevel .. "-" .. wing.maxLevel) or "")
				end
				y = y + WING_ROW + 2
			end
			local b = bossRows:Get()
			b.boss = boss
			b:ClearAllPoints()
			b:SetPoint("TOPLEFT", list.content, "TOPLEFT", 0, -y)
			b:SetWidth(width)
			if boss.rare then
				b.badge:SetTexture("Interface\\TargetingFrame\\UI-TargetingFrame-Skull")
				b.badge:SetVertexColor(0.75, 0.75, 1)
				b.num:SetText("")
			else
				number = number + 1
				b.badge:SetTexture("Interface\\Minimap\\UI-Minimap-Background")
				b.badge:SetVertexColor(0.25, 0.18, 0.05)
				b.num:SetText(number)
				total = total + 1
			end
			b.text:SetText(boss.name)
			local killed = DJ.Killed(st, variant, boss)
			if killed then killedCount = killedCount + 1 end
			setShown(b.check, killed)
			if killed then b.text:SetTextColor(0.6, 0.6, 0.6) else b.text:SetTextColor(1, 1, 1) end
			setShown(b.selected, DJ.view.boss == boss.key)
			y = y + BOSS_ROW
		end
	end
	list:SetContentHeight(y)
	return killedCount, total
end

local ctx = {}
DJ.ctx = ctx

local function refreshPane()
	local v = DJ.view
	for key, pane in pairs(panes) do
		if key == v.tab then pane:Show() ; if pane.Refresh then pane:Refresh(ctx) end else pane:Hide() end
	end
end

function page:Refresh()
	local v = DJ.view
	local inst = DJ.Instance(v.instance)
	if not inst then return end

	local variant = pickVariant(inst)
	if not variant then return end
	v.variant = variant.id
	variantMenu:SetText(variant.label)
	setShown(variantMenu, #DJ.VariantsFor(inst, v.tier) > 0)

	setHeaderArt(inst)
	header.name:SetText(inst.name)
	local players = variant.players and variant.players > 0 and (variant.players .. " players") or nil
	local levels = inst.minLevel == inst.maxLevel and ("Level " .. inst.minLevel) or ("Level " .. inst.minLevel .. "-" .. inst.maxLevel)
	local parts = { levels }
	if players then table.insert(parts, players) end
	if inst.zone ~= "" then table.insert(parts, inst.zone) end
	header.sub:SetText(table.concat(parts, "  -  "))
	local locked = DJ.IsLocked(variant)
	setShown(header.lock, locked)
	header.art:SetDesaturated(locked)

	-- Tabs: Abilities only with a boss picked.
	if not v.boss and v.tab == "abilities" then v.tab = "overview" end
	for key, tab in pairs(tabs) do
		tab:SetSelected(key == v.tab)
		if key == "abilities" then
			if v.boss then tab:Enable() else tab:Disable() end
		end
	end

	ctx.inst, ctx.variant, ctx.locked = inst, variant, locked
	DJ.LoadBosses(inst, function (bosses)
		if DJ.view.instance ~= inst.id then return end
		bosses = bosses or {}
		DJ.currentBosses = bosses
		ctx.bosses = bosses
		ctx.boss = nil
		for _, b in ipairs(bosses) do
			if b.key == v.boss then ctx.boss = b end
		end
		if v.boss and not ctx.boss then v.boss = nil end
		DJ.UpdateCrumbs()
		layoutBosses(inst, bosses, variant, ctx.status and ctx.status.inst == inst.id and ctx.status.value or nil)
		refreshPane()
		DJ.LoadStatus(inst, function (st)
			if DJ.view.instance ~= inst.id or not st then return end
			ctx.status = { inst = inst.id, value = st }
			ctx.killed, ctx.total = layoutBosses(inst, bosses, variant, st)
			DJ.Fire("STATUS", st)
		end)
	end)
end

DJ.On("STATUS_STALE", function () if page:IsShown() then page:Refresh() end end)
DJ.On("ITEM_INFO", function () if page:IsShown() then refreshPane() end end)

DJ.RegisterPage("instance", page)
