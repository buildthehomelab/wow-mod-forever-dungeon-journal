-- The Quests tab: every quest of the realm under the quest log's headings (zones by continent,
-- dungeons, raids, battlegrounds, classes, professions, world events and the rest), with your
-- progress on each. Pick a heading on the left for its quests on the right, chains in the order
-- they're done; the search box finds quests by name. Rows are Quests.lua's.

local J = Journal
local setShown = J.SetShown
local Q = J.QUEST

local LIST_WIDTH = 270
local ROW_HEIGHT = 20
local ROWS = 21

local GROUPS = {
	{ key = "E", name = "Eastern Kingdoms" },
	{ key = "K", name = "Kalimdor" },
	{ key = "O", name = "Outland" },
	{ key = "N", name = "Northrend" },
	{ key = "D", name = "Dungeons" },
	{ key = "R", name = "Raids" },
	{ key = "B", name = "Battlegrounds" },
	{ key = "C", name = "Classes" },
	{ key = "P", name = "Professions" },
	{ key = "W", name = "World Events" },
	{ key = "X", name = "Other" },
}

local function levelText(h)
	if not h.low or h.low == 0 then return "" end
	if h.low == h.high then return tostring(h.low) end
	return h.low .. "-" .. h.high
end

local function locked(h)
	return J.Has(J.HELLO_PROGRESSION) and (h.era or 0) > (J.state or 18)
end

local function progressText(done, countable)
	if not countable or countable == 0 then return "" end
	if done >= countable then return "|cff40ff40" .. done .. "/" .. countable .. "|r" end
	return done .. "/" .. countable
end

local function build(panel)
	local db = JournalCharDB.questTab or {}
	JournalCharDB.questTab = db
	db.collapsed = db.collapsed or {}

	local state = { headings = nil, selected = nil, entries = {} }

	local status = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	status:SetPoint("CENTER", panel, "CENTER", 0, 20)
	status:SetWidth(520)
	local statusSub = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	statusSub:SetPoint("TOP", status, "BOTTOM", 0, -10)
	statusSub:SetWidth(520)

	local body = CreateFrame("Frame", nil, panel)
	body:SetAllPoints(panel)

	-- left: search, filters, count, headings

	local left = CreateFrame("Frame", nil, body)
	left:SetPoint("TOPLEFT", body, "TOPLEFT", 0, 0)
	left:SetPoint("BOTTOMLEFT", body, "BOTTOMLEFT", 0, 0)
	left:SetWidth(LIST_WIDTH)

	local search = J.CreateEditBox(left, LIST_WIDTH - 104)
	search:SetPoint("TOPLEFT", left, "TOPLEFT", 8, -4)
	search.hint = search:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	search.hint:SetPoint("LEFT", search, "LEFT", 2, 0)
	search.hint:SetText("Search quests")
	search.hint:SetTextColor(0.6, 0.6, 0.6)

	local renderList, renderRight

	local filter = J.CreateDropdown(left, 90, function ()
		local function toggle(key)
			return function ()
				db[key] = not db[key] or nil
				renderList()
				renderRight()
			end
		end
		return {
			{ text = "Hide quests I've done", checked = db.hideDone and true or false, keepShownOnClick = true, func = toggle("hideDone") },
			{ text = "Only quests I can do now", checked = db.onlyAvailable and true or false, keepShownOnClick = true,
				func = toggle("onlyAvailable") },
			{ text = "Hide headings I've finished", checked = db.hideFinished and true or false, keepShownOnClick = true,
				func = toggle("hideFinished") },
		}
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

	local scrollName = J.UniqueName("QuestHeadingScroll")
	local scroll = CreateFrame("ScrollFrame", scrollName, listInset, "FauxScrollFrameTemplate")
	scroll:SetPoint("TOPLEFT", listInset, "TOPLEFT", 4, -4)
	scroll:SetPoint("BOTTOMRIGHT", listInset, "BOTTOMRIGHT", -26, 4)
	J.SkinScrollBar(scroll, scrollName)

	-- right: the heading's quests

	local right = J.CreateInset(body)
	right:SetPoint("TOPLEFT", left, "TOPRIGHT", 8, 0)
	right:SetPoint("BOTTOMRIGHT", body, "BOTTOMRIGHT", 0, 0)

	local title = right:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	title:SetPoint("TOPLEFT", right, "TOPLEFT", 12, -10)
	title:SetPoint("RIGHT", right, "RIGHT", -12, 0)
	title:SetJustifyH("LEFT")
	local subtitle = right:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	subtitle:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -4)
	subtitle:SetPoint("RIGHT", right, "RIGHT", -12, 0)
	subtitle:SetJustifyH("LEFT")

	local list = J.CreateQuestList(right, true)
	list.scroll:SetPoint("TOPLEFT", right, "TOPLEFT", 8, -44)
	list.scroll:SetPoint("BOTTOMRIGHT", right, "BOTTOMRIGHT", -30, 8)

	local empty = right:CreateFontString(nil, "OVERLAY", "GameFontDisable")
	empty:SetPoint("TOP", right, "TOP", 0, -90)
	empty:SetWidth(380)

	-----------------------------------------
	-- the headings list

	local function selectHeading(h)
		if state.selected ~= h then list:Reset() end
		state.selected = h
		db.heading = h and h.id or nil
		if search:GetText() ~= "" then
			search:SetText("")
			search:ClearFocus()
		end
		renderList()
		renderRight()
	end

	local rows = {}
	for i = 1, ROWS do
		local b = CreateFrame("Button", nil, listInset)
		b:SetHeight(ROW_HEIGHT)
		b:SetPoint("TOPLEFT", listInset, "TOPLEFT", 6, -6 - (i - 1) * ROW_HEIGHT)
		b:SetPoint("RIGHT", scroll, "RIGHT", 0, 0)
		b.expand = b:CreateTexture(nil, "ARTWORK")
		b.expand:SetSize(14, 14)
		b.expand:SetPoint("LEFT", b, "LEFT", 2, 0)
		b.lock = b:CreateTexture(nil, "ARTWORK")
		b.lock:SetTexture("Interface\\LFGFrame\\UI-LFG-ICON-LOCK")
		b.lock:SetSize(14, 14)
		b.right = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
		b.right:SetPoint("RIGHT", b, "RIGHT", -4, 0)
		b.right:SetJustifyH("RIGHT")
		b.name = b:CreateFontString(nil, "OVERLAY", "GameFontNormal")
		b.name:SetPoint("RIGHT", b.right, "LEFT", -6, 0)
		b.name:SetJustifyH("LEFT")
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
			if e.group then
				db.collapsed[e.group.key] = not db.collapsed[e.group.key] or nil
				renderList()
			else
				selectHeading(e.heading)
			end
		end)
		b:SetScript("OnEnter", function (self)
			local e = self.entry
			if not e or not e.heading then return end
			local h = e.heading
			GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
			GameTooltip:SetText(h.name, 1, 0.82, 0)
			if levelText(h) ~= "" then GameTooltip:AddLine("Level " .. levelText(h), 1, 1, 1) end
			GameTooltip:AddLine(h.total .. " quests" .. (h.countable > 0 and (", " .. h.done .. " of " .. h.countable .. " done") or ""),
				0.8, 0.8, 0.8)
			if h.total > h.countable then GameTooltip:AddLine("Repeatable quests, and quests you can no longer get, aren't counted.", 0.6, 0.6, 0.6, true) end
			if locked(h) then GameTooltip:AddLine("Opens at progression stage " .. h.era, 1, 0.33, 0.33) end
			GameTooltip:Show()
		end)
		b:SetScript("OnLeave", function () GameTooltip:Hide() end)
		rows[i] = b
	end

	local function finished(h)
		return h.countable > 0 and h.done >= h.countable
	end

	local function buildEntries()
		local out = {}
		local byGroup = {}
		for _, h in ipairs(state.headings.list) do
			byGroup[h.group] = byGroup[h.group] or {}
			table.insert(byGroup[h.group], h)
		end
		for _, g in ipairs(GROUPS) do
			local hs = byGroup[g.key]
			if hs then
				local entry = { group = g, done = 0, countable = 0 }
				table.insert(out, entry)
				for _, h in ipairs(hs) do
					entry.done = entry.done + h.done
					entry.countable = entry.countable + h.countable
				end
				if not db.collapsed[g.key] then
					for _, h in ipairs(hs) do
						if not (db.hideFinished and finished(h)) or h == state.selected then
							table.insert(out, { heading = h })
						end
					end
				end
			end
		end
		return out
	end

	local function renderRows()
		local offset = FauxScrollFrame_GetOffset(scroll)
		for i = 1, ROWS do
			local b = rows[i]
			local e = state.entries[offset + i]
			b.entry = e
			if not e then
				b:Hide()
			else
				b:Show()
				b.name:ClearAllPoints()
				b.lock:ClearAllPoints()
				if e.group then
					b.expand:SetTexture(db.collapsed[e.group.key] and "Interface\\Buttons\\UI-PlusButton-Up"
						or "Interface\\Buttons\\UI-MinusButton-Up")
					b.expand:Show()
					b.lock:Hide()
					b.name:SetPoint("LEFT", b, "LEFT", 20, 0)
					b.name:SetPoint("RIGHT", b.right, "LEFT", -6, 0)
					b.name:SetFontObject(GameFontNormal)
					b.name:SetText(e.group.name)
					b.name:SetTextColor(1, 0.82, 0)
					b.right:SetText(progressText(e.done, e.countable))
					b.selectedBg:Hide()
				else
					local h = e.heading
					b.expand:Hide()
					local isLocked = locked(h)
					setShown(b.lock, isLocked)
					if isLocked then
						b.lock:SetPoint("LEFT", b, "LEFT", 18, 0)
						b.name:SetPoint("LEFT", b.lock, "RIGHT", 2, 0)
					else
						b.name:SetPoint("LEFT", b, "LEFT", 20, 0)
					end
					b.name:SetPoint("RIGHT", b.right, "LEFT", -6, 0)
					b.name:SetFontObject(GameFontHighlightSmall)
					local lv = levelText(h)
					b.name:SetText(h.name .. (lv ~= "" and ("  |cff808080" .. lv .. "|r") or ""))
					if isLocked then
						b.name:SetTextColor(0.6, 0.6, 0.6)
					else
						b.name:SetTextColor(1, 1, 1)
					end
					b.right:SetText(progressText(h.done, h.countable))
					setShown(b.selectedBg, h == state.selected and search:GetText() == "")
				end
			end
		end
		FauxScrollFrame_Update(scroll, #state.entries, ROWS, ROW_HEIGHT)
	end

	scroll:SetScript("OnVerticalScroll", function (self, offset)
		FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, renderRows)
	end)

	renderList = function ()
		if not state.headings then return end
		state.entries = buildEntries()
		local done, countable = 0, 0
		for _, h in ipairs(state.headings.list) do
			done, countable = done + h.done, countable + h.countable
		end
		count:SetText(string.format("Quests done: |cffffffff%s|r / %s", J.Number(done), J.Number(countable)))
		renderRows()
	end

	-- Scrolls the headings list so the selected one shows.
	local function scrollToSelected()
		for i, e in ipairs(state.entries) do
			if e.heading and e.heading == state.selected then
				local offset = FauxScrollFrame_GetOffset(scroll)
				if i <= offset or i > offset + ROWS then
					local target = math.max(0, math.min(i - 3, #state.entries - ROWS))
					local bar = _G[scrollName .. "ScrollBar"]
					if bar then bar:SetValue(target * ROW_HEIGHT) end
					FauxScrollFrame_SetOffset(scroll, target)
					renderRows()
				end
				return
			end
		end
	end

	-----------------------------------------
	-- the quests

	local function filtered(quests)
		local out = {}
		for _, q in ipairs(quests) do
			local keep = true
			if db.hideDone and q.state == Q.DONE then keep = false end
			if db.onlyAvailable and q.state ~= Q.AVAILABLE and q.state ~= Q.ACTIVE and q.state ~= Q.COMPLETE then keep = false end
			if keep then table.insert(out, q) end
		end
		return out
	end

	local serial = 0
	local function show(quests, none)
		if not quests then
			list:Render({})
			empty:SetText("The quests couldn't be loaded. Try again in a moment.")
			empty:Show()
			return
		end
		local shown = filtered(quests)
		list:Render(shown)
		if #shown == 0 then
			empty:SetText(#quests == 0 and none or "No quests here match the filters.")
			empty:Show()
		else
			empty:Hide()
		end
	end

	renderRight = function ()
		serial = serial + 1
		local mine = serial
		local text = (search:GetText() or ""):match("^%s*(.-)%s*$")
		if #text >= 3 then
			title:SetText("Search: " .. text)
			title:SetTextColor(1, 0.82, 0)
			subtitle:SetText("Quests whose name has \"" .. text .. "\" in it (at most 50)")
			J.SearchQuests(text, function (quests)
				if mine ~= serial then return end
				for _, q in ipairs(quests or {}) do
					local h = state.headings and state.headings.byId[q.heading]
					q.note = h and h.name or nil
				end
				show(quests, "No quest has that in its name.")
			end)
			return
		end
		local h = state.selected
		if not h then
			title:SetText("")
			subtitle:SetText("")
			list:Render({})
			empty:SetText("Pick a zone or a heading on the left.")
			empty:Show()
			return
		end
		title:SetText(h.name)
		if locked(h) then title:SetTextColor(0.6, 0.6, 0.6) else title:SetTextColor(1, 0.82, 0) end
		local bits = {}
		if levelText(h) ~= "" then table.insert(bits, "Level " .. levelText(h)) end
		table.insert(bits, h.total .. " quests")
		if h.countable > 0 then table.insert(bits, h.done .. " of " .. h.countable .. " done") end
		if locked(h) then table.insert(bits, "|cffff5555Opens at progression stage " .. h.era .. "|r") end
		subtitle:SetText(table.concat(bits, " - "))
		empty:SetText("Loading...")
		empty:Show()
		J.LoadHeadingQuests(h.id, function (quests)
			if mine ~= serial or state.selected ~= h then return end
			if quests and quests.total and quests.total > #quests then
				subtitle:SetText(subtitle:GetText() .. " - |cffff8020showing the first " .. #quests .. "; search for the rest|r")
			end
			show(quests, "No quests here for you.")
		end)
	end

	-----------------------------------------
	-- loading

	local function showProblem(problem, sub)
		body:Hide()
		status:SetText(problem)
		statusSub:SetText(sub or "")
		status:Show()
		statusSub:Show()
	end

	local function load(force)
		local problem, sub = J.ServerStatus()
		if not problem and not J.Has(J.HELLO_QUESTS) then
			problem, sub = "This realm doesn't list its quests yet",
				"The realm's journal module has its Quests tab turned off (Journal.Quests.Enable), or is older than this addon."
		end
		if problem then showProblem(problem, sub) return end
		if not state.headings then showProblem("Loading quests...") end
		J.LoadQuestHeadings(function (out)
			if not out then
				if not state.headings then showProblem("The quests couldn't be loaded", "Try again in a moment with /journal refresh.") end
				return
			end
			status:Hide()
			statusSub:Hide()
			body:Show()
			local first = not state.headings
			state.headings = out
			-- Where you are, when that changed since you last looked; else what you had open.
			local pick
			if out.here ~= 0 and out.here ~= db.lastHere and out.byId[out.here] then
				pick = out.byId[out.here]
				db.lastHere = out.here
			end
			pick = pick or (state.selected and out.byId[state.selected.id]) or (db.heading and out.byId[db.heading])
				or out.byId[out.here] or out.list[1]
			if pick and state.selected and pick.id ~= state.selected.id then list:Reset() end
			state.selected = pick
			db.heading = pick and pick.id or nil
			if pick and (first or pick.id == out.here) then db.collapsed[pick.group] = nil end
			renderList()
			if first then scrollToSelected() end
			renderRight()
		end, force)
	end

	local lastText = ""
	search:SetScript("OnTextChanged", function (self)
		local text = self:GetText() or ""
		setShown(self.hint, text == "" and not self:HasFocus())
		if text == lastText then return end
		lastText = text
		J.Debounce("quest-search", 0.4, function ()
			if search:GetText() ~= text then return end
			list:Reset()
			renderRows()
			renderRight()
		end)
	end)
	search:SetScript("OnEditFocusGained", function (self) self.hint:Hide() end)
	search:SetScript("OnEditFocusLost", function (self) setShown(self.hint, (self:GetText() or "") == "") end)
	search:SetScript("OnEscapePressed", function (self)
		self:SetText("")
		self:ClearFocus()
	end)

	local function shown() return panel:IsVisible() end
	-- Quest log changes come often (every kill of a quest's creature): asked again a little later.
	J.On("QUESTS_STALE", function () if shown() then J.Debounce("quest-tab", 5.0, function () if shown() then load() end end) end end)
	J.On("SECTION_SERVER", function () if shown() then load() end end)

	return load
end

local load
J.AddSection({
	key = "quests", name = "Quests", order = 25, icon = "Interface\\Icons\\INV_Misc_Book_08",
	build = function (panel) load = build(panel) end,
	onShow = function () if load then load() end end,
})
