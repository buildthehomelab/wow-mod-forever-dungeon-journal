-- The Lore tab: a codex of each zone's places, people and history that fills in as you play.
-- Zones on the left, by continent, with how much of each you've found; the zone's entries in the
-- middle (undiscovered ones say how they're found); the entry you picked on the right. The
-- Library, at the top of the zones, keeps every book, plaque and letter you've read to read again.

local J = Journal
local setShown = J.SetShown
local LORE = J.LORE

local ZONE_WIDTH = 220
local ENTRY_WIDTH = 250
local ROW_HEIGHT = 20
local ZONE_ROWS = 22
local ENTRY_ROWS = 23

local GROUPS = {
	{ key = "E", name = "Eastern Kingdoms" },
	{ key = "K", name = "Kalimdor" },
	{ key = "O", name = "Outland" },
	{ key = "N", name = "Northrend" },
	{ key = "D", name = "Dungeons" },
	{ key = "R", name = "Raids" },
	{ key = "B", name = "Battlegrounds" },
	{ key = "X", name = "Other" },
}

-- Stands in the zones list for the Library; its "entries" are your books.
local LIBRARY = { id = "library", library = true, name = "Library", known = 0, total = 0 }

local function eraLocked(era)
	return J.Has(J.HELLO_PROGRESSION) and (era or 0) > (J.state or 18)
end

local function progressText(known, total)
	if not total or total == 0 then return "" end
	if known >= total then return "|cff40ff40" .. known .. "/" .. total .. "|r" end
	return known .. "/" .. total
end

-- A list of rows in a faux scroll frame: rows get .entry, render(b, e) draws one.
local function makeList(parent, nameBase, count, onClick, onEnter, render)
	local scrollName = J.UniqueName(nameBase)
	local scroll = CreateFrame("ScrollFrame", scrollName, parent, "FauxScrollFrameTemplate")
	scroll:SetPoint("TOPLEFT", parent, "TOPLEFT", 4, -4)
	scroll:SetPoint("BOTTOMRIGHT", parent, "BOTTOMRIGHT", -26, 4)
	J.SkinScrollBar(scroll, scrollName)

	local list = { scroll = scroll, rows = {}, entries = {}, scrollName = scrollName }
	for i = 1, count do
		local b = CreateFrame("Button", nil, parent)
		b:SetHeight(ROW_HEIGHT)
		b:SetPoint("TOPLEFT", parent, "TOPLEFT", 6, -6 - (i - 1) * ROW_HEIGHT)
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
		b:SetScript("OnClick", function (self) if self.entry then onClick(self.entry) end end)
		if onEnter then
			b:SetScript("OnEnter", function (self) if self.entry then onEnter(self, self.entry) end end)
			b:SetScript("OnLeave", function () GameTooltip:Hide() end)
		end
		list.rows[i] = b
	end

	function list:Render()
		local offset = FauxScrollFrame_GetOffset(self.scroll)
		for i = 1, count do
			local b = self.rows[i]
			local e = self.entries[offset + i]
			b.entry = e
			if not e then
				b:Hide()
			else
				b:Show()
				b.name:ClearAllPoints()
				b.lock:ClearAllPoints()
				b.name:SetPoint("RIGHT", b.right, "LEFT", -6, 0)
				render(b, e)
			end
		end
		FauxScrollFrame_Update(self.scroll, #self.entries, count, ROW_HEIGHT)
	end

	-- Scrolls so the i-th entry shows.
	function list:Reveal(i)
		local offset = FauxScrollFrame_GetOffset(self.scroll)
		if i > offset and i <= offset + count then return end
		local target = math.max(0, math.min(i - 3, #self.entries - count))
		local bar = _G[self.scrollName .. "ScrollBar"]
		if bar then bar:SetValue(target * ROW_HEIGHT) end
		FauxScrollFrame_SetOffset(self.scroll, target)
		self:Render()
	end

	scroll:SetScript("OnVerticalScroll", function (self, offset)
		FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, function () list:Render() end)
	end)
	return list
end

local function build(panel)
	local db = JournalCharDB.loreTab or {}
	JournalCharDB.loreTab = db
	db.collapsed = db.collapsed or {}

	local state = { zones = nil, zone = nil, entries = nil, entry = nil }

	local status = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	status:SetPoint("CENTER", panel, "CENTER", 0, 20)
	status:SetWidth(520)
	local statusSub = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	statusSub:SetPoint("TOP", status, "BOTTOM", 0, -10)
	statusSub:SetWidth(520)

	local body = CreateFrame("Frame", nil, panel)
	body:SetAllPoints(panel)

	-- left: the zones

	local left = CreateFrame("Frame", nil, body)
	left:SetPoint("TOPLEFT", body, "TOPLEFT", 0, 0)
	left:SetPoint("BOTTOMLEFT", body, "BOTTOMLEFT", 0, 0)
	left:SetWidth(ZONE_WIDTH)

	local count = left:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	count:SetPoint("TOPLEFT", left, "TOPLEFT", 8, -8)
	count:SetPoint("RIGHT", left, "RIGHT", -8, 0)
	count:SetJustifyH("LEFT")

	local zoneInset = J.CreateInset(left)
	zoneInset:SetPoint("TOPLEFT", left, "TOPLEFT", 0, -24)
	zoneInset:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", 0, 0)

	-- middle: the zone's entries

	local middle = J.CreateInset(body)
	middle:SetPoint("TOPLEFT", left, "TOPRIGHT", 8, 0)
	middle:SetPoint("BOTTOMLEFT", left, "BOTTOMRIGHT", 8, 0)
	middle:SetWidth(ENTRY_WIDTH)

	-- right: the entry

	local right = J.CreateInset(body)
	right:SetPoint("TOPLEFT", middle, "TOPRIGHT", 8, 0)
	right:SetPoint("BOTTOMRIGHT", body, "BOTTOMRIGHT", 0, 0)

	local title = right:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	title:SetPoint("TOPLEFT", right, "TOPLEFT", 14, -12)
	title:SetPoint("RIGHT", right, "RIGHT", -14, 0)
	title:SetJustifyH("LEFT")
	local subtitle = right:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	subtitle:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -4)
	subtitle:SetPoint("RIGHT", right, "RIGHT", -14, 0)
	subtitle:SetJustifyH("LEFT")
	subtitle:SetTextColor(0.7, 0.7, 0.7)

	local reader = J.CreateScrollArea(right)
	reader:SetPoint("TOPLEFT", right, "TOPLEFT", 12, -50)
	reader:SetPoint("BOTTOMRIGHT", right, "BOTTOMRIGHT", -30, 10)
	local text = reader.content:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	text:SetPoint("TOPLEFT", reader.content, "TOPLEFT", 2, -2)
	text:SetPoint("RIGHT", reader.content, "RIGHT", -2, 0)
	text:SetJustifyH("LEFT")
	text:SetJustifyV("TOP")
	text:SetSpacing(3)

	local placeholder = right:CreateFontString(nil, "OVERLAY", "GameFontDisable")
	placeholder:SetPoint("TOP", right, "TOP", 0, -110)
	placeholder:SetWidth(320)

	local renderZones, renderEntries, showEntry

	-----------------------------------------
	-- the zones list

	local zones
	local function selectZone(z)
		state.zone = z
		db.zone = z and z.id or nil
		renderZones()
		renderEntries()
	end

	zones = makeList(zoneInset, "LoreZoneScroll", ZONE_ROWS,
		function (e)
			if e.group then
				db.collapsed[e.group.key] = not db.collapsed[e.group.key] or nil
				renderZones()
			else
				selectZone(e.zone)
			end
		end,
		function (b, e)
			if not e.zone then return end
			local z = e.zone
			GameTooltip:SetOwner(b, "ANCHOR_RIGHT")
			GameTooltip:SetText(z.name, 1, 0.82, 0)
			if z.library then
				GameTooltip:AddLine("Every book, plaque and letter you've read in the world, to read again.", 1, 1, 1, true)
				GameTooltip:Show()
				return
			end
			GameTooltip:AddLine(z.known .. " of " .. z.total .. " entries found", 1, 1, 1)
			if eraLocked(z.era) then GameTooltip:AddLine("Opens at progression stage " .. z.era, 1, 0.33, 0.33) end
			GameTooltip:Show()
		end,
		function (b, e)
			if e.group then
				b.expand:SetTexture(db.collapsed[e.group.key] and "Interface\\Buttons\\UI-PlusButton-Up" or "Interface\\Buttons\\UI-MinusButton-Up")
				b.expand:Show()
				b.lock:Hide()
				b.name:SetPoint("LEFT", b, "LEFT", 20, 0)
				b.name:SetFontObject(GameFontNormal)
				b.name:SetText(e.group.name)
				b.name:SetTextColor(1, 0.82, 0)
				b.right:SetText(progressText(e.known, e.total))
				b.selectedBg:Hide()
			elseif e.zone.library then
				b.expand:Hide()
				b.lock:Hide()
				b.name:SetPoint("LEFT", b, "LEFT", 4, 0)
				b.name:SetFontObject(GameFontNormal)
				b.name:SetText("|TInterface\\Icons\\INV_Misc_Book_09:16:16:0:0|t Library")
				b.name:SetTextColor(1, 0.82, 0)
				b.right:SetText(LIBRARY.known > 0 and tostring(LIBRARY.known) or "")
				setShown(b.selectedBg, state.zone == LIBRARY)
			else
				local z = e.zone
				b.expand:Hide()
				local locked = eraLocked(z.era)
				setShown(b.lock, locked)
				if locked then
					b.lock:SetPoint("LEFT", b, "LEFT", 18, 0)
					b.name:SetPoint("LEFT", b.lock, "RIGHT", 2, 0)
					b.name:SetTextColor(0.6, 0.6, 0.6)
				else
					b.name:SetPoint("LEFT", b, "LEFT", 20, 0)
					b.name:SetTextColor(1, 1, 1)
				end
				b.name:SetFontObject(GameFontHighlightSmall)
				b.name:SetText(z.name)
				b.right:SetText(progressText(z.known, z.total))
				setShown(b.selectedBg, z == state.zone)
			end
		end)

	renderZones = function ()
		if not state.zones then return end
		local out, byGroup = {}, {}
		if J.Has(J.HELLO_LIBRARY) then table.insert(out, { zone = LIBRARY }) end
		local known = {}
		for _, g in ipairs(GROUPS) do known[g.key] = true end
		for _, z in ipairs(state.zones.list) do
			local key = known[z.group] and z.group or "X"
			byGroup[key] = byGroup[key] or {}
			table.insert(byGroup[key], z)
		end
		for _, g in ipairs(GROUPS) do
			local list = byGroup[g.key]
			if list then
				table.sort(list, function (a, b) return a.name < b.name end)
				local entry = { group = g, known = 0, total = 0 }
				table.insert(out, entry)
				for _, z in ipairs(list) do
					entry.known = entry.known + z.known
					entry.total = entry.total + z.total
				end
				if not db.collapsed[g.key] then
					for _, z in ipairs(list) do table.insert(out, { zone = z }) end
				end
			end
		end
		zones.entries = out
		count:SetText(string.format("Lore found: |cffffffff%s|r / %s", J.Number(state.zones.known), J.Number(state.zones.total)))
		zones:Render()
	end

	local function revealZone()
		for i, e in ipairs(zones.entries) do
			if e.zone and e.zone == state.zone then zones:Reveal(i) return end
		end
	end

	-----------------------------------------
	-- the entries list

	local entries = makeList(middle, "LoreEntryScroll", ENTRY_ROWS,
		function (e)
			if e.entry then showEntry(e.entry) end
			if e.book then showEntry(e.book) end
		end,
		function (b, e)
			if e.book then
				GameTooltip:SetOwner(b, "ANCHOR_RIGHT")
				GameTooltip:SetText(e.book.title, 1, 0.82, 0)
				if e.book.where then GameTooltip:AddLine("Read in " .. e.book.where, 1, 1, 1) end
				GameTooltip:Show()
				return
			end
			if not e.entry then return end
			local entry = e.entry
			GameTooltip:SetOwner(b, "ANCHOR_RIGHT")
			GameTooltip:SetText(entry.title, 1, 0.82, 0)
			if entry.state == LORE.UNKNOWN then
				if eraLocked(entry.era) then
					GameTooltip:AddLine("Opens at progression stage " .. entry.era, 1, 0.33, 0.33)
				elseif entry.hint then
					GameTooltip:AddLine(entry.hint, 1, 1, 1, true)
				end
			elseif J.LoreIsNew(entry) then
				GameTooltip:AddLine("New", 0.25, 1, 0.25)
			end
			GameTooltip:Show()
		end,
		function (b, e)
			b.expand:Hide()
			if e.book then
				b.lock:Hide()
				b.name:SetPoint("LEFT", b, "LEFT", 6, 0)
				b.name:SetFontObject(GameFontHighlightSmall)
				b.name:SetText(e.book.title)
				b.name:SetTextColor(1, 1, 1)
				b.right:SetText(J.LoreIsNew(e.book) and "|cff40ff40New|r" or "")
				setShown(b.selectedBg, state.entry and state.entry.book and state.entry.id == e.book.id)
				return
			end
			if e.kind then
				b.lock:Hide()
				b.name:SetPoint("LEFT", b, "LEFT", 4, 0)
				b.name:SetFontObject(GameFontNormal)
				b.name:SetText(J.LORE_KINDS[e.kind] or "")
				b.name:SetTextColor(1, 0.82, 0)
				b.right:SetText(progressText(e.known, e.total))
				b.selectedBg:Hide()
				return
			end
			local entry = e.entry
			local unknown = entry.state == LORE.UNKNOWN
			local locked = unknown and eraLocked(entry.era)
			setShown(b.lock, locked)
			if locked then
				b.lock:SetPoint("LEFT", b, "LEFT", 12, 0)
				b.name:SetPoint("LEFT", b.lock, "RIGHT", 2, 0)
			else
				b.name:SetPoint("LEFT", b, "LEFT", 14, 0)
			end
			b.name:SetFontObject(GameFontHighlightSmall)
			b.name:SetText(entry.title)
			if unknown then b.name:SetTextColor(0.5, 0.5, 0.5) else b.name:SetTextColor(1, 1, 1) end
			b.right:SetText(J.LoreIsNew(entry) and "|cff40ff40New|r" or "")
			setShown(b.selectedBg, state.entry and not state.entry.book and state.entry.id == entry.id)
		end)

	local function fillEntries()
		local out = {}
		if state.zone == LIBRARY then
			for _, book in ipairs(state.entries or {}) do table.insert(out, { book = book }) end
			entries.entries = out
			entries:Render()
			return
		end
		local byKind = {}
		for _, entry in ipairs(state.entries or {}) do
			byKind[entry.kind] = byKind[entry.kind] or {}
			table.insert(byKind[entry.kind], entry)
		end
		for kind = 0, 3 do
			local list = byKind[kind]
			if list then
				local header = { kind = kind, known = 0, total = #list }
				for _, entry in ipairs(list) do
					if entry.state ~= LORE.UNKNOWN then header.known = header.known + 1 end
				end
				-- The zone's overview needs no heading of its own.
				if kind ~= 0 then table.insert(out, header) end
				for _, entry in ipairs(list) do table.insert(out, { entry = entry }) end
			end
		end
		entries.entries = out
		entries:Render()
	end

	local function clearReader(message)
		title:SetText("")
		subtitle:SetText("")
		text:SetText("")
		reader:SetContentHeight(1)
		placeholder:SetText(message or "")
		setShown(placeholder, message ~= nil)
	end

	local serial = 0
	showEntry = function (entry)
		state.entry = entry
		if entry and entry.book then db.book = entry.id else db.entry = entry and entry.id or nil end
		entries:Render()
		serial = serial + 1
		local mine = serial
		if not entry then
			clearReader("Pick an entry.")
			return
		end
		if entry.book then
			title:SetText(entry.title)
			title:SetTextColor(1, 0.82, 0)
			subtitle:SetText(entry.where and ("Read in " .. entry.where) or "")
			placeholder:Hide()
			text:SetText("Loading...")
			J.LoadBook(entry.id, function (book)
				if mine ~= serial then return end
				if not book then
					text:SetText("The book couldn't be loaded. Try again in a moment.")
					return
				end
				local parts = {}
				for i, page in ipairs(book.pages) do
					if #book.pages > 1 then table.insert(parts, "|cffffd200Page " .. i .. "|r") end
					table.insert(parts, page)
				end
				text:SetText(table.concat(parts, "\n\n"))
				reader:ScrollToTop()
				reader:SetContentHeight(text:GetStringHeight() + 8)
				if J.LoreIsNew(entry) then
					J.LoreMarkRead(entry.readKey)
					entries:Render()
				end
			end)
			return
		end
		local zone = state.zone and state.zone.name or ""
		local kind = entry.kind == 0 and "Overview" or (J.LORE_KINDS[entry.kind] or "")
		if entry.state == LORE.UNKNOWN then
			clearReader()
			title:SetText(entry.title)
			title:SetTextColor(0.6, 0.6, 0.6)
			subtitle:SetText(zone .. " - " .. kind)
			if eraLocked(entry.era) then
				placeholder:SetText("Undiscovered.\n\nThis part of the world's story opens at progression stage " .. entry.era .. ".")
			else
				placeholder:SetText("Undiscovered.\n\n" .. (entry.hint or "Keep exploring") .. " to learn more.")
			end
			placeholder:Show()
			return
		end
		title:SetText(entry.title)
		title:SetTextColor(1, 0.82, 0)
		subtitle:SetText(zone .. " - " .. kind)
		placeholder:Hide()
		text:SetText("Loading...")
		J.LoadLoreEntry(entry.id, function (full)
			if mine ~= serial then return end
			if not full then
				text:SetText("The entry couldn't be loaded. Try again in a moment.")
				return
			end
			text:SetText(full.text)
			reader:ScrollToTop()
			reader:SetContentHeight(text:GetStringHeight() + 8)
			if J.LoreIsNew(entry) then
				J.LoreMarkRead(entry.id)
				entries:Render()
			end
		end)
	end

	renderEntries = function ()
		local z = state.zone
		serial = serial + 1
		if not z then
			state.entries = nil
			state.entry = nil
			fillEntries()
			clearReader("Pick a zone on the left.")
			return
		end
		if z == LIBRARY then
			J.LoadLibrary(function (list)
				if state.zone ~= z then return end
				state.entries = list or {}
				LIBRARY.known = #state.entries
				for _, book in ipairs(state.entries) do
					book.book = true
					-- New until opened, like lore found while playing (books share the read list).
					book.readKey = "b" .. book.id
					book.state = J.LORE.FOUND
				end
				fillEntries()
				zones:Render()
				if #state.entries == 0 then
					state.entry = nil
					clearReader("Your library is empty.\n\nBooks, plaques, tablets and letters you read in the world are kept "
						.. "here, so you can read them again whenever you like.")
					return
				end
				local want = (J.loreTarget and J.loreTarget.zone == "library" and J.loreTarget.entry) or db.book
				J.loreTarget = nil
				local pick
				for _, book in ipairs(state.entries) do
					if book.id == want then pick = book end
				end
				pick = pick or state.entries[1]
				local shown = state.entry
				if shown and shown.book and pick.id == shown.id then
					state.entry = pick
					entries:Render()
					return
				end
				for i, e in ipairs(entries.entries) do
					if e.book == pick then entries:Reveal(i) end
				end
				showEntry(pick)
			end)
			return
		end
		J.LoadLoreZone(z.id, function (list)
			if state.zone ~= z then return end
			state.entries = list or {}
			fillEntries()
			-- What you asked to see, else what you had open, else the zone's overview.
			local want = (J.loreTarget and J.loreTarget.zone == z.id and J.loreTarget.entry) or db.entry
			J.loreTarget = nil
			local pick
			for _, entry in ipairs(state.entries) do
				if entry.id == want then pick = entry end
			end
			pick = pick or state.entries[1]
			-- The entry already being read (the list was asked again after a discovery): stay where
			-- the reader is.
			local shown = state.entry
			if pick and shown and not shown.book and pick.id == shown.id and pick.state == shown.state then
				state.entry = pick
				entries:Render()
				return
			end
			for i, e in ipairs(entries.entries) do
				if e.entry and e.entry == pick then entries:Reveal(i) end
			end
			showEntry(pick)
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
		if not problem and not J.Has(J.HELLO_LORE) then
			problem, sub = "This realm has no lore yet",
				"The realm's journal module has its Lore tab turned off (Journal.Lore.Enable), or is older than this addon."
		end
		if problem then showProblem(problem, sub) return end
		if not state.zones then showProblem("Loading lore...") end
		J.LoadLoreZones(function (out)
			if not out then
				if not state.zones then showProblem("The lore couldn't be loaded", "Try again in a moment with /journal refresh.") end
				return
			end
			status:Hide()
			statusSub:Hide()
			body:Show()
			if J.Has(J.HELLO_LIBRARY) then out.byId.library = LIBRARY end
			if #out.list == 0 and not out.byId.library then
				showProblem("Nothing to discover yet", "This realm's journal has no lore your character can find.")
				return
			end
			local first = not state.zones
			state.zones = out
			-- A zone asked for (the banner), else where you are when that changed, else what you had open.
			local pick
			if J.loreTarget then pick = out.byId[J.loreTarget.zone] end
			if not pick and out.here ~= 0 and out.here ~= db.lastHere and out.byId[out.here] then
				pick = out.byId[out.here]
				db.lastHere = out.here
			end
			pick = pick or (state.zone and out.byId[state.zone.id]) or (db.zone and out.byId[db.zone]) or out.byId[out.here]
				or out.list[1]
			state.zone = pick
			db.zone = pick and pick.id or nil
			if pick and pick.group then db.collapsed[pick.group] = nil end
			renderZones()
			if first or J.loreTarget then revealZone() end
			renderEntries()
		end, force)
	end

	local function shown() return panel:IsVisible() end
	J.On("LORE_FOUND", function () if shown() then J.Debounce("lore-tab", 1.0, function () if shown() then load() end end) end end)
	J.On("BOOK_FOUND", function () if shown() then J.Debounce("lore-tab", 1.0, function () if shown() then load() end end) end end)
	J.On("LORE_TARGET", function () if shown() then load() end end)
	J.On("SECTION_SERVER", function () if shown() then load() end end)

	return load
end

local load
J.AddSection({
	key = "lore", name = "Lore", order = 27, icon = "Interface\\Icons\\INV_Misc_Book_11",
	build = function (panel) load = build(panel) end,
	onShow = function () if load then load() end end,
})
