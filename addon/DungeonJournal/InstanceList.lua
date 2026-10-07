-- The journal's home page, as retail's: a tier menu, Dungeons and Raids tabs, and a tile per
-- instance with its Dungeon Finder art, name and level range. Instances the character's era
-- hasn't opened (mod-individual-progression) carry a lock.

local DJ = DungeonJournal
local setShown = DJ.SetShown

local COLUMNS = 4
local TILE_HEIGHT = 104
local GAP = 8

local page = CreateFrame("Frame", nil, DJ.pageArea)
page:SetAllPoints(DJ.pageArea)

local tierMenu = DJ.CreateDropdown(page, 190, function ()
	local items = {}
	for tier = 0, 2 do
		table.insert(items, { text = DJ.EXPANSIONS[tier], checked = DJ.view.tier == tier,
			func = function () DJ.Show({ tier = tier }) end })
	end
	return items
end)
tierMenu:SetPoint("TOPLEFT", page, "TOPLEFT", 4, -2)

local dungeonsTab, raidsTab
dungeonsTab = DJ.CreateTab(page, "Dungeons", function () DJ.Show({ raids = false }) end)
raidsTab = DJ.CreateTab(page, "Raids", function () DJ.Show({ raids = true }) end)
raidsTab:SetPoint("TOPRIGHT", page, "TOPRIGHT", -4, -2)
dungeonsTab:SetPoint("RIGHT", raidsTab, "LEFT", -4, 0)

local eraNote = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
eraNote:SetPoint("LEFT", tierMenu, "RIGHT", 12, 0)
eraNote:SetPoint("RIGHT", dungeonsTab, "LEFT", -12, 0)
eraNote:SetJustifyH("LEFT")

local inset = DJ.CreateInset(page)
inset:SetPoint("TOPLEFT", page, "TOPLEFT", 0, -30)
inset:SetPoint("BOTTOMRIGHT", page, "BOTTOMRIGHT", 0, 0)

local scroll = DJ.CreateScrollArea(inset)
scroll:SetPoint("TOPLEFT", inset, "TOPLEFT", 8, -8)
scroll:SetPoint("BOTTOMRIGHT", inset, "BOTTOMRIGHT", -28, 8)
scroll:SetScript("OnMouseWheel", function (self, delta)
	local max = math.max(0, self.content:GetHeight() - self:GetHeight())
	self:SetVerticalScroll(math.max(0, math.min(max, self:GetVerticalScroll() - delta * 60)))
end)

local empty = inset:CreateFontString(nil, "OVERLAY", "GameFontDisableLarge")
empty:SetPoint("CENTER", inset, "CENTER")

local function makeTile()
	local t = CreateFrame("Button", nil, scroll.content)
	t:SetHeight(TILE_HEIGHT)
	t.bg = t:CreateTexture(nil, "BACKGROUND")
	t.bg:SetPoint("TOPLEFT", t, "TOPLEFT", 3, -3)
	t.bg:SetPoint("BOTTOMRIGHT", t, "BOTTOMRIGHT", -3, 3)
	t.shade = t:CreateTexture(nil, "BORDER")
	t.shade:SetPoint("BOTTOMLEFT", t.bg, "BOTTOMLEFT")
	t.shade:SetPoint("BOTTOMRIGHT", t.bg, "BOTTOMRIGHT")
	t.shade:SetHeight(46)
	t.shade:SetTexture(0, 0, 0, 0.7)
	t.icon = t:CreateTexture(nil, "ARTWORK")
	t.icon:SetSize(40, 40)
	t.icon:SetPoint("TOPLEFT", t, "TOPLEFT", 10, -10)
	t.name = t:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	t.name:SetPoint("BOTTOMLEFT", t, "BOTTOMLEFT", 10, 24)
	t.name:SetPoint("RIGHT", t, "RIGHT", -8, 0)
	t.name:SetJustifyH("LEFT")
	t.name:SetShadowOffset(1, -1)
	t.sub = t:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	t.sub:SetPoint("TOPLEFT", t.name, "BOTTOMLEFT", 0, -3)
	t.sub:SetPoint("RIGHT", t, "RIGHT", -8, 0)
	t.sub:SetJustifyH("LEFT")
	t.lock = t:CreateTexture(nil, "OVERLAY")
	t.lock:SetTexture("Interface\\LFGFrame\\UI-LFG-ICON-LOCK")
	t.lock:SetSize(24, 24)
	t.lock:SetPoint("TOPRIGHT", t, "TOPRIGHT", -8, -8)
	t.border = CreateFrame("Frame", nil, t)
	t.border:SetAllPoints(t)
	t.border:SetBackdrop({ edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border", edgeSize = 14 })
	t.border:SetBackdropBorderColor(0.6, 0.5, 0.3, 1)
	t.border:EnableMouse(false)
	local hl = t:CreateTexture(nil, "HIGHLIGHT")
	hl:SetPoint("TOPLEFT", t, "TOPLEFT", 3, -3)
	hl:SetPoint("BOTTOMRIGHT", t, "BOTTOMRIGHT", -3, 3)
	hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
	hl:SetBlendMode("ADD")
	hl:SetVertexColor(1, 0.82, 0, 0.6)
	t:SetScript("OnEnter", function (self)
		self.border:SetBackdropBorderColor(1, 0.82, 0, 1)
		local inst = self.inst
		if not inst then return end
		GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
		GameTooltip:SetText(inst.name)
		GameTooltip:AddLine((inst.raid and "Raid" or "Dungeon") .. ", level " .. inst.minLevel .. "-" .. inst.maxLevel, 1, 1, 1)
		for _, v in ipairs(DJ.VariantsFor(inst, DJ.view.tier)) do
			local locked = DJ.IsLocked(v)
			GameTooltip:AddDoubleLine(v.label, locked and ("Progression stage " .. v.reqState) or (v.players .. " players"),
				1, 0.82, 0, locked and 1 or 0.8, locked and 0.3 or 0.8, locked and 0.3 or 0.8)
		end
		if #inst.wings > 0 then
			local names = {}
			for _, w in ipairs(inst.wings) do table.insert(names, w.name) end
			GameTooltip:AddLine("Wings: " .. table.concat(names, ", "), 0.8, 0.8, 0.8, true)
		end
		if inst.zone ~= "" then GameTooltip:AddLine("Entrance: " .. inst.zone, 0.8, 0.8, 0.8) end
		GameTooltip:Show()
	end)
	t:SetScript("OnLeave", function (self)
		self.border:SetBackdropBorderColor(0.6, 0.5, 0.3, 1)
		GameTooltip:Hide()
	end)
	t:SetScript("OnClick", function (self)
		if self.inst then DJ.OpenInstance(self.inst.id) end
	end)
	return t
end

local tiles = DJ.CreatePool(makeTile)

local GENERIC_BG = "Interface\\LFGFrame\\UI-LFG-BACKGROUND-QUESTPAPER"

local function setTileArt(t, inst)
	-- The Dungeon Finder's background art, cropped to the tile; its icon in the corner.
	local ok = inst.texture and t.bg:SetTexture("Interface\\LFGFrame\\UI-LFG-BACKGROUND-" .. inst.texture)
	if ok then
		t.bg:SetTexCoord(0.03, 0.66, 0.05, 0.62)
		t.bg:SetVertexColor(1, 1, 1)
	else
		t.bg:SetTexture(GENERIC_BG)
		t.bg:SetTexCoord(0, 0.6, 0, 0.6)
		t.bg:SetVertexColor(0.5, 0.45, 0.4)
	end
	local iconOk = inst.texture and t.icon:SetTexture("Interface\\LFGFrame\\LFGIcon-" .. inst.texture)
	if iconOk then
		DJ.SetPortrait(t.icon, "Interface\\LFGFrame\\LFGIcon-" .. inst.texture)
		t.icon:Show()
	else
		t.icon:Hide()
	end
end

function page:Refresh()
	local v = DJ.view
	local tier = v.tier or 0
	tierMenu:SetText(DJ.EXPANSIONS[tier])
	dungeonsTab:SetSelected(not v.raids)
	raidsTab:SetSelected(v.raids and true or false)

	if DJ.Has(DJ.HELLO_PROGRESSION) and (DJ.state or 18) < 18 then
		eraNote:SetText("|cff9d9d9dYour progression stage: |r" .. (DJ.state or 0))
	else
		eraNote:SetText("")
	end

	DJ.LoadInstances(function (byId, list)
		tiles:Reset()
		if not list then return end
		local shown = {}
		for _, inst in ipairs(list) do
			if inst.tiers[tier] and inst.raid == (v.raids and true or false) then table.insert(shown, inst) end
		end
		table.sort(shown, function (a, b)
			local la, lb = a.targetLevel or a.minLevel, b.targetLevel or b.minLevel
			if a.raid then la, lb = a.minLevel, b.minLevel end
			if la ~= lb then return la < lb end
			return a.name < b.name
		end)

		local width = math.max(200, scroll:GetWidth() - 4)
		local tileWidth = math.floor((width - GAP * (COLUMNS - 1)) / COLUMNS)
		for i, inst in ipairs(shown) do
			local t = tiles:Get()
			t.inst = inst
			local col, row = (i - 1) % COLUMNS, math.floor((i - 1) / COLUMNS)
			t:ClearAllPoints()
			t:SetWidth(tileWidth)
			t:SetPoint("TOPLEFT", scroll.content, "TOPLEFT", col * (tileWidth + GAP), -row * (TILE_HEIGHT + GAP))
			setTileArt(t, inst)
			t.name:SetText(inst.name)
			local levels = inst.minLevel == inst.maxLevel and ("Level " .. inst.minLevel)
				or ("Level " .. inst.minLevel .. "-" .. inst.maxLevel)
			local locked = DJ.InstanceLocked(inst, tier)
			setShown(t.lock, locked)
			if locked then
				t.name:SetTextColor(0.6, 0.6, 0.6)
				local need = 99
				for _, var in ipairs(DJ.VariantsFor(inst, tier)) do need = math.min(need, var.reqState or 0) end
				t.sub:SetText("|cffff5555Progression stage " .. need .. "|r")
				t.bg:SetDesaturated(true)
			else
				t.name:SetTextColor(1, 0.82, 0)
				t.sub:SetText(levels)
				t.bg:SetDesaturated(false)
			end
		end
		local rows = math.ceil(#shown / COLUMNS)
		scroll:SetContentHeight(rows * (TILE_HEIGHT + GAP))
		if #shown == 0 then
			empty:SetText(v.raids and "No raids in this tier" or "No dungeons in this tier")
			empty:Show()
		else
			empty:Hide()
		end
	end)
end

page:SetScript("OnSizeChanged", function () if page:IsShown() then page:Refresh() end end)
DJ.RegisterPage("home", page)
