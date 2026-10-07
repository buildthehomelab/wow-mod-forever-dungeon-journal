-- The Abilities tab: per creature of the encounter (the boss, others fought with it, then adds it
-- summons), each ability with its icon, cast time, tags (Magic, Interruptible, AoE...) and the
-- description from the spell's own tooltip. Abilities the server only knows because the boss was
-- seen casting them are marked "seen in combat". Hover an ability for its full tooltip;
-- Shift-click links it in chat.

local DJ = DungeonJournal
local setShown = DJ.SetShown
local TAG = DJ.TAG

local pane = CreateFrame("Frame")
local scroll = DJ.CreateScrollArea(pane)
scroll:SetPoint("TOPLEFT", pane, "TOPLEFT", 0, 0)
scroll:SetPoint("BOTTOMRIGHT", pane, "BOTTOMRIGHT", -22, 0)
scroll:SetScript("OnMouseWheel", function (self, delta)
	local max = math.max(0, self.content:GetHeight() - self:GetHeight())
	self:SetVerticalScroll(math.max(0, math.min(max, self:GetVerticalScroll() - delta * 50)))
end)
local c = scroll.content

-- Tag text and colour, in the order they're shown.
local TAGS = {
	{ TAG.INTERRUPT, "Interruptible", { 0.4, 0.8, 1 } },
	{ TAG.MAGIC, "Magic", { 0.2, 0.6, 1 } },
	{ TAG.CURSE, "Curse", { 0.6, 0, 1 } },
	{ TAG.POISON, "Poison", { 0, 0.6, 0 } },
	{ TAG.DISEASE, "Disease", { 0.6, 0.4, 0 } },
	{ TAG.ENRAGE, "Enrage", { 1, 0.35, 0.2 } },
	{ TAG.AOE, "Area", { 1, 0.6, 0.2 } },
	{ TAG.CC, "Crowd control", { 1, 0.45, 0.8 } },
	{ TAG.KNOCKBACK, "Knockback", { 0.9, 0.9, 0.5 } },
	{ TAG.HEAL, "Heal", { 0.3, 1, 0.3 } },
	{ TAG.SUMMON, "Summon", { 0.8, 0.7, 1 } },
	{ TAG.BUFF, "Buff", { 0.7, 0.9, 0.7 } },
}

local function tagText(tags)
	local parts = {}
	for _, t in ipairs(TAGS) do
		if bit.band(tags, t[1]) ~= 0 then
			table.insert(parts, DJ.Colored(t[2], t[3][1], t[3][2], t[3][3]))
		end
	end
	return table.concat(parts, "  ")
end

local function castText(s)
	local ms = s.castMs or 0
	if ms <= 0 then return "Instant" end
	local secs = ms / 1000
	local str = secs == math.floor(secs) and tostring(secs) or string.format("%.1f", secs)
	-- The server sends a channel's duration as its cast time; the tooltip says which it is.
	return str .. " sec"
end

local function makeHeader()
	local h = CreateFrame("Frame", nil, c)
	h:SetHeight(26)
	h.band = h:CreateTexture(nil, "BACKGROUND")
	h.band:SetAllPoints(h)
	h.band:SetTexture(0.2, 0.14, 0.04, 0.8)
	h.rule = h:CreateTexture(nil, "BORDER")
	h.rule:SetHeight(1)
	h.rule:SetPoint("BOTTOMLEFT", h, "BOTTOMLEFT")
	h.rule:SetPoint("BOTTOMRIGHT", h, "BOTTOMRIGHT")
	h.rule:SetTexture(1, 0.82, 0, 0.55)
	h.text = h:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	h.text:SetPoint("LEFT", h, "LEFT", 6, 0)
	h.extra = h:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	h.extra:SetPoint("RIGHT", h, "RIGHT", -6, 0)
	return h
end

local function makeRow()
	local r = CreateFrame("Button", nil, c)
	r.icon = r:CreateTexture(nil, "ARTWORK")
	r.icon:SetSize(30, 30)
	r.icon:SetPoint("TOPLEFT", r, "TOPLEFT", 4, -4)
	r.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)
	r.name = r:CreateFontString(nil, "OVERLAY", "GameFontNormal")
	r.name:SetPoint("TOPLEFT", r.icon, "TOPRIGHT", 8, 0)
	r.name:SetJustifyH("LEFT")
	r.cast = r:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	r.cast:SetPoint("TOPRIGHT", r, "TOPRIGHT", -4, -4)
	r.cast:SetJustifyH("RIGHT")
	r.tags = r:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	r.tags:SetPoint("TOPLEFT", r.name, "BOTTOMLEFT", 0, -2)
	r.tags:SetJustifyH("LEFT")
	r.desc = r:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	r.desc:SetPoint("TOPLEFT", r.tags, "BOTTOMLEFT", 0, -4)
	r.desc:SetJustifyH("LEFT")
	r.desc:SetJustifyV("TOP")
	r.desc:SetTextColor(0.85, 0.85, 0.85)
	local hl = r:CreateTexture(nil, "HIGHLIGHT")
	hl:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
	hl:SetBlendMode("ADD")
	hl:SetVertexColor(1, 1, 1, 0.15)
	hl:SetAllPoints(r)
	r:SetScript("OnEnter", function (self)
		GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
		GameTooltip:SetHyperlink("spell:" .. self.spell)
		if self.learned then GameTooltip:AddLine("Seen in combat on this realm.", 0.6, 0.6, 0.6) end
		GameTooltip:Show()
	end)
	r:SetScript("OnLeave", function () GameTooltip:Hide() end)
	r:SetScript("OnClick", function (self)
		if IsModifiedClick("CHATLINK") and GetSpellLink then
			local link = GetSpellLink(self.spell)
			if link then ChatEdit_InsertLink(link) end
		end
	end)
	return r
end

local headers = DJ.CreatePool(makeHeader)
local rows = DJ.CreatePool(makeRow)
local empty = pane:CreateFontString(nil, "OVERLAY", "GameFontDisable")
empty:SetPoint("TOP", pane, "TOP", 0, -60)
empty:SetWidth(380)

local RANK_TEXT = { [1] = "Elite", [2] = "Rare Elite", [3] = "Boss", [4] = "Rare" }

local serial = 0

function pane:Refresh(ctx)
	serial = serial + 1
	local mine = serial
	headers:Reset()
	rows:Reset()
	empty:Hide()
	if not ctx.boss then return end
	if pane.lastKey ~= ctx.boss.key then
		pane.lastKey = ctx.boss.key
		scroll:ScrollToTop()
	end
	DJ.LoadAbilities(ctx.inst, ctx.boss, ctx.variant, function (units)
		if mine ~= serial then return end
		headers:Reset()
		rows:Reset()
		local width = math.max(200, scroll:GetWidth() - 8)
		local y = 0
		local shownAny = false
		local addsHeader = false
		for _, u in ipairs(units or {}) do
			local spells = {}
			for _, s in ipairs(u.spells) do
				local desc = DJ.SpellDescription(s.spell)
				local name, _, icon = GetSpellInfo(s.spell)
				if desc and name then table.insert(spells, { s = s, desc = desc, name = name, icon = icon }) end
			end
			if #spells > 0 or not u.add then
				if u.add and not addsHeader then
					addsHeader = true
					y = y + 8
				end
				local h = headers:Get()
				h:ClearAllPoints()
				h:SetPoint("TOPLEFT", c, "TOPLEFT", 0, -y)
				h:SetWidth(width)
				h.text:SetText((u.add and "Add: " or "") .. u.name)
				local extra = u.rank == 3 and "Level ??" or ("Level " .. (u.level or "?"))
				if RANK_TEXT[u.rank] then extra = extra .. " " .. RANK_TEXT[u.rank] end
				if u.health and u.health > 0 then extra = extra .. "  -  " .. DJ.Number(u.health) .. " health" end
				h.extra:SetText(extra)
				y = y + 30
				if #spells == 0 then
					local r = rows:Get()
					r:ClearAllPoints()
					r:SetPoint("TOPLEFT", c, "TOPLEFT", 0, -y)
					r:SetWidth(width)
					r.spell = 6603 -- Auto Attack, for the tooltip
					r.learned = nil
					r.icon:SetTexture("Interface\\Icons\\INV_Sword_04")
					r.name:SetText("Melee")
					r.cast:SetText("")
					r.tags:SetText("")
					r.desc:SetWidth(width - 48)
					r.desc:SetText(DJ.Has(DJ.HELLO_LEARNING)
						and "No special abilities known. The journal learns them as this creature is fought."
						or "No special abilities known.")
					r:SetHeight(42 + r.desc:GetStringHeight())
					y = y + r:GetHeight()
				end
				for _, sp in ipairs(spells) do
					shownAny = true
					local r = rows:Get()
					r:ClearAllPoints()
					r:SetPoint("TOPLEFT", c, "TOPLEFT", 0, -y)
					r:SetWidth(width)
					r.spell = sp.s.spell
					r.learned = bit.band(sp.s.tags, TAG.LEARNED) ~= 0
					r.icon:SetTexture(sp.icon or "Interface\\Icons\\INV_Misc_QuestionMark")
					r.name:SetText(sp.name)
					r.name:SetWidth(width - 130)
					r.cast:SetText(castText(sp.s))
					local tags = tagText(sp.s.tags)
					if r.learned then tags = (tags ~= "" and (tags .. "  ") or "") .. "|cff9d9d9dseen in combat|r" end
					r.tags:SetText(tags)
					r.desc:SetWidth(width - 48)
					r.desc:SetText(sp.desc)
					local h = 6 + r.name:GetStringHeight() + 2 + (tags ~= "" and r.tags:GetStringHeight() or 0) + 4 + r.desc:GetStringHeight() + 8
					r:SetHeight(math.max(40, h))
					y = y + r:GetHeight() + 2
				end
				y = y + 6
			end
		end
		scroll:SetContentHeight(y)
		if not units then
			empty:SetText("The abilities couldn't be loaded.")
			empty:Show()
		elseif not shownAny and #units == 0 then
			empty:SetText("This encounter has no creature the journal knows.")
			empty:Show()
		end
	end)
end

DJ.RegisterPane("abilities", pane)
