-- The Quests tab: the instance's quests with your status on each (to pick up, in your log, ready
-- to turn in, done, too low, needs an earlier quest), who gives it and where, the XP and money
-- it pays you and its reward items. Click a quest for its objectives and a map flag on its giver.

local DJ = DungeonJournal
local setShown = DJ.SetShown
local Q = DJ.QUEST

local pane = CreateFrame("Frame")

local filterCheck = DJ.CreateCheck(pane, "Hide quests I've done")
filterCheck:SetPoint("TOPLEFT", pane, "TOPLEFT", 0, 2)
filterCheck:SetScript("OnClick", function (self)
	DungeonJournalCharDB.hideDoneQuests = self:GetChecked() and true or nil
	pane:Refresh(DJ.ctx)
end)

local countText = pane:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
countText:SetPoint("TOPRIGHT", pane, "TOPRIGHT", -4, -6)
countText:SetJustifyH("RIGHT")

local scroll = DJ.CreateScrollArea(pane)
scroll:SetPoint("TOPLEFT", pane, "TOPLEFT", 0, -28)
scroll:SetPoint("BOTTOMRIGHT", pane, "BOTTOMRIGHT", -22, 0)
scroll:SetScript("OnMouseWheel", function (self, delta)
	local max = math.max(0, self.content:GetHeight() - self:GetHeight())
	self:SetVerticalScroll(math.max(0, math.min(max, self:GetVerticalScroll() - delta * 60)))
end)
local c = scroll.content

local empty = pane:CreateFontString(nil, "OVERLAY", "GameFontDisable")
empty:SetPoint("TOP", pane, "TOP", 0, -80)
empty:SetWidth(380)

local STATE = {
	[Q.AVAILABLE] = { icon = "Interface\\GossipFrame\\AvailableQuestIcon", text = "Available", color = { 1, 0.82, 0 } },
	[Q.ACTIVE] = { icon = "Interface\\GossipFrame\\IncompleteQuestIcon", text = "In your quest log", color = { 0.5, 0.75, 1 } },
	[Q.COMPLETE] = { icon = "Interface\\GossipFrame\\ActiveQuestIcon", text = "Ready to turn in", color = { 0.3, 1, 0.3 } },
	[Q.DONE] = { icon = "Interface\\RaidFrame\\ReadyCheck-Ready", text = "Done", color = { 0.55, 0.55, 0.55 } },
	[Q.LOW_LEVEL] = { icon = "Interface\\GossipFrame\\AvailableQuestIcon", text = "Level too low", color = { 1, 0.35, 0.35 }, grey = true },
	[Q.PREREQ] = { icon = "Interface\\LFGFrame\\UI-LFG-ICON-LOCK", text = "Needs an earlier quest", color = { 0.8, 0.6, 0.4 } },
	[Q.LOCKED] = { icon = "Interface\\LFGFrame\\UI-LFG-ICON-LOCK", text = "Not available to you now", color = { 0.6, 0.6, 0.6 } },
}

-----------------------------------------
-- rows

local function makeRewardButton(parent)
	local b = DJ.CreateItemButton(parent, 24)
	b:SetScript("OnEnter", function (self)
		if self.entry then
			DJ.ShowItemTooltip(self, self.entry)
			if self.choice then GameTooltip:AddLine("One of these rewards, your choice.", 0.8, 0.8, 0.8) end
			GameTooltip:Show()
		end
	end)
	b:SetScript("OnLeave", function () GameTooltip:Hide() end)
	b:SetScript("OnClick", function (self)
		if self.entry then DJ.HandleItemClick(self.entry, GetItemInfo(self.entry), select(3, GetItemInfo(self.entry))) end
	end)
	return b
end

local function makeRow()
	local r = CreateFrame("Button", nil, c)
	r.icon = r:CreateTexture(nil, "ARTWORK")
	r.icon:SetSize(18, 18)
	r.icon:SetPoint("TOPLEFT", r, "TOPLEFT", 4, -5)
	r.name = r:CreateFontString(nil, "OVERLAY", "GameFontNormal")
	r.name:SetPoint("TOPLEFT", r, "TOPLEFT", 28, -6)
	r.name:SetJustifyH("LEFT")
	r.pay = r:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	r.pay:SetPoint("TOPRIGHT", r, "TOPRIGHT", -6, -6)
	r.pay:SetJustifyH("RIGHT")
	r.giver = r:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	r.giver:SetPoint("TOPLEFT", r.name, "BOTTOMLEFT", 0, -3)
	r.giver:SetJustifyH("LEFT")
	r.giver:SetTextColor(0.75, 0.75, 0.75)
	r.status = r:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	r.status:SetPoint("TOPRIGHT", r.pay, "BOTTOMRIGHT", 0, -3)
	r.status:SetJustifyH("RIGHT")
	r.rewards = {}
	r.detail = r:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	r.detail:SetJustifyH("LEFT")
	r.detail:SetJustifyV("TOP")
	r.mapButton = DJ.CreateButton(r, "Show giver on map", 150, 20)
	r.mapButton:SetScript("OnClick", function (self)
		local q = self:GetParent().quest
		if q then DJ.ShowOnMap("Q", q.id) end
	end)
	local hl = r:CreateTexture(nil, "HIGHLIGHT")
	hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
	hl:SetBlendMode("ADD")
	hl:SetVertexColor(1, 1, 1, 0.15)
	hl:SetAllPoints(r)
	r.band = r:CreateTexture(nil, "BACKGROUND")
	r.band:SetAllPoints(r)
	r.band:SetTexture(1, 1, 1, 0.04)
	r:SetScript("OnClick", function (self)
		local q = self.quest
		if not q then return end
		if IsModifiedClick("CHATLINK") then
			local link = string.format("|cffffff00|Hquest:%d:%d|h[%s]|h|r", q.id, q.level or 0, q.name)
			if not ChatEdit_InsertLink(link) then ChatFrame_OpenChat(link) end
			return
		end
		pane.open = pane.open ~= q.id and q.id or nil
		pane:Refresh(DJ.ctx)
	end)
	return r
end

local rows = DJ.CreatePool(makeRow)

local function payText(q)
	local parts = {}
	if q.xp and q.xp > 0 then table.insert(parts, DJ.Number(q.xp) .. " XP") end
	if q.money and q.money > 0 then table.insert(parts, DJ.Money(q.money)) end
	return table.concat(parts, "  ")
end

local function giverText(q)
	if q.giverKind == "I" then
		return "Starts from " .. (GetItemInfo(q.giverEntry) or q.giver) .. (q.where ~= "" and (", " .. q.where:lower()) or "")
	elseif q.giver ~= "" then
		return q.giver .. (q.where ~= "" and ("  -  " .. q.where) or "")
	end
	return ""
end

local function flagText(q)
	local parts = {}
	if bit.band(q.flags, 1) ~= 0 then table.insert(parts, "Daily") end
	if bit.band(q.flags, 2) ~= 0 then table.insert(parts, "Weekly") end
	if bit.band(q.flags, 16) ~= 0 then table.insert(parts, "Heroic")
	elseif bit.band(q.flags, 32) ~= 0 then table.insert(parts, "Raid")
	elseif bit.band(q.flags, 8) ~= 0 then table.insert(parts, "Group") end
	return #parts > 0 and (" |cff9d9d9d(" .. table.concat(parts, ", ") .. ")|r") or ""
end

local function placeRewards(r, q, width, y)
	for _, b in ipairs(r.rewards) do b:Hide() end
	local list = {}
	for _, it in ipairs(q.rewards) do table.insert(list, { id = it.id, n = it.n }) end
	for _, it in ipairs(q.choices) do table.insert(list, { id = it.id, n = it.n, choice = true }) end
	if #list == 0 then return y end
	local x = 28
	for i, it in ipairs(list) do
		local b = r.rewards[i]
		if not b then
			b = makeRewardButton(r)
			r.rewards[i] = b
		end
		local _, _, quality = GetItemInfo(it.id)
		if not quality then DJ.QueryItem(it.id) end
		b.entry = it.id
		b.choice = it.choice
		b:SetItem(DJ.ItemIcon(it.id), quality, it.n)
		b:ClearAllPoints()
		b:SetPoint("TOPLEFT", r, "TOPLEFT", x, -y)
		b:Show()
		x = x + 28
		if x > width - 28 and list[i + 1] then
			x = 28
			y = y + 28
		end
	end
	return y + 28
end

local serial = 0

function pane:Refresh(ctx)
	serial = serial + 1
	local mine = serial
	filterCheck:SetChecked(DungeonJournalCharDB.hideDoneQuests and true or false)
	if pane.lastInst ~= ctx.inst.id then
		pane.lastInst = ctx.inst.id
		pane.open = nil
		scroll:ScrollToTop()
	end
	DJ.LoadQuests(ctx.inst, function (quests)
		if mine ~= serial then return end
		rows:Reset()
		local width = math.max(200, scroll:GetWidth() - 8)
		local y = 0
		local shown, done = 0, 0
		for _, q in ipairs(quests or {}) do
			if q.state == Q.DONE then done = done + 1 end
			if not (DungeonJournalCharDB.hideDoneQuests and q.state == Q.DONE) then
				shown = shown + 1
				local st = STATE[q.state] or STATE[Q.LOCKED]
				local r = rows:Get()
				r.quest = q
				r:ClearAllPoints()
				r:SetPoint("TOPLEFT", c, "TOPLEFT", 0, -y)
				r:SetWidth(width)
				r.icon:SetTexture(st.icon)
				r.icon:SetDesaturated(st.grey and true or false)

				local level = q.level and q.level > 0 and q.level or q.minLevel
				local color = GetQuestDifficultyColor and GetQuestDifficultyColor(level) or { r = 1, g = 0.82, b = 0 }
				r.name:SetText(string.format("[%d] %s", level or 0, q.name) .. flagText(q))
				r.name:SetTextColor(color.r, color.g, color.b)
				r.name:SetWidth(width - 160)
				r.pay:SetText(payText(q))
				r.giver:SetText(giverText(q))
				r.giver:SetWidth(width - 160)
				local status = st.text
				if q.state == Q.LOW_LEVEL then status = "Needs level " .. (q.minLevel or "?") end
				r.status:SetText(status)
				r.status:SetTextColor(st.color[1], st.color[2], st.color[3])

				local h = placeRewards(r, q, width, 42)
				if pane.open == q.id then
					r.detail:ClearAllPoints()
					r.detail:SetPoint("TOPLEFT", r, "TOPLEFT", 28, -h - 2)
					r.detail:SetWidth(width - 40)
					r.detail:SetText("Loading...")
					r.detail:Show()
					local d = r.detail
					local function fill(detail)
						if not detail then d:SetText("The objectives couldn't be loaded.") return end
						local lines = {}
						if detail.objectives ~= "" then table.insert(lines, detail.objectives) end
						for _, k in ipairs(detail.kills) do
							table.insert(lines, "|cffffd200-|r " .. (k.target < 0 and "Use " or "Slay ") .. k.name
								.. (k.count and k.count > 1 and (" x" .. k.count) or ""))
						end
						for _, it in ipairs(detail.items) do
							table.insert(lines, "|cffffd200-|r " .. (GetItemInfo(it.item) or it.name) .. (it.count > 1 and (" x" .. it.count) or ""))
						end
						if detail.prev then
							table.insert(lines, (detail.prev.done and "|cff40ff40Done:|r " or "|cffff6060First:|r ") .. detail.prev.name)
						end
						d:SetText(table.concat(lines, "\n"))
					end
					local waiting = true
					DJ.LoadQuestDetail(q.id, function (detail)
						if mine ~= serial then return end
						fill(detail)
						-- The detail arrived after the layout: lay out again with its height. Not
						-- after a failure, which would only ask again.
						if detail and not waiting then pane:Refresh(DJ.ctx) end
					end)
					waiting = false
					h = h + 4 + d:GetStringHeight() + 6
					if q.giverKind ~= "I" and DJ.Has(DJ.HELLO_MAP_PINS) then
						r.mapButton:ClearAllPoints()
						r.mapButton:SetPoint("TOPLEFT", r, "TOPLEFT", 28, -h)
						r.mapButton:Show()
						h = h + 26
					else
						r.mapButton:Hide()
					end
				else
					r.detail:Hide()
					r.mapButton:Hide()
				end
				r:SetHeight(h + 4)
				y = y + h + 8
			end
		end
		scroll:SetContentHeight(y)
		countText:SetText(quests and (#quests .. " quests, " .. done .. " done") or "")
		if not quests then
			empty:SetText("The quests couldn't be loaded.")
			empty:Show()
		elseif shown == 0 then
			empty:SetText(#quests == 0 and "No quests for this instance." or "You've done every quest here.")
			empty:Show()
		else
			empty:Hide()
		end
	end)
end

DJ.On("QUESTS_STALE", function () if pane:IsShown() and DJ.ctx then pane:Refresh(DJ.ctx) end end)

DJ.RegisterPane("quests", pane)
