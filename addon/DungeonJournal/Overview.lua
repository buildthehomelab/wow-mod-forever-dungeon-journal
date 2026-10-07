-- The Overview tab. With no boss picked it describes the instance: difficulties with your
-- lockout on each, what it takes to get in, where the entrance is, your era lock and its quests.
-- With a boss picked it shows the boss's model (drag to turn it), level, health on the chosen
-- difficulty, the creatures fought in the encounter and whether you killed it this lockout.

local DJ = DungeonJournal
local setShown = DJ.SetShown

local pane = CreateFrame("Frame")
local scroll = DJ.CreateScrollArea(pane)
scroll:SetPoint("TOPLEFT", pane, "TOPLEFT", 0, 0)
scroll:SetPoint("BOTTOMRIGHT", pane, "BOTTOMRIGHT", -22, 0)
scroll:SetScript("OnMouseWheel", function (self, delta)
	local max = math.max(0, self.content:GetHeight() - self:GetHeight())
	self:SetVerticalScroll(math.max(0, math.min(max, self:GetVerticalScroll() - delta * 40)))
end)
local c = scroll.content

-----------------------------------------
-- building blocks, laid out top to bottom

local texts = DJ.CreatePool(function ()
	local fs = c:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	fs:SetJustifyH("LEFT")
	fs:SetJustifyV("TOP")
	return fs
end)
local icons = DJ.CreatePool(function () return c:CreateTexture(nil, "OVERLAY") end)
local buttons = DJ.CreatePool(function () return DJ.CreateButton(c, "", 160, 22) end)

local y, width

local function reset()
	texts:Reset()
	icons:Reset()
	buttons:Reset()
	y = 4
	width = math.max(200, scroll:GetWidth() - 8)
end

local function text(str, font, color, indent, gap)
	local fs = texts:Get()
	fs:SetFontObject(font or GameFontHighlight)
	fs:ClearAllPoints()
	fs:SetPoint("TOPLEFT", c, "TOPLEFT", 4 + (indent or 0), -y)
	fs:SetWidth(width - (indent or 0))
	fs:SetText(str)
	if color then fs:SetTextColor(color[1], color[2], color[3]) else fs:SetTextColor(1, 1, 1) end
	y = y + fs:GetStringHeight() + (gap or 6)
	return fs
end

local GOLD = { 1, 0.82, 0 }
local GREY = { 0.62, 0.62, 0.62 }
local RED = { 1, 0.35, 0.35 }
local GREEN = { 0.3, 1, 0.3 }

local function heading(str)
	y = y + 6
	text(str, GameFontNormalLarge, GOLD, 0, 4)
	local rule = icons:Get()
	rule:SetTexture(1, 0.82, 0, 0.35)
	rule:ClearAllPoints()
	rule:SetPoint("TOPLEFT", c, "TOPLEFT", 4, -y)
	rule:SetSize(width, 1)
	y = y + 6
end

-- A line with a ready-check tick or cross in front.
local function check(ok, str)
	local icon = icons:Get()
	icon:SetTexture(ok and "Interface\\RaidFrame\\ReadyCheck-Ready" or "Interface\\RaidFrame\\ReadyCheck-NotReady")
	icon:SetSize(14, 14)
	icon:ClearAllPoints()
	icon:SetPoint("TOPLEFT", c, "TOPLEFT", 6, -y + 1)
	text(str, GameFontHighlight, ok and nil or RED, 22, 4)
end

local function button(label, onClick, x)
	local b = buttons:Get()
	b:SetText(label)
	b:SetWidth(math.max(120, b:GetFontString():GetStringWidth() + 30))
	b:ClearAllPoints()
	b:SetPoint("TOPLEFT", c, "TOPLEFT", 4 + (x or 0), -y)
	b:SetScript("OnClick", onClick)
	return b
end

-----------------------------------------
-- the boss model

local model = CreateFrame("PlayerModel", nil, c)
model:SetHeight(250)
model:EnableMouse(true)
model.facing = 0.4
model:SetScript("OnMouseDown", function (self, btn)
	if btn == "LeftButton" then
		self.dragging = GetCursorPosition()
	end
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
local modelBg = c:CreateTexture(nil, "BACKGROUND")
modelBg:SetPoint("TOPLEFT", model, "TOPLEFT")
modelBg:SetPoint("BOTTOMRIGHT", model, "BOTTOMRIGHT")
modelBg:SetTexture("Interface\\LFGFrame\\UI-LFG-BACKGROUND-QUESTPAPER")
modelBg:SetTexCoord(0, 0.6, 0, 0.55)
modelBg:SetVertexColor(0.35, 0.32, 0.3)
local modelHint = model:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
modelHint:SetPoint("BOTTOMRIGHT", model, "BOTTOMRIGHT", -6, 4)
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
	DJ.After(0.8, function ()
		if model.shownEntry == entry and model:IsVisible() then
			model:SetCreature(entry)
			model:SetFacing(model.facing)
		end
	end)
end

-----------------------------------------
-- instance overview

local serial = 0

local function instanceOverview(ctx)
	local inst, variant = ctx.inst, ctx.variant
	local mine = serial
	model:Hide()
	modelBg:Hide()

	text(inst.name, GameFontNormalHuge, GOLD, 0, 2)
	text((inst.raid and "Raid" or "Dungeon") .. " for level " .. inst.minLevel .. "-" .. inst.maxLevel
		.. (inst.targetLevel and inst.targetLevel > 0 and (", best around " .. inst.targetLevel) or ""), GameFontHighlight, GREY)

	local st = ctx.status and ctx.status.inst == inst.id and ctx.status.value or nil

	heading("Difficulties")
	for _, v in ipairs(DJ.VariantsFor(inst, DJ.view.tier)) do
		local line = "|cffffd200" .. v.label .. "|r  " .. (v.players or 0) .. " players"
		if v.minLevel and v.minLevel > 0 then line = line .. ", level " .. v.minLevel .. "+" end
		local vs = st and st.variants[v.id]
		if DJ.IsLocked(v) then
			line = line .. "  |cffff5555locked: progression stage " .. v.reqState .. " (you: " .. (DJ.state or 0) .. ")|r"
		elseif vs and vs.saved then
			local killed, total = 0, 0
			for _, b in ipairs(ctx.bosses or {}) do
				if not b.rare and DJ.BossOnVariant(b, v) then
					total = total + 1
					if DJ.Killed(st, v, b) then killed = killed + 1 end
				end
			end
			local left = math.max(0, (vs.resetIn or 0) - (GetTime() - (vs.at or GetTime())))
			line = line .. "  |cff80c0ffsaved, " .. killed .. "/" .. total .. " defeated"
				.. (left > 0 and (", resets in " .. DJ.Duration(left)) or "") .. "|r"
		end
		text(line, GameFontHighlight, nil, 6, 4)
	end

	local reqs = st and st.requirements[variant.id]
	if reqs and #reqs > 0 then
		heading("To enter (" .. variant.label .. ")")
		for _, r in ipairs(reqs) do
			if r.kind == "L" then
				check(r.have, "Level " .. r.id)
			elseif r.kind == "I" then
				check(r.have, "Carry " .. (GetItemInfo(r.id) or r.name))
			elseif r.kind == "Q" then
				check(r.have, "Complete the quest " .. r.name)
			elseif r.kind == "A" then
				check(r.have, "Achievement: " .. r.name)
			end
		end
	end

	if DJ.IsLocked(variant) then
		heading("Progression")
		text("Your character hasn't reached this part of the game yet. It opens at progression stage "
			.. variant.reqState .. "; you are at stage " .. (DJ.state or 0) .. ".", GameFontHighlight, RED)
	end

	heading("Entrance")
	if inst.zone ~= "" then
		text(inst.zone, GameFontHighlight)
		if DJ.Has(DJ.HELLO_MAP_PINS) then
			button("Show on map", function () DJ.ShowOnMap("E", inst.id) end)
			y = y + 28
		end
	else
		text("Unknown", GameFontHighlight, GREY)
	end

	if #inst.wings > 0 then
		heading("Wings")
		for _, w in ipairs(inst.wings) do
			text("|cffffd200" .. w.name .. "|r  level " .. w.minLevel .. "-" .. w.maxLevel, GameFontHighlight, nil, 6, 4)
		end
	end

	heading("Quests")
	local questLine = text("Loading...", GameFontHighlight, GREY)
	DJ.LoadQuests(inst, function (quests)
		if not quests or mine ~= serial then return end
		local available, active, done = 0, 0, 0
		for _, q in ipairs(quests) do
			if q.state == DJ.QUEST.AVAILABLE then available = available + 1
			elseif q.state == DJ.QUEST.ACTIVE or q.state == DJ.QUEST.COMPLETE then active = active + 1
			elseif q.state == DJ.QUEST.DONE then done = done + 1 end
		end
		if #quests == 0 then
			questLine:SetText("No quests for this instance.")
		else
			questLine:SetText(string.format("%d quests: |cffffd200%d to pick up|r, |cff80c0ff%d in your log|r, %d done.",
				#quests, available, active, done))
			questLine:SetTextColor(1, 1, 1)
		end
	end)
	button("See quests", function () DJ.Show({ tab = "quests" }) end)
	y = y + 28
end

-----------------------------------------
-- boss overview

local RANKS = { [0] = "", [1] = "Elite", [2] = "Rare Elite", [3] = "Boss", [4] = "Rare" }

local function bossOverview(ctx)
	local inst, boss, variant = ctx.inst, ctx.boss, ctx.variant
	local mine = serial
	local st = ctx.status and ctx.status.inst == inst.id and ctx.status.value or nil

	if boss.display and boss.display > 0 then
		model:ClearAllPoints()
		model:SetPoint("TOPLEFT", c, "TOPLEFT", 4, -y)
		model:SetWidth(width)
		model:Show()
		modelBg:Show()
		showCreature(pane.unit or boss.display)
		y = y + 256
	else
		model:Hide()
		modelBg:Hide()
	end

	text(boss.name, GameFontNormalHuge, GOLD, 0, 2)
	local killed = DJ.Killed(st, variant, boss)
	if boss.rare then
		text("Rare spawn: it isn't there every run.", GameFontHighlight, { 0.75, 0.75, 1 })
	elseif killed then
		text("Defeated this lockout (" .. variant.label .. ")", GameFontHighlight, GREEN)
	end

	local unitsAt = y
	local placeholder = text("Loading...", GameFontHighlight, GREY)
	DJ.LoadAbilities(inst, boss, variant, function (units)
		if not units or mine ~= serial then return end
		placeholder:Hide()
		y = unitsAt
		local abilities = 0
		local mains = {}
		for _, u in ipairs(units) do
			if not u.add then table.insert(mains, u) end
			for _, s in ipairs(u.spells) do
				if DJ.SpellDescription(s.spell) then abilities = abilities + 1 end
			end
		end
		for _, u in ipairs(mains) do
			local rank = RANKS[u.rank] or ""
			local skull = u.rank == 3 and "|TInterface\\TargetingFrame\\UI-TargetingFrame-Skull:14|t " or ""
			local line = skull .. "|cffffd200" .. u.name .. "|r  " .. (u.rank == 3 and "Level ??" or ("Level " .. u.level))
				.. (rank ~= "" and (" " .. rank) or "")
			if u.health and u.health > 0 then line = line .. "  -  " .. DJ.Number(u.health) .. " health" end
			text(line, GameFontHighlight, nil, 0, 4)
			if #mains > 1 and boss.display and boss.display > 0 then
				local b = button("Show " .. u.name, function () pane.unit = u.entry ; showCreature(u.entry) end, 12)
				b:SetHeight(20)
				y = y + 24
			end
		end
		y = y + 6
		local summary = abilities == 1 and "1 ability" or (abilities .. " abilities")
		button("Abilities (" .. abilities .. ")", function () DJ.Show({ tab = "abilities" }) end)
		button("Loot", function () DJ.Show({ tab = "loot" }) end, 170)
		y = y + 30
		if abilities == 0 and DJ.Has(DJ.HELLO_LEARNING) then
			text("No abilities known yet: the journal learns them as this boss is fought.", GameFontHighlightSmall, GREY)
		end
		text(summary .. " on " .. variant.label .. ".", GameFontHighlightSmall, GREY)
		scroll:SetContentHeight(y)
	end)
end

function pane:Refresh(ctx)
	serial = serial + 1
	reset()
	if not ctx.boss then pane.unit = nil end
	if pane.lastBoss ~= (ctx.boss and ctx.boss.key) then
		pane.unit = nil
		pane.lastBoss = ctx.boss and ctx.boss.key
		scroll:ScrollToTop()
	end
	if ctx.boss then bossOverview(ctx) else instanceOverview(ctx) end
	scroll:SetContentHeight(y)
end

DJ.On("STATUS", function () if pane:IsShown() and DJ.ctx then pane:Refresh(DJ.ctx) end end)

DJ.RegisterPane("overview", pane)
