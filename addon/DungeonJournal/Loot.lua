-- The Loot tab: what the boss (or, with no boss picked, every boss) drops on the chosen
-- difficulty, with the chance one kill drops it. A class menu (your class first, as retail does)
-- and a slot menu narrow it down. Hover for the item's tooltip (Shift compares with what you
-- wear), Shift-click to link it, Ctrl-click to try it on.

local DJ = DungeonJournal
local setShown = DJ.SetShown
local LOOT = DJ.LOOT

local ROW_HEIGHT = 40

local pane = CreateFrame("Frame")

-----------------------------------------
-- who can use what

local CLASSES = { "WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST", "DEATHKNIGHT", "SHAMAN", "MAGE", "WARLOCK", "DRUID" }

-- Armor (class 4) subclasses: 1 cloth, 2 leather, 3 mail, 4 plate, 6 shield, 7 libram, 8 idol,
-- 9 totem, 10 sigil. The type each class wants, before and from level 40.
local ARMOR = {
	WARRIOR = { early = 3, late = 4, shield = true }, PALADIN = { early = 3, late = 4, shield = true, relic = 7 },
	HUNTER = { early = 2, late = 3 }, ROGUE = { early = 2, late = 2 }, PRIEST = { early = 1, late = 1 },
	DEATHKNIGHT = { early = 4, late = 4, relic = 10 }, SHAMAN = { early = 2, late = 3, shield = true, relic = 9 },
	MAGE = { early = 1, late = 1 }, WARLOCK = { early = 1, late = 1 }, DRUID = { early = 2, late = 2, relic = 8 },
}

-- Weapon (class 2) subclasses each class can use.
local WEAPONS = {
	WARRIOR = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 13, 15, 16, 18 },
	PALADIN = { 0, 1, 4, 5, 6, 7, 8 },
	HUNTER = { 0, 1, 2, 3, 6, 7, 8, 10, 13, 15, 16, 18 },
	ROGUE = { 0, 2, 3, 4, 7, 13, 15, 16, 18 },
	PRIEST = { 4, 10, 15, 19 },
	DEATHKNIGHT = { 0, 1, 4, 5, 6, 7, 8 },
	SHAMAN = { 0, 1, 4, 5, 10, 13, 15 },
	MAGE = { 7, 10, 15, 19 },
	WARLOCK = { 7, 10, 15, 19 },
	DRUID = { 4, 5, 6, 10, 13, 15 },
}
local weaponSets = {}
for class, list in pairs(WEAPONS) do
	weaponSets[class] = {}
	for _, s in ipairs(list) do weaponSets[class][s] = true end
end

local INVTYPE_CLOAK = 16

local function usableBy(item, class)
	if not class then return true end
	if item.class == 2 then
		return weaponSets[class][item.subclass] or false
	elseif item.class == 4 then
		local a = ARMOR[class]
		local sub = item.subclass
		if sub == 0 or item.invType == INVTYPE_CLOAK then return true end
		if sub >= 1 and sub <= 4 then
			local want = (item.reqLevel or 0) >= 40 and a.late or a.early
			return sub == want
		end
		if sub == 6 then return a.shield or false end
		if sub >= 7 and sub <= 10 then return a.relic == sub end
		return true
	end
	return true
end

-- Slot groups for the slot menu, by inventory type.
local SLOTS = {
	{ key = "head", text = INVTYPE_HEAD or "Head", types = { 1 } },
	{ key = "neck", text = INVTYPE_NECK or "Neck", types = { 2 } },
	{ key = "shoulder", text = INVTYPE_SHOULDER or "Shoulder", types = { 3 } },
	{ key = "back", text = INVTYPE_CLOAK or "Back", types = { 16 } },
	{ key = "chest", text = INVTYPE_CHEST or "Chest", types = { 5, 20 } },
	{ key = "wrist", text = INVTYPE_WRIST or "Wrist", types = { 9 } },
	{ key = "hands", text = INVTYPE_HAND or "Hands", types = { 10 } },
	{ key = "waist", text = INVTYPE_WAIST or "Waist", types = { 6 } },
	{ key = "legs", text = INVTYPE_LEGS or "Legs", types = { 7 } },
	{ key = "feet", text = INVTYPE_FEET or "Feet", types = { 8 } },
	{ key = "finger", text = INVTYPE_FINGER or "Finger", types = { 11 } },
	{ key = "trinket", text = INVTYPE_TRINKET or "Trinket", types = { 12 } },
	{ key = "weapon", text = "Weapons", types = { 13, 17, 21 } },
	{ key = "offhand", text = "Off Hand", types = { 14, 22, 23 } },
	{ key = "ranged", text = "Ranged and relics", types = { 15, 25, 26, 28 } },
	{ key = "other", text = "Other", types = { 0, 4, 18, 19, 24, 27 } },
}
local slotOf = {}
for _, s in ipairs(SLOTS) do
	for _, t in ipairs(s.types) do slotOf[t] = s.key end
end

local INVTYPE_NAMES = {
	[1] = INVTYPE_HEAD, [2] = INVTYPE_NECK, [3] = INVTYPE_SHOULDER, [4] = INVTYPE_BODY, [5] = INVTYPE_CHEST,
	[6] = INVTYPE_WAIST, [7] = INVTYPE_LEGS, [8] = INVTYPE_FEET, [9] = INVTYPE_WRIST, [10] = INVTYPE_HAND,
	[11] = INVTYPE_FINGER, [12] = INVTYPE_TRINKET, [13] = INVTYPE_WEAPON, [14] = INVTYPE_SHIELD, [15] = INVTYPE_RANGED,
	[16] = INVTYPE_CLOAK, [17] = INVTYPE_2HWEAPON, [18] = INVTYPE_BAG, [19] = INVTYPE_TABARD, [20] = INVTYPE_ROBE,
	[21] = INVTYPE_WEAPONMAINHAND, [22] = INVTYPE_WEAPONOFFHAND, [23] = INVTYPE_HOLDABLE, [24] = INVTYPE_AMMO,
	[25] = INVTYPE_THROWN, [26] = INVTYPE_RANGEDRIGHT, [27] = INVTYPE_QUIVER, [28] = INVTYPE_RELIC,
}
local ARMOR_NAMES = { [1] = "Cloth", [2] = "Leather", [3] = "Mail", [4] = "Plate", [6] = "Shield", [7] = "Libram",
	[8] = "Idol", [9] = "Totem", [10] = "Sigil" }
local WEAPON_NAMES = { [0] = "Axe", [1] = "Axe", [2] = "Bow", [3] = "Gun", [4] = "Mace", [5] = "Mace", [6] = "Polearm",
	[7] = "Sword", [8] = "Sword", [10] = "Staff", [13] = "Fist Weapon", [14] = "Miscellaneous", [15] = "Dagger",
	[16] = "Thrown", [18] = "Crossbow", [19] = "Wand", [20] = "Fishing Pole" }
local CLASS_NAMES = { [0] = "Consumable", [1] = "Container", [3] = "Gem", [5] = "Reagent", [6] = "Projectile",
	[7] = "Trade Goods", [9] = "Recipe", [11] = "Quiver", [12] = "Quest Item", [13] = "Key", [15] = "Miscellaneous", [16] = "Glyph" }

local function typeText(item)
	local slot = INVTYPE_NAMES[item.invType]
	local _, _, _, _, _, _, subType = GetItemInfo(item.item)
	if item.class == 4 then
		local kind = subType or ARMOR_NAMES[item.subclass]
		if item.subclass == 0 or item.invType == INVTYPE_CLOAK or not kind or kind == "Miscellaneous" then return slot or "" end
		if item.subclass == 6 or item.subclass >= 7 then return kind end
		return (slot and (slot .. ", ") or "") .. kind
	elseif item.class == 2 then
		return (slot and (slot .. ", ") or "") .. (subType or WEAPON_NAMES[item.subclass] or "")
	end
	return subType or CLASS_NAMES[item.class] or ""
end

-----------------------------------------
-- filters

local _, playerClass = UnitClass("player")

local function filterState()
	local db = DungeonJournalCharDB
	if db.lootClass == nil then db.lootClass = playerClass or false end
	return db
end

local classMenu = DJ.CreateDropdown(pane, 150, function ()
	local db = filterState()
	local items = {
		{ text = "All classes", checked = db.lootClass == false, func = function () db.lootClass = false ; pane:Refresh(DJ.ctx) end },
	}
	for _, class in ipairs(CLASSES) do
		local name = (LOCALIZED_CLASS_NAMES_MALE and LOCALIZED_CLASS_NAMES_MALE[class]) or class
		local color = RAID_CLASS_COLORS and RAID_CLASS_COLORS[class]
		if color then name = DJ.Colored(name, color.r, color.g, color.b) end
		table.insert(items, { text = name, checked = db.lootClass == class,
			func = function () db.lootClass = class ; pane:Refresh(DJ.ctx) end })
	end
	return items
end)
classMenu:SetPoint("TOPLEFT", pane, "TOPLEFT", 0, 0)

local slotMenu = DJ.CreateDropdown(pane, 150, function ()
	local db = filterState()
	local items = { { text = "All slots", checked = not db.lootSlot, func = function () db.lootSlot = nil ; pane:Refresh(DJ.ctx) end } }
	for _, s in ipairs(SLOTS) do
		table.insert(items, { text = s.text, checked = db.lootSlot == s.key,
			func = function () db.lootSlot = s.key ; pane:Refresh(DJ.ctx) end })
	end
	return items
end)
slotMenu:SetPoint("LEFT", classMenu, "RIGHT", 6, 0)

local countText = pane:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
countText:SetPoint("LEFT", slotMenu, "RIGHT", 10, 0)
countText:SetPoint("RIGHT", pane, "RIGHT", -4, 0)
countText:SetJustifyH("RIGHT")

local scroll = DJ.CreateScrollArea(pane)
scroll:SetPoint("TOPLEFT", pane, "TOPLEFT", 0, -30)
scroll:SetPoint("BOTTOMRIGHT", pane, "BOTTOMRIGHT", -22, 0)
scroll:SetScript("OnMouseWheel", function (self, delta)
	local max = math.max(0, self.content:GetHeight() - self:GetHeight())
	self:SetVerticalScroll(math.max(0, math.min(max, self:GetVerticalScroll() - delta * ROW_HEIGHT * 2)))
end)
local c = scroll.content

local empty = pane:CreateFontString(nil, "OVERLAY", "GameFontDisable")
empty:SetPoint("TOP", pane, "TOP", 0, -80)
empty:SetWidth(380)

-----------------------------------------
-- rows

local function makeRow()
	local r = CreateFrame("Button", nil, c)
	r:SetHeight(ROW_HEIGHT)
	r:RegisterForClicks("LeftButtonUp", "RightButtonUp")
	r.item = DJ.CreateItemButton(r, 34)
	r.item:SetPoint("LEFT", r, "LEFT", 3, 0)
	r.item:EnableMouse(false)
	r.name = r:CreateFontString(nil, "OVERLAY", "GameFontNormal")
	r.name:SetPoint("TOPLEFT", r.item, "TOPRIGHT", 8, -2)
	r.name:SetJustifyH("LEFT")
	r.type = r:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	r.type:SetPoint("TOPLEFT", r.name, "BOTTOMLEFT", 0, -3)
	r.type:SetJustifyH("LEFT")
	r.type:SetTextColor(0.75, 0.75, 0.75)
	r.chance = r:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	r.chance:SetPoint("TOPRIGHT", r, "TOPRIGHT", -6, -5)
	r.chance:SetJustifyH("RIGHT")
	r.tags = r:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	r.tags:SetPoint("TOPRIGHT", r.chance, "BOTTOMRIGHT", 0, -3)
	r.tags:SetJustifyH("RIGHT")
	local hl = r:CreateTexture(nil, "HIGHLIGHT")
	hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
	hl:SetBlendMode("ADD")
	hl:SetVertexColor(1, 1, 1, 0.2)
	hl:SetAllPoints(r)
	r:SetScript("OnEnter", function (self)
		if not self.data then return end
		DJ.ShowItemTooltip(self, self.data.item)
		local d = self.data
		GameTooltip:AddLine(" ")
		GameTooltip:AddDoubleLine("Drop chance", DJ.Chance(d.chance), 1, 0.82, 0, 1, 1, 1)
		if d.maxCount and d.maxCount > 1 then
			GameTooltip:AddDoubleLine("Amount", d.minCount == d.maxCount and d.minCount or (d.minCount .. "-" .. d.maxCount), 1, 0.82, 0, 1, 1, 1)
		end
		if bit.band(d.flags, LOOT.QUEST) ~= 0 then GameTooltip:AddLine("Only drops while you're on the quest that needs it.", 0.6, 0.8, 1, true) end
		if bit.band(d.flags, LOOT.HARD_MODE) ~= 0 then GameTooltip:AddLine("Hard mode only.", 1, 0.5, 0.2) end
		if bit.band(d.flags, LOOT.CHEST) ~= 0 then GameTooltip:AddLine("Found in the encounter's chest.", 0.8, 0.8, 0.8) end
		if bit.band(d.flags, LOOT.CONDITION) ~= 0 then GameTooltip:AddLine("Has extra conditions (a quest, reputation or event).", 0.8, 0.8, 0.8, true) end
		if bit.band(d.flags, LOOT.SHARED) ~= 0 then GameTooltip:AddLine("A world drop: many creatures can drop it.", 0.8, 0.8, 0.8, true) end
		if self.bossName then GameTooltip:AddLine("Dropped by " .. self.bossName, 0.8, 0.8, 0.8) end
		GameTooltip:Show()
	end)
	r:SetScript("OnLeave", function () GameTooltip:Hide() end)
	r:SetScript("OnClick", function (self)
		if self.data then DJ.HandleItemClick(self.data.item, self.data.name, self.data.quality) end
	end)
	-- Shift pressed or released while hovering: compare or stop comparing.
	r:SetScript("OnUpdate", function (self)
		if not self.data or not GameTooltip:IsOwned(self) then return end
		local shift = IsShiftKeyDown() and true or false
		if shift ~= self.shift then
			self.shift = shift
			self:GetScript("OnEnter")(self)
		end
	end)
	return r
end

local rows = DJ.CreatePool(makeRow)

local function tagsText(d)
	local parts = {}
	if bit.band(d.flags, LOOT.HARD_MODE) ~= 0 then table.insert(parts, "|cffff8020Hard mode|r") end
	if bit.band(d.flags, LOOT.QUEST) ~= 0 then table.insert(parts, "|cff80c0ffQuest|r") end
	if bit.band(d.flags, LOOT.CHEST) ~= 0 then table.insert(parts, "|cffccccccChest|r") end
	return table.concat(parts, "  ")
end

local serial = 0

function pane:Refresh(ctx)
	serial = serial + 1
	local mine = serial
	local db = filterState()
	local classLabel = "All classes"
	if db.lootClass then classLabel = (LOCALIZED_CLASS_NAMES_MALE and LOCALIZED_CLASS_NAMES_MALE[db.lootClass]) or db.lootClass end
	classMenu:SetText(classLabel)
	local slotLabel = "All slots"
	for _, s in ipairs(SLOTS) do if s.key == db.lootSlot then slotLabel = s.text end end
	slotMenu:SetText(slotLabel)

	local bossKey = ctx.boss and ctx.boss.key or "all"
	local listKey = ctx.inst.id .. ":" .. bossKey .. ":" .. ctx.variant.id
	if pane.lastKey ~= listKey then
		pane.lastKey = listKey
		scroll:ScrollToTop()
	end

	DJ.LoadLoot(ctx.inst, bossKey, ctx.variant, function (items)
		if mine ~= serial then return end
		rows:Reset()
		local every = bossKey == "all"
		local bossNames = {}
		for _, b in ipairs(ctx.bosses or {}) do bossNames[b.key] = b.name end

		local shown = {}
		for _, d in ipairs(items or {}) do
			local slotOk = not db.lootSlot or slotOf[d.invType] == db.lootSlot
			if slotOk and usableBy(d, db.lootClass or nil) then table.insert(shown, d) end
		end
		-- Every boss: in boss order, best chance first within a boss.
		if every then
			local order = {}
			for i, b in ipairs(ctx.bosses or {}) do order[b.key] = i end
			table.sort(shown, function (a, b)
				local oa, ob = order[a.boss] or 999, order[b.boss] or 999
				if oa ~= ob then return oa < ob end
				if a.chance ~= b.chance then return a.chance > b.chance end
				return a.item < b.item
			end)
		end

		local width = math.max(200, scroll:GetWidth() - 8)
		local y = 0
		for _, d in ipairs(shown) do
			local r = rows:Get()
			r.data = d
			r.bossName = every and bossNames[d.boss] or nil
			r:ClearAllPoints()
			r:SetPoint("TOPLEFT", c, "TOPLEFT", 0, -y)
			r:SetWidth(width)
			local name = GetItemInfo(d.item) or d.name
			if not GetItemInfo(d.item) then DJ.QueryItem(d.item) end
			local cr, cg, cb = DJ.QualityColor(d.quality)
			r.item:SetItem(DJ.ItemIcon(d.item), d.quality, d.maxCount and d.maxCount > 1 and d.maxCount or nil)
			r.name:SetText(name)
			r.name:SetTextColor(cr, cg, cb)
			r.name:SetWidth(width - 150)
			local kind = typeText(d)
			if r.bossName then kind = (kind ~= "" and (kind .. "  -  ") or "") .. "|cffffd200" .. r.bossName .. "|r" end
			r.type:SetText(kind)
			r.type:SetWidth(width - 120)
			r.chance:SetText(DJ.Chance(d.chance))
			r.tags:SetText(tagsText(d))
			y = y + ROW_HEIGHT + 2
		end
		scroll:SetContentHeight(y)

		local total = items and #items or 0
		if total > #shown then
			countText:SetText(#shown .. " of " .. total .. " items")
		else
			countText:SetText(total == 1 and "1 item" or (total .. " items"))
		end
		if not items then
			empty:SetText("The loot couldn't be loaded.")
			empty:Show()
		elseif #shown == 0 then
			empty:SetText(total > 0 and "Nothing for this class or slot. Pick All classes or All slots to see everything."
				or "This boss drops nothing the journal lists.")
			empty:Show()
		else
			empty:Hide()
		end
	end)
end

DJ.RegisterPane("loot", pane)
