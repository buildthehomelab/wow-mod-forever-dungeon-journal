-- The Mounts, Pets and Toys tabs, laid out like retail's Collections: a list on the left (search,
-- filters, collected count) and on the right the selected one's model, a Summon or Use button
-- and where it comes from. Data from CollectionData.lua.

local J = Journal
local setShown = J.SetShown
local band = bit.band

local LIST_WIDTH = 300
local ROW_HEIGHT = 40
local ROWS = 11

local SOURCE_ICONS = {
	V = "Interface\\GossipFrame\\VendorGossipIcon",
	D = "Interface\\Icons\\INV_Misc_Bag_07",
	G = "Interface\\Icons\\INV_Box_02",
	P = "Interface\\Icons\\INV_Misc_Bag_11",
	F = "Interface\\Icons\\Trade_Fishing",
	R = "Interface\\Icons\\INV_Misc_Map02",
	Q = "Interface\\GossipFrame\\AvailableQuestIcon",
	A = "Interface\\AchievementFrame\\UI-Achievement-TinyShield",
	T = "Interface\\GossipFrame\\TrainerGossipIcon",
	N = "Interface\\GossipFrame\\GossipGossipIcon",
}
local BOSS_ICON = "Interface\\TargetingFrame\\UI-TargetingFrame-Skull"
local MORE_NAMES = { V = "vendors", D = "creatures", G = "chests", P = "pockets", F = "fishing spots", I = "containers",
	Q = "quests", A = "achievements", C = "recipes", T = "trainers" }

local function lockText(state)
	return "|cffff5555Progression stage " .. state .. "|r"
end

-- "12 [icon] 2 [icon]" for an extended cost's "id*count/id*count".
local function itemCosts(text)
	if not text or text == "-" or text == "" then return nil end
	local parts = {}
	for idText, count in tostring(text):gmatch("(%d+)%*(%d+)") do
		local id = tonumber(idText)
		local icon = J.ItemIcon(id)
		if not GetItemInfo(id) then J.QueryItem(id) end
		table.insert(parts, count .. " |T" .. icon .. ":14:14:0:0|t")
	end
	return #parts > 0 and table.concat(parts, " ") or nil
end

local function costText(s)
	local parts = {}
	if s.copper and s.copper > 0 then table.insert(parts, J.Money(s.copper)) end
	if s.honor and s.honor > 0 then
		local faction = UnitFactionGroup and UnitFactionGroup("player") or "Alliance"
		table.insert(parts, J.Number(s.honor) .. " |TInterface\\PVPFrame\\PVP-Currency-" .. faction .. ":14:14:0:0|t")
	end
	if s.arena and s.arena > 0 then
		table.insert(parts, J.Number(s.arena) .. " |TInterface\\PVPFrame\\PVP-ArenaPoints-Icon:14:14:0:0|t")
	end
	local items = itemCosts(s.items)
	if items then table.insert(parts, items) end
	return #parts > 0 and table.concat(parts, "  ") or nil
end

local function className(id)
	local token = J.CLASS_TOKENS[id or 0]
	return token and LOCALIZED_CLASS_NAMES_MALE and LOCALIZED_CLASS_NAMES_MALE[token] or token
end

-- The title, the line under it and the icon of a source row.
local function describe(s)
	local title, detail, icon = "", nil, SOURCE_ICONS[s.kind]
	local chance = s.chance and s.chance > 0 and J.Chance(s.chance) or nil
	if s.kind == "V" then
		title = "Vendor: " .. s.name
		local bits = {}
		if s.where ~= "" then table.insert(bits, s.where) end
		local cost = costText(s)
		if cost then table.insert(bits, cost) end
		if s.event ~= "" then table.insert(bits, "|cff66ccffDuring " .. s.event .. "|r") end
		detail = table.concat(bits, " - ")
	elseif s.kind == "D" then
		title = (s.instance and s.instance > 0 and s.boss and s.boss > 0 and "Boss: " or "Drop: ") .. s.name
			.. (s.rare and " |cffc0c0c0(Rare)|r" or "")
		if s.instance and s.instance > 0 then icon = BOSS_ICON end
		detail = (s.where ~= "" and (s.where .. " - ") or "") .. (chance or "")
	elseif s.kind == "G" then
		title = "Chest: " .. s.name
		detail = (s.where ~= "" and (s.where .. " - ") or "") .. (chance or "")
	elseif s.kind == "P" then
		title = "Pickpocket: " .. s.name
		detail = (s.where ~= "" and (s.where .. " - ") or "") .. (chance or "")
	elseif s.kind == "F" then
		title = "Fishing: " .. s.name
		detail = chance
	elseif s.kind == "I" then
		local name = GetItemInfo(s.item)
		if not name then J.QueryItem(s.item) end
		title = "Contained in: " .. (name or ("item " .. s.item))
		detail = chance
		icon = J.ItemIcon(s.item)
	elseif s.kind == "R" then
		title = "World drop"
		detail = "Drops from many creatures around the world"
	elseif s.kind == "Q" then
		title = "Quest: " .. s.name
		local bits = {}
		if s.giver ~= "" then table.insert(bits, "from " .. s.giver) end
		if s.where ~= "" then table.insert(bits, s.where) end
		if s.level and s.level > 0 then table.insert(bits, "level " .. s.level) end
		if s.faction == 1 then table.insert(bits, "|cff4080ffAlliance|r") elseif s.faction == 2 then table.insert(bits, "|cffff4040Horde|r") end
		detail = table.concat(bits, ", ")
	elseif s.kind == "A" then
		local ok, _, name, points, _, _, _, _, description, _, achIcon = pcall(GetAchievementInfo, s.achievement)
		title = "Achievement: " .. (ok and name or ("#" .. s.achievement))
		detail = ok and description or nil
		if ok and points and points > 0 then detail = (detail and (detail .. " ") or "") .. "(" .. points .. " points)" end
		if ok and achIcon then icon = achIcon end
	elseif s.kind == "C" then
		local recipe, _, spellIcon = GetSpellInfo(s.spell)
		title = "Profession: " .. s.name .. (s.rank and s.rank > 0 and (" (" .. s.rank .. ")") or "")
		detail = recipe
		icon = spellIcon or "Interface\\Icons\\Trade_Engineering"
	elseif s.kind == "T" then
		local cls = className(s.class)
		title = "Trainer: " .. (cls and (cls .. " trainers") or s.name)
		local bits = {}
		if s.level and s.level > 0 then table.insert(bits, "level " .. s.level) end
		if s.copper and s.copper > 0 then table.insert(bits, J.Money(s.copper)) end
		if s.name ~= "" then table.insert(bits, s.name .. (s.where ~= "" and (", " .. s.where) or "")) end
		if s.trainers and s.trainers > 1 then table.insert(bits, "and " .. (s.trainers - 1) .. " more") end
		detail = table.concat(bits, " - ")
	elseif s.kind == "N" then
		title = s.text
	elseif s.kind == "+" then
		title = "|cff9d9d9d...and " .. s.more .. " more " .. (MORE_NAMES[s.of] or "sources") .. "|r"
		icon = nil
	end
	if (s.reqState or 0) > 0 and J.Has(J.HELLO_PROGRESSION) and s.reqState > (J.state or 18) then
		detail = (detail and detail ~= "" and (detail .. " - ") or "") .. lockText(s.reqState)
	end
	return title, detail, icon
end

-----------------------------------------
-- one tab

local function buildTab(kind, panel)
	local info = J.KINDS[kind]
	local state = { selected = nil, entries = {}, catalog = nil }

	local status = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	status:SetPoint("CENTER", panel, "CENTER", 0, 20)
	status:SetWidth(520)
	local statusSub = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	statusSub:SetPoint("TOP", status, "BOTTOM", 0, -10)
	statusSub:SetWidth(520)

	local body = CreateFrame("Frame", nil, panel)
	body:SetAllPoints(panel)

	-- left: search, filters, count, list

	local left = CreateFrame("Frame", nil, body)
	left:SetPoint("TOPLEFT", body, "TOPLEFT", 0, 0)
	left:SetPoint("BOTTOMLEFT", body, "BOTTOMLEFT", 0, 0)
	left:SetWidth(LIST_WIDTH)

	local search = J.CreateEditBox(left, LIST_WIDTH - 104)
	search:SetPoint("TOPLEFT", left, "TOPLEFT", 8, -4)
	search.hint = search:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	search.hint:SetPoint("LEFT", search, "LEFT", 2, 0)
	search.hint:SetText("Search " .. info.name:lower())
	search.hint:SetTextColor(0.6, 0.6, 0.6)

	local filter = J.CreateDropdown(left, 90, function ()
		local f = J.Filters(kind)
		local function toggle(key) return function () f[key] = not f[key]; J.Fire("COLLECTION_FILTER", kind) end end
		local items = {
			{ text = "Collected", checked = f.collected, keepShownOnClick = true, func = toggle("collected") },
			{ text = "Not collected", checked = f.missing, keepShownOnClick = true, func = toggle("missing") },
			{ text = "Only usable by me", checked = f.usable, keepShownOnClick = true, func = toggle("usable") },
		}
		if J.Has(J.HELLO_PROGRESSION) then
			table.insert(items, { text = "Hide locked by progression", checked = f.hideLocked, keepShownOnClick = true,
				func = toggle("hideLocked") })
		end
		if kind == "mounts" then
			table.insert(items, { text = "Mount type", isTitle = true, notCheckable = true })
			for _, t in ipairs({ { nil, "All" }, { "flying", "Flying" }, { "ground", "Ground" } }) do
				table.insert(items, { text = t[2], checked = f.mountType == t[1], func = function ()
					f.mountType = t[1]
					J.Fire("COLLECTION_FILTER", kind)
				end })
			end
		end
		table.insert(items, { text = "Sources (none ticked: all)", isTitle = true, notCheckable = true })
		for _, src in ipairs(J.SOURCE) do
			table.insert(items, { text = src.name, checked = f.sources[src.bit] and true or false, keepShownOnClick = true,
				func = function ()
					f.sources[src.bit] = not f.sources[src.bit] or nil
					J.Fire("COLLECTION_FILTER", kind)
				end })
		end
		return items
	end)
	filter:SetPoint("LEFT", search, "RIGHT", 6, 0)
	filter:SetText("Filter")

	local count = left:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	count:SetPoint("TOPLEFT", left, "TOPLEFT", 8, -30)
	count:SetPoint("RIGHT", left, "RIGHT", -8, 0)
	count:SetJustifyH("LEFT")

	local listInset = J.CreateInset(left)
	listInset:SetPoint("TOPLEFT", left, "TOPLEFT", 0, -46)
	listInset:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", 0, 0)

	local scrollName = J.UniqueName("CollectionScroll")
	local scroll = CreateFrame("ScrollFrame", scrollName, listInset, "FauxScrollFrameTemplate")
	scroll:SetPoint("TOPLEFT", listInset, "TOPLEFT", 4, -4)
	scroll:SetPoint("BOTTOMRIGHT", listInset, "BOTTOMRIGHT", -26, 4)
	J.SkinScrollBar(scroll, scrollName)

	local empty = listInset:CreateFontString(nil, "OVERLAY", "GameFontDisable")
	empty:SetPoint("CENTER", listInset, "CENTER")
	empty:SetWidth(LIST_WIDTH - 30)

	local rows = {}
	local render

	local function select(e)
		state.selected = e
		JournalCharDB.collectionSelected = JournalCharDB.collectionSelected or {}
		JournalCharDB.collectionSelected[kind] = e and e.id or nil
		render()
	end

	for i = 1, ROWS do
		local b = CreateFrame("Button", nil, listInset)
		b:SetHeight(ROW_HEIGHT)
		b:SetPoint("TOPLEFT", listInset, "TOPLEFT", 6, -6 - (i - 1) * ROW_HEIGHT)
		b:SetPoint("RIGHT", scroll, "RIGHT", 0, 0)
		b.icon = b:CreateTexture(nil, "ARTWORK")
		b.icon:SetSize(32, 32)
		b.icon:SetPoint("LEFT", b, "LEFT", 2, 0)
		b.name = b:CreateFontString(nil, "OVERLAY", "GameFontNormal")
		b.name:SetPoint("TOPLEFT", b.icon, "TOPRIGHT", 8, -2)
		b.name:SetPoint("RIGHT", b, "RIGHT", -22, 0)
		b.name:SetJustifyH("LEFT")
		b.sub = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
		b.sub:SetPoint("TOPLEFT", b.name, "BOTTOMLEFT", 0, -2)
		b.sub:SetPoint("RIGHT", b, "RIGHT", -22, 0)
		b.sub:SetJustifyH("LEFT")
		b.lock = b:CreateTexture(nil, "OVERLAY")
		b.lock:SetTexture("Interface\\LFGFrame\\UI-LFG-ICON-LOCK")
		b.lock:SetSize(16, 16)
		b.lock:SetPoint("RIGHT", b, "RIGHT", -2, 0)
		b.selectedBg = b:CreateTexture(nil, "BACKGROUND")
		b.selectedBg:SetAllPoints(b)
		b.selectedBg:SetTexture("Interface\\QuestFrame\\UI-QuestLogTitleHighlight")
		b.selectedBg:SetBlendMode("ADD")
		b.selectedBg:SetVertexColor(1, 0.82, 0, 0.5)
		local hl = b:CreateTexture(nil, "HIGHLIGHT")
		hl:SetAllPoints(b)
		hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
		hl:SetBlendMode("ADD")
		b:SetScript("OnClick", function (self)
			local e = self.entry
			if not e then return end
			if IsModifiedClick("CHATLINK") then
				local link = kind == "toys" and J.ItemLink(e.id, J.EntryName(kind, e), e.quality)
					or (GetSpellLink and GetSpellLink(e.id))
				if link and not ChatEdit_InsertLink(link) then ChatFrame_OpenChat(link) end
				return
			end
			select(e)
		end)
		b:SetScript("OnEnter", function (self)
			local e = self.entry
			if not e then return end
			GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
			GameTooltip:SetHyperlink(kind == "toys" and ("item:" .. e.id) or ("spell:" .. e.id))
			GameTooltip:Show()
		end)
		b:SetScript("OnLeave", function () GameTooltip:Hide() end)
		rows[i] = b
	end

	local function summary(e)
		local parts = {}
		for _, src in ipairs(J.SOURCE) do
			if band(e.sources, src.bit) ~= 0 then table.insert(parts, src.name) end
		end
		if band(e.flags, J.ENTRY.UNOBTAINABLE) ~= 0 then return "|cff9d9d9dUnobtainable|r" end
		return table.concat(parts, ", ")
	end

	local function renderList()
		local offset = FauxScrollFrame_GetOffset(scroll)
		for i = 1, ROWS do
			local b = rows[i]
			local e = state.entries[offset + i]
			b.entry = e
			if e then
				b.icon:SetTexture(J.EntryIcon(kind, e))
				b.icon:SetDesaturated(not e.mine)
				b.name:SetText(J.EntryName(kind, e) or ("item " .. e.id))
				if e.mine then
					local r, g, bl = J.QualityColor(e.quality)
					b.name:SetTextColor(r, g, bl)
				else
					b.name:SetTextColor(0.6, 0.6, 0.6)
				end
				local locked = J.EntryLocked(e)
				b.sub:SetText(locked and lockText(e.reqState) or summary(e))
				setShown(b.lock, locked)
				setShown(b.selectedBg, state.selected == e)
				b:Show()
			else
				b:Hide()
			end
		end
		FauxScrollFrame_Update(scroll, #state.entries, ROWS, ROW_HEIGHT)
	end

	scroll:SetScript("OnVerticalScroll", function (self, offset)
		FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, renderList)
	end)

	-- right: the selected one

	local right = J.CreateInset(body)
	right:SetPoint("TOPLEFT", left, "TOPRIGHT", 8, 0)
	right:SetPoint("BOTTOMRIGHT", body, "BOTTOMRIGHT", 0, 0)

	local bigIcon = right:CreateTexture(nil, "ARTWORK")
	bigIcon:SetSize(40, 40)
	bigIcon:SetPoint("TOPLEFT", right, "TOPLEFT", 12, -12)
	local title = right:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	title:SetPoint("TOPLEFT", bigIcon, "TOPRIGHT", 10, -2)
	title:SetPoint("RIGHT", right, "RIGHT", -12, 0)
	title:SetJustifyH("LEFT")
	local subtitle = right:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	subtitle:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -4)
	subtitle:SetPoint("RIGHT", right, "RIGHT", -12, 0)
	subtitle:SetJustifyH("LEFT")

	local SOURCES_HEIGHT = 190

	local model = CreateFrame("PlayerModel", nil, right)
	model:SetPoint("TOPLEFT", right, "TOPLEFT", 8, -60)
	model:SetPoint("BOTTOMRIGHT", right, "BOTTOMRIGHT", -8, SOURCES_HEIGHT + 8)
	model:EnableMouse(true)
	model.facing = 0.5
	model:SetScript("OnMouseDown", function (self, btn)
		if btn == "LeftButton" then self.dragging = GetCursorPosition() end
	end)
	model:SetScript("OnMouseUp", function (self) self.dragging = nil end)
	model:SetScript("OnUpdate", function (self)
		if self.dragging then
			local x = GetCursorPosition()
			self.facing = self.facing + (x - self.dragging) / 80
			self.dragging = x
			self:SetFacing(self.facing)
		end
	end)
	model:SetScript("OnHide", function (self) self.shownEntry = nil end)
	local modelBg = right:CreateTexture(nil, "BACKGROUND", nil, 1)
	modelBg:SetPoint("TOPLEFT", model, "TOPLEFT")
	modelBg:SetPoint("BOTTOMRIGHT", model, "BOTTOMRIGHT")
	modelBg:SetTexture("Interface\\LFGFrame\\UI-LFG-BACKGROUND-QUESTPAPER")
	modelBg:SetTexCoord(0, 0.6, 0, 0.55)
	modelBg:SetVertexColor(0.35, 0.32, 0.3)
	local modelHint = model:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
	modelHint:SetPoint("BOTTOMLEFT", model, "BOTTOMLEFT", 6, 4)
	modelHint:SetText("Drag to turn")

	local function showCreature(entry)
		if not entry or entry == 0 then
			model:ClearModel()
			model.shownEntry = nil
			return
		end
		if model.shownEntry == entry then return end
		model.shownEntry = entry
		model:SetCreature(entry)
		model:SetFacing(model.facing)
		-- A creature the client hasn't seen yet only shows once its query comes back.
		J.After(0.8, function ()
			if model.shownEntry == entry and model:IsVisible() then
				model:SetCreature(entry)
				model:SetFacing(model.facing)
			end
		end)
	end

	-- Toys have no model: their icon and what using them does.
	local toyIcon = right:CreateTexture(nil, "ARTWORK")
	toyIcon:SetSize(96, 96)
	toyIcon:SetPoint("CENTER", model, "CENTER", 0, 30)
	local toyText = right:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	toyText:SetPoint("TOP", toyIcon, "BOTTOM", 0, -14)
	toyText:SetWidth(380)

	-- Summon (mounts, pets) or Use (toys, a secure button so the item can be used).
	local action = J.CreateButton(right, "Summon", 110, 24)
	action:SetPoint("BOTTOMRIGHT", model, "BOTTOMRIGHT", -6, 6)
	action:SetScript("OnClick", function ()
		local e = state.selected
		local c = e and J.Companion(kind, e)
		if not c then return end
		if kind == "mounts" and IsMounted and IsMounted() then
			Dismount()
		elseif c.active then
			DismissCompanion(info.companion)
		else
			CallCompanion(info.companion, c.index)
		end
		J.Debounce("collection-companions", 0.5, function () J.Fire("COMPANIONS") end)
	end)

	local use = CreateFrame("Button", J.UniqueName("ToyUse"), right, "SecureActionButtonTemplate,UIPanelButtonTemplate")
	use:SetSize(110, 24)
	use:SetText("Use")
	use:SetPoint("BOTTOMRIGHT", model, "BOTTOMRIGHT", -6, 6)
	use:SetAttribute("type", "item")
	do local D = J.Dragon() if D then D.SkinRedButton(use) end end
	use:Hide()

	local sourcesTitle = right:CreateFontString(nil, "OVERLAY", "GameFontNormal")
	sourcesTitle:SetPoint("TOPLEFT", model, "BOTTOMLEFT", 4, -8)
	sourcesTitle:SetText("Where to get it")
	local mapNote = right:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	mapNote:SetPoint("LEFT", sourcesTitle, "RIGHT", 12, 0)
	mapNote:SetPoint("RIGHT", right, "RIGHT", -12, 0)
	mapNote:SetJustifyH("LEFT")

	local sourceScroll = J.CreateScrollArea(right)
	sourceScroll:SetPoint("TOPLEFT", sourcesTitle, "BOTTOMLEFT", -4, -6)
	sourceScroll:SetPoint("BOTTOMRIGHT", right, "BOTTOMRIGHT", -28, 8)
	sourceScroll:SetScript("OnMouseWheel", function (self, delta)
		local max = math.max(0, self.content:GetHeight() - self:GetHeight())
		self:SetVerticalScroll(math.max(0, math.min(max, self:GetVerticalScroll() - delta * 40)))
	end)

	local function makeSourceRow()
		local b = CreateFrame("Button", nil, sourceScroll.content)
		b:SetHeight(34)
		b.icon = b:CreateTexture(nil, "ARTWORK")
		b.icon:SetSize(22, 22)
		b.icon:SetPoint("TOPLEFT", b, "TOPLEFT", 2, -4)
		b.title = b:CreateFontString(nil, "OVERLAY", "GameFontNormal")
		b.title:SetPoint("TOPLEFT", b, "TOPLEFT", 30, -3)
		b.title:SetPoint("RIGHT", b, "RIGHT", -4, 0)
		b.title:SetJustifyH("LEFT")
		b.detail = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
		b.detail:SetPoint("TOPLEFT", b.title, "BOTTOMLEFT", 0, -2)
		b.detail:SetPoint("RIGHT", b, "RIGHT", -4, 0)
		b.detail:SetJustifyH("LEFT")
		local hl = b:CreateTexture(nil, "HIGHLIGHT")
		hl:SetAllPoints(b)
		hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
		hl:SetBlendMode("ADD")
		hl:SetVertexColor(1, 1, 1, 0.4)
		b:SetScript("OnClick", function (self)
			local s, e = self.source, state.selected
			if not s or not e then return end
			if s.kind == "I" then
				if J.HandleItemClick(s.item) then return end
			elseif (s.kind == "D" or s.kind == "G") and s.instance and s.instance > 0 then
				J.OpenInstance(s.instance, { boss = s.boss and s.boss > 0 and s.boss or nil, tab = "loot" })
				return
			elseif s.kind == "A" then
				if not AchievementFrame and AchievementFrame_LoadUI then AchievementFrame_LoadUI() end
				if AchievementFrame and AchievementFrame_SelectAchievement then
					ShowUIPanel(AchievementFrame)
					AchievementFrame_SelectAchievement(s.achievement)
				end
				return
			end
			if s.placed then
				J.ShowSourceOnMap(kind, e, s, function (where)
					if state.selected ~= e then return end
					mapNote:SetText(where and ("|cff66ff66Flagged on your map:|r " .. where) or "|cff9d9d9dNot on any map the journal knows|r")
				end)
			end
		end)
		b:SetScript("OnEnter", function (self)
			local s = self.source
			if not s then return end
			if s.kind == "I" then
				J.ShowItemTooltip(self, s.item)
			elseif s.kind == "C" then
				GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
				GameTooltip:SetHyperlink("spell:" .. s.spell)
				GameTooltip:Show()
			elseif s.placed or (s.instance and s.instance > 0) then
				GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
				GameTooltip:SetText(self.title:GetText() or "")
				GameTooltip:AddLine(s.instance and s.instance > 0 and "Click to open it in the dungeon journal."
					or "Click to flag it on your map.", 0.8, 0.8, 0.8)
				GameTooltip:Show()
			end
		end)
		b:SetScript("OnLeave", function () GameTooltip:Hide() end)
		return b
	end
	local sourceRows = J.CreatePool(makeSourceRow)
	local sourcesEmpty = sourceScroll.content:CreateFontString(nil, "OVERLAY", "GameFontDisable")
	sourcesEmpty:SetPoint("TOPLEFT", sourceScroll.content, "TOPLEFT", 4, -4)

	local function renderSources(e)
		sourceRows:Reset()
		mapNote:SetText("")
		sourcesEmpty:SetText("Loading...")
		sourcesEmpty:Show()
		J.LoadSources(kind, e, function (list)
			if state.selected ~= e then return end
			sourceRows:Reset()
			if not list or #list == 0 then
				sourcesEmpty:SetText(list and "Nothing on this realm gives it" or "Couldn't ask the server")
				sourcesEmpty:Show()
				sourceScroll:SetContentHeight(20)
				return
			end
			sourcesEmpty:Hide()
			local y = 0
			for _, s in ipairs(list) do
				local b = sourceRows:Get()
				b.source = s
				local t, detail, icon = describe(s)
				b.title:SetText(t)
				b.detail:SetText(detail or "")
				b.icon:SetTexture(icon)
				setShown(b.icon, icon ~= nil)
				local h = (detail and detail ~= "") and 34 or 22
				b:SetHeight(h)
				b:ClearAllPoints()
				b:SetPoint("TOPLEFT", sourceScroll.content, "TOPLEFT", 0, -y)
				b:SetPoint("RIGHT", sourceScroll.content, "RIGHT", 0, 0)
				y = y + h + 2
			end
			sourceScroll:SetContentHeight(y)
		end)
	end

	local function renderDetail()
		local e = state.selected
		setShown(right, e ~= nil)
		if not e then return end
		local name = J.EntryName(kind, e) or ("item " .. e.id)
		bigIcon:SetTexture(J.EntryIcon(kind, e))
		title:SetText(name)
		local r, g, b = J.QualityColor(e.quality)
		title:SetTextColor(r, g, b)
		local bits = {}
		if kind == "mounts" then table.insert(bits, band(e.flags, J.ENTRY.FLYING) ~= 0 and "Flying mount" or "Mount") end
		if band(e.flags, J.ENTRY.ALLIANCE) ~= 0 then table.insert(bits, "|cff4080ffAlliance only|r") end
		if band(e.flags, J.ENTRY.HORDE) ~= 0 then table.insert(bits, "|cffff4040Horde only|r") end
		if band(e.flags, J.ENTRY.CLASS) ~= 0 and e.classMask ~= 0 then
			local names = {}
			for id = 1, 11 do
				if band(e.classMask, 2 ^ (id - 1)) ~= 0 and className(id) then table.insert(names, className(id)) end
			end
			if #names > 0 then table.insert(bits, table.concat(names, ", ") .. " only") end
		end
		table.insert(bits, e.mine and "|cff66ff66Collected|r" or "|cff9d9d9dNot collected|r")
		if J.EntryLocked(e) then table.insert(bits, lockText(e.reqState)) end
		subtitle:SetText(table.concat(bits, " - "))

		if kind == "toys" then
			model:Hide()
			modelBg:Show()
			toyIcon:SetTexture(J.ItemIcon(e.id))
			toyIcon:Show()
			local text = J.ItemUseText and J.ItemUseText(e.id) or nil
			toyText:SetText(text or "")
			toyText:Show()
			action:Hide()
			setShown(use, e.mine)
			if not InCombatLockdown() then use:SetAttribute("item", "item:" .. e.id) end
		else
			toyIcon:Hide()
			toyText:Hide()
			use:Hide()
			model:Show()
			showCreature(e.creature)
			local c = J.Companion(kind, e)
			setShown(action, c ~= nil)
			if c then
				local mounted = kind == "mounts" and IsMounted and IsMounted()
				action:SetText((c.active or mounted) and (kind == "mounts" and "Dismount" or "Dismiss") or "Summon")
			end
		end
		if state.sourcesFor ~= e then
			state.sourcesFor = e
			renderSources(e)
		end
	end

	render = function ()
		local problem, sub = J.ServerStatus()
		if not problem and not J.Has(J.HELLO_COLLECTIONS) then
			problem, sub = "This realm doesn't list " .. info.name:lower() .. " yet",
				"The realm's journal module has its collections turned off (Journal.Collections.Enable)."
		end
		if problem then
			body:Hide()
			status:SetText(problem)
			statusSub:SetText(sub or "")
			status:Show()
			statusSub:Show()
			return
		end
		status:Hide()
		statusSub:Hide()
		if not state.catalog then
			body:Hide()
			status:SetText("Loading " .. info.name:lower() .. "...")
			status:Show()
			return
		end
		body:Show()
		local have, total
		state.entries, have, total = J.FilteredEntries(kind, state.catalog, search:GetText())
		count:SetText(string.format("Collected: |cffffffff%d|r / %d", have, total))
		if not state.selected and JournalCharDB.collectionSelected then
			state.selected = state.catalog.byId[JournalCharDB.collectionSelected[kind] or -1]
		end
		-- A selection the filters now hide gives way to the first one shown.
		local listed = false
		for _, e in ipairs(state.entries) do if e == state.selected then listed = true break end end
		if not listed then state.selected = state.entries[1] end
		if #state.entries == 0 then
			empty:SetText(total == 0 and ("No " .. info.name:lower() .. " on this realm") or "Nothing matches the filters")
			empty:Show()
		else
			empty:Hide()
		end
		renderList()
		renderDetail()
	end

	local function load()
		if J.ServerStatus() or not J.Has(J.HELLO_COLLECTIONS) then render() return end
		J.Fire("COMPANIONS")
		if kind == "toys" then J.RefreshToys() end
		if state.catalog then render() return end
		render()
		J.LoadCatalog(kind, function (catalog)
			if not catalog then
				status:SetText("The " .. info.name:lower() .. " couldn't be loaded")
				statusSub:SetText("Try again in a moment with /journal refresh.")
				status:Show()
				statusSub:Show()
				return
			end
			state.catalog = catalog
			render()
		end)
	end

	local lastText = ""
	search:SetScript("OnTextChanged", function (self)
		local text = self:GetText() or ""
		setShown(self.hint, text == "" and not self:HasFocus())
		if text == lastText then return end
		lastText = text
		J.Debounce("collection-search-" .. kind, 0.2, function ()
			FauxScrollFrame_SetOffset(scroll, 0)
			local bar = _G[scrollName .. "ScrollBar"]
			if bar then bar:SetValue(0) end
			render()
		end)
	end)
	search:SetScript("OnEditFocusGained", function (self) self.hint:Hide() end)
	search:SetScript("OnEditFocusLost", function (self) setShown(self.hint, (self:GetText() or "") == "") end)

	local function shown() return panel:IsVisible() end
	J.On("COLLECTION_FILTER", function (k) if k == kind and shown() then FauxScrollFrame_SetOffset(scroll, 0) render() end end)
	J.On("COLLECTION_OWNED", function (k) if k == kind and shown() then render() end end)
	J.On("ITEM_INFO", function () if shown() then render() end end)
	J.On("SECTION_SERVER", function () if shown() then load() end end)
	J.On("CACHE_RESET", function ()
		state.catalog, state.selected, state.sourcesFor = nil, nil, nil
		if shown() then load() end
	end)
	J.On("LOCKOUTS_CHANGED", function () if shown() then render() end end)

	return load
end

-- What an item does when used: the green "Use:" lines of its tooltip.
local scanTip = CreateFrame("GameTooltip", "JournalToyScanTooltip", nil, "GameTooltipTemplate")
function J.ItemUseText(item)
	scanTip:SetOwner(WorldFrame, "ANCHOR_NONE")
	scanTip:ClearLines()
	scanTip:SetHyperlink("item:" .. item)
	local parts = {}
	for i = 2, scanTip:NumLines() do
		local line = _G["JournalToyScanTooltipTextLeft" .. i]
		local text = line and line:GetText()
		if text and text ~= "" then
			local r, g, b = line:GetTextColor()
			if g and g > 0.9 and r < 0.2 and b < 0.2 then table.insert(parts, text) end
		end
	end
	return #parts > 0 and table.concat(parts, "\n") or nil
end

for _, def in ipairs({
	{ key = "mounts", order = 30, icon = "Interface\\Icons\\Ability_Mount_RidingHorse" },
	{ key = "pets", order = 40, icon = "Interface\\Icons\\INV_Box_PetCarrier_01" },
	{ key = "toys", order = 50, icon = "Interface\\Icons\\INV_Misc_Toy_10" },
}) do
	local load
	J.AddSection({
		key = def.key, name = J.KINDS[def.key].name, order = def.order, icon = def.icon,
		build = function (panel) load = buildTab(def.key, panel) end,
		onShow = function () if load then load() end end,
	})
end
