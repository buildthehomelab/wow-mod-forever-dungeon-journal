-- The Directory tab: a town guide. Pick your class trainer, a profession or a service on the left
-- (or search by name, title or zone) for every friendly one on the realm on the right, nearest
-- first; click one to flag it on your map. The Town Guide spell and .find open this tab.

local J = Journal
local setShown = J.SetShown
local Unescape = J.Unescape

local LIST_WIDTH = 220
local ROW_HEIGHT = 20
local ROWS = 21
local RESULT_HEIGHT = 34
local RESULTS = 12

-- The numbers are the server's categories (src/JournalDirectory.cpp).
local CLASS_TRAINER = 1
local MAIN = {
	{ id = 5, name = "Flight master" },
	{ id = 6, name = "Innkeeper" },
	{ id = 7, name = "Bank" },
	{ id = 8, name = "Auction house" },
	{ id = 9, name = "Mailbox" },
	{ id = 10, name = "Repairs" },
}
local MORE = {
	{ id = 2, name = "Riding trainer" },
	{ id = 3, name = "Weapon master" },
	{ id = 4, name = "Pet trainer", class = "HUNTER" },
	{ id = 11, name = "Stable master", class = "HUNTER" },
	{ id = 12, name = "Guild master" },
	{ id = 13, name = "Tabard designer" },
	{ id = 14, name = "Guild vault" },
	{ id = 15, name = "Battlemaster" },
	{ id = 16, name = "Barber" },
	{ id = 17, name = "Reagents" },
	{ id = 18, name = "Food and drink" },
	{ id = 19, name = "General goods" },
	{ id = 20, name = "Poisons", class = "ROGUE" },
}
local PROFESSIONS = {
	{ skill = 171, name = "Alchemy" }, { skill = 164, name = "Blacksmithing" }, { skill = 333, name = "Enchanting" },
	{ skill = 202, name = "Engineering" }, { skill = 182, name = "Herbalism" }, { skill = 773, name = "Inscription" },
	{ skill = 755, name = "Jewelcrafting" }, { skill = 165, name = "Leatherworking" }, { skill = 186, name = "Mining" },
	{ skill = 393, name = "Skinning" }, { skill = 197, name = "Tailoring" }, { skill = 185, name = "Cooking" },
	{ skill = 129, name = "First Aid" }, { skill = 356, name = "Fishing" },
}
local RANKS = { "Apprentice", "Journeyman", "Expert", "Artisan", "Master", "Grand Master" }
local DIRECTIONS = { "north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west" }

-----------------------------------------
-- talking to the server

-- done(places, found) with the rows of an N answer, nil when it failed. Places aren't cached:
-- the distances are from where you stand.
local function fetch(fields, done)
	if not J.Has(J.HELLO_DIRECTORY) then done(nil) return end
	J.Request("N", fields, function (result)
		if not result then done(nil) return end
		local out = {}
		for _, r in ipairs(result.rows) do
			table.insert(out, {
				place = r[1], kind = r[2], id = r[3],
				name = Unescape(tostring(r[4] or "-")), title = Unescape(tostring(r[5] or "-")),
				zone = Unescape(tostring(r[6] or "-")), area = Unescape(tostring(r[7] or "-")),
				x = tonumber(r[8]) or -1, y = tonumber(r[9]) or -1,
				continent = Unescape(tostring(r[10] or "-")),
				yards = tonumber(r[11]) or -1, direction = tonumber(r[12]) or 8, rank = tonumber(r[13]) or 0,
			})
		end
		done(out, tonumber(result.meta[2]) or #out)
	end)
end

function J.DirectoryCategory(id, done) fetch({ "C", tostring(id) }, done) end
function J.DirectoryProfession(skill, done) fetch({ "P", tostring(skill) }, done) end

-- The first n bytes of a text, never ending inside a UTF-8 character.
local function cut(text, n)
	if #text <= n then return text end
	while n > 0 do
		local nextByte = text:byte(n + 1)
		if nextByte < 128 or nextByte >= 192 then break end
		n = n - 1
	end
	return text:sub(1, n)
end

-- The words the server matches (every one, against name, title and zone); nil when there's
-- nothing worth asking for.
local function searchText(text)
	text = cut((text or ""):gsub("[:;|]", " "):match("^%s*(.-)%s*$"), 64):match("^(.-)%s*$")
	for word in text:gmatch("%S+") do
		if #word > 1 then return text end
	end
	return nil
end

function J.DirectorySearch(text, done)
	text = searchText(text)
	if not text then done(nil) return end
	fetch({ "S", text }, done)
end

-- done({ [skill] = rank name }) for the professions the character knows; the server reads the
-- skills, so the names don't have to match the client's language.
function J.DirectoryRanks(done)
	if not J.Has(J.HELLO_DIRECTORY) then done(nil) return end
	J.Request("N", { "R" }, function (result)
		if not result then done(nil) return end
		local out = {}
		for _, r in ipairs(result.rows) do
			local skill, max = tonumber(r[1]), tonumber(r[2])
			if skill and max and max > 0 then out[skill] = RANKS[math.max(1, math.min(6, math.floor(max / 75)))] end
		end
		done(out)
	end)
end

-- A red flag on the map; done(the server's line) or done(nil).
function J.ShowDirectoryPlace(p, done)
	J.Request("M", { "N", tostring(p.place), tostring(p.id) }, function (result)
		done(result and result[1] == "ok" and Unescape(result[2] or "") or nil)
	end)
end

-----------------------------------------

local function whereText(p)
	local where = p.zone
	if p.area ~= "" and p.area ~= p.zone then where = (where ~= "" and (where .. ", ") or "") .. p.area end
	if p.x >= 0 and p.y >= 0 then where = where .. string.format(" (%d, %d)", math.floor(p.x / 10 + 0.5), math.floor(p.y / 10 + 0.5)) end
	return where
end

local function distanceText(p)
	if p.yards >= 0 then
		return p.yards .. " yd" .. (DIRECTIONS[p.direction + 1] and (" " .. DIRECTIONS[p.direction + 1]) or "")
	end
	return p.continent ~= "" and p.continent or "elsewhere"
end

local function build(panel)
	local db = JournalCharDB.directory or {}
	JournalCharDB.directory = db
	db.collapsed = db.collapsed or { professions = true, more = true }

	local _, playerClass = UnitClass("player")
	local className = (UnitClass("player"))
	local state = { selected = nil, entries = {}, places = {}, picked = nil, ranks = {} }
	local lastText = "" -- the search text last acted on

	local status = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	status:SetPoint("CENTER", panel, "CENTER", 0, 20)
	status:SetWidth(520)
	local statusSub = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
	statusSub:SetPoint("TOP", status, "BOTTOM", 0, -10)
	statusSub:SetWidth(520)

	local body = CreateFrame("Frame", nil, panel)
	body:SetAllPoints(panel)

	-----------------------------------------
	-- left: search and what to look for

	local left = CreateFrame("Frame", nil, body)
	left:SetPoint("TOPLEFT", body, "TOPLEFT", 0, 0)
	left:SetPoint("BOTTOMLEFT", body, "BOTTOMLEFT", 0, 0)
	left:SetWidth(LIST_WIDTH)

	local search = J.CreateEditBox(left, LIST_WIDTH - 16)
	search:SetPoint("TOPLEFT", left, "TOPLEFT", 8, -4)
	search.hint = search:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	search.hint:SetPoint("LEFT", search, "LEFT", 2, 0)
	search.hint:SetText("Search name, title or zone")
	search.hint:SetTextColor(0.6, 0.6, 0.6)

	local listInset = J.CreateInset(left)
	listInset:SetPoint("TOPLEFT", left, "TOPLEFT", 0, -30)
	listInset:SetPoint("BOTTOMRIGHT", left, "BOTTOMRIGHT", 0, 0)

	local scrollName = J.UniqueName("DirectoryListScroll")
	local scroll = CreateFrame("ScrollFrame", scrollName, listInset, "FauxScrollFrameTemplate")
	scroll:SetPoint("TOPLEFT", listInset, "TOPLEFT", 4, -4)
	scroll:SetPoint("BOTTOMRIGHT", listInset, "BOTTOMRIGHT", -26, 4)
	J.SkinScrollBar(scroll, scrollName)

	-----------------------------------------
	-- right: the places

	local right = J.CreateInset(body)
	right:SetPoint("TOPLEFT", left, "TOPRIGHT", 8, 0)
	right:SetPoint("BOTTOMRIGHT", body, "BOTTOMRIGHT", 0, 0)

	local title = right:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
	title:SetPoint("TOPLEFT", right, "TOPLEFT", 12, -10)
	title:SetPoint("RIGHT", right, "RIGHT", -100, 0)
	title:SetJustifyH("LEFT")
	local subtitle = right:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	subtitle:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -4)
	subtitle:SetPoint("RIGHT", right, "RIGHT", -12, 0)
	subtitle:SetJustifyH("LEFT")

	local refresh = J.CreateButton(right, "Refresh", 80, 22)
	refresh:SetPoint("TOPRIGHT", right, "TOPRIGHT", -10, -8)

	local resultScrollName = J.UniqueName("DirectoryResultScroll")
	local resultScroll = CreateFrame("ScrollFrame", resultScrollName, right, "FauxScrollFrameTemplate")
	resultScroll:SetPoint("TOPLEFT", right, "TOPLEFT", 8, -44)
	resultScroll:SetPoint("BOTTOMRIGHT", right, "BOTTOMRIGHT", -30, 8)
	J.SkinScrollBar(resultScroll, resultScrollName)

	local empty = right:CreateFontString(nil, "OVERLAY", "GameFontDisable")
	empty:SetPoint("TOP", right, "TOP", 0, -90)
	empty:SetWidth(380)

	local renderList, renderRight, renderResults

	-----------------------------------------
	-- the list on the left

	local function selectEntry(e)
		state.selected = e
		db.selected = e and e.key or nil
		if search:GetText() ~= "" then
			-- SetText fires OnTextChanged at once: it mustn't ask the server a second time.
			lastText = ""
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
		b.right = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
		b.right:SetPoint("RIGHT", b, "RIGHT", -4, 0)
		b.right:SetJustifyH("RIGHT")
		b.name = b:CreateFontString(nil, "OVERLAY", "GameFontNormal")
		b.name:SetPoint("LEFT", b, "LEFT", 20, 0)
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
				db.collapsed[e.group] = not db.collapsed[e.group] or nil
				renderList()
			else
				selectEntry(e)
			end
		end)
		rows[i] = b
	end

	-- Every entry of the list, groups expanded or not; byKey finds a remembered one.
	local all, byKey = {}, {}
	local function add(e)
		table.insert(all, e)
		if e.key then byKey[e.key] = e end
	end
	add({ key = "c" .. CLASS_TRAINER, category = CLASS_TRAINER, name = (className or "Class") .. " trainer" })
	for _, c in ipairs(MAIN) do add({ key = "c" .. c.id, category = c.id, name = c.name }) end
	add({ group = "professions", name = "Profession trainers" })
	for _, p in ipairs(PROFESSIONS) do add({ key = "p" .. p.skill, skill = p.skill, name = p.name, under = "professions" }) end
	add({ group = "more", name = "More" })
	for _, c in ipairs(MORE) do
		if not c.class or c.class == playerClass then add({ key = "c" .. c.id, category = c.id, name = c.name, under = "more" }) end
	end

	local function buildEntries()
		local out = {}
		for _, e in ipairs(all) do
			if not e.under or not db.collapsed[e.under] or e == state.selected then table.insert(out, e) end
		end
		return out
	end

	local function renderRows()
		local offset = FauxScrollFrame_GetOffset(scroll)
		local ranks = state.ranks
		local searching = search:GetText() ~= ""
		for i = 1, ROWS do
			local b = rows[i]
			local e = state.entries[offset + i]
			b.entry = e
			if not e then
				b:Hide()
			else
				b:Show()
				if e.group then
					b.expand:SetTexture(db.collapsed[e.group] and "Interface\\Buttons\\UI-PlusButton-Up"
						or "Interface\\Buttons\\UI-MinusButton-Up")
					b.expand:Show()
					b.name:SetFontObject(GameFontNormal)
					b.name:SetTextColor(1, 0.82, 0)
					b.right:SetText("")
					b.selectedBg:Hide()
				else
					b.expand:Hide()
					b.name:SetFontObject(GameFontHighlightSmall)
					b.name:SetTextColor(1, 1, 1)
					b.right:SetText(e.skill and ranks[e.skill] and ("|cff808080" .. ranks[e.skill] .. "|r") or "")
					setShown(b.selectedBg, e == state.selected and not searching)
				end
				b.name:SetText(e.name)
			end
		end
		FauxScrollFrame_Update(scroll, #state.entries, ROWS, ROW_HEIGHT)
	end

	scroll:SetScript("OnVerticalScroll", function (self, offset)
		FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, renderRows)
	end)

	renderList = function ()
		state.entries = buildEntries()
		renderRows()
	end

	-----------------------------------------
	-- the places on the right

	local results = {}
	for i = 1, RESULTS do
		local b = CreateFrame("Button", nil, right)
		b:SetHeight(RESULT_HEIGHT)
		b:SetPoint("TOPLEFT", right, "TOPLEFT", 10, -44 - (i - 1) * RESULT_HEIGHT)
		b:SetPoint("RIGHT", resultScroll, "RIGHT", 0, 0)
		b.distance = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
		b.distance:SetPoint("TOPRIGHT", b, "TOPRIGHT", -4, -3)
		b.distance:SetJustifyH("RIGHT")
		b.name = b:CreateFontString(nil, "OVERLAY", "GameFontNormal")
		b.name:SetPoint("TOPLEFT", b, "TOPLEFT", 4, -3)
		b.name:SetPoint("RIGHT", b.distance, "LEFT", -8, 0)
		b.name:SetJustifyH("LEFT")
		b.rank = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
		b.rank:SetPoint("BOTTOMRIGHT", b, "BOTTOMRIGHT", -4, 3)
		b.rank:SetJustifyH("RIGHT")
		b.where = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
		b.where:SetPoint("BOTTOMLEFT", b, "BOTTOMLEFT", 4, 3)
		b.where:SetPoint("RIGHT", b.rank, "LEFT", -8, 0)
		b.where:SetJustifyH("LEFT")
		b.where:SetTextColor(0.75, 0.75, 0.75)
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
			local p = self.place
			if not p then return end
			state.picked = p
			renderResults()
			J.ShowDirectoryPlace(p, function (line)
				if not line then
					J.Print("that place can't be flagged any more. Refresh the list.")
				elseif p.yards >= 0 then
					J.Print(line .. ". Flagged on your map.")
				else
					J.Print(line .. ".")
				end
			end)
		end)
		b:SetScript("OnEnter", function (self)
			local p = self.place
			if not p then return end
			GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
			GameTooltip:SetText(p.name, 1, 0.82, 0)
			if p.title ~= "" then GameTooltip:AddLine("<" .. p.title .. ">", 1, 1, 1) end
			GameTooltip:AddLine(whereText(p), 0.8, 0.8, 0.8)
			if p.yards >= 0 then
				GameTooltip:AddLine(distanceText(p) .. " of you", 0.8, 0.8, 0.8)
				GameTooltip:AddLine("Click to flag it on your map.", 0.4, 1, 0.4)
			else
				GameTooltip:AddLine("On " .. distanceText(p) .. ". Click for its coordinates.", 0.6, 0.6, 0.6, true)
			end
			GameTooltip:Show()
		end)
		b:SetScript("OnLeave", function () GameTooltip:Hide() end)
		results[i] = b
	end

	renderResults = function ()
		local offset = FauxScrollFrame_GetOffset(resultScroll)
		for i = 1, RESULTS do
			local b = results[i]
			local p = state.places[offset + i]
			b.place = p
			if not p then
				b:Hide()
			else
				b:Show()
				b.name:SetText(p.name .. (p.title ~= "" and ("  |cffffffff<" .. p.title .. ">|r") or ""))
				b.where:SetText(whereText(p))
				b.distance:SetText(distanceText(p))
				if p.yards >= 0 then b.distance:SetTextColor(1, 1, 1) else b.distance:SetTextColor(0.6, 0.6, 0.6) end
				b.rank:SetText(RANKS[p.rank] and ("up to " .. RANKS[p.rank]) or "")
				setShown(b.selectedBg, state.picked ~= nil and state.picked.place == p.place)
			end
		end
		FauxScrollFrame_Update(resultScroll, #state.places, RESULTS, RESULT_HEIGHT)
	end

	resultScroll:SetScript("OnVerticalScroll", function (self, offset)
		FauxScrollFrame_OnVerticalScroll(self, offset, RESULT_HEIGHT, renderResults)
	end)

	local serial = 0
	local function show(places, found, none, label)
		if not places then
			state.places = {}
			renderResults()
			empty:SetText("The directory couldn't be loaded. Try again in a moment.")
			empty:Show()
			return
		end
		state.places = places
		local still
		for _, p in ipairs(places) do
			if state.picked and p.place == state.picked.place then still = p end
		end
		state.picked = still
		renderResults()
		if #places == 0 then
			subtitle:SetText("")
			empty:SetText(none)
			empty:Show()
		else
			empty:Hide()
			local text = label .. " - nearest first"
			if found > #places then text = text .. " - |cffff8020showing " .. #places .. " of " .. found .. "|r" end
			subtitle:SetText(text)
		end
	end

	renderRight = function ()
		serial = serial + 1
		local mine = serial
		local text = searchText(search:GetText())
		local function answer(none, label)
			return function (places, found)
				if mine ~= serial then return end
				show(places, found, none, label)
			end
		end
		empty:SetText("Loading...")
		empty:Show()
		if text then
			title:SetText("Search: " .. text)
			subtitle:SetText("")
			J.DirectorySearch(text, answer("No one like that could be found. Try part of a name, a title such as "
				.. "\"alchemy trainer\", or a zone.", "Everyone whose name, title or zone has every word"))
			return
		end
		local e = state.selected
		if not e then
			title:SetText("")
			subtitle:SetText("")
			state.places = {}
			renderResults()
			empty:SetText("Pick what you're looking for on the left, or search.")
			return
		end
		title:SetText(e.name)
		subtitle:SetText("")
		if e.skill then
			J.DirectoryProfession(e.skill, answer("No " .. e.name:lower() .. " trainer you can use was found.", e.name .. " trainers"))
		else
			J.DirectoryCategory(e.category, answer("None that you can use was found.", e.name))
		end
	end

	refresh:SetScript("OnClick", function () renderRight() end)

	-----------------------------------------
	-- loading

	local function showProblem(problem, sub)
		body:Hide()
		status:SetText(problem)
		statusSub:SetText(sub or "")
		status:Show()
		statusSub:Show()
	end

	-- Puts a text in the search box and searches it (.find <words>).
	local function searchFor(text)
		lastText = text
		search:SetText(text)
		setShown(search.hint, false)
		renderRows()
		renderRight()
	end

	-- pending: .find's words, searched instead of the remembered pick.
	local function load(pending)
		local problem, sub = J.ServerStatus()
		if not problem and not J.Has(J.HELLO_DIRECTORY) then
			problem, sub = "This realm has no directory yet",
				"The realm's journal module has its Directory turned off (Journal.Directory.Enable), or is older than this addon."
		end
		if problem then showProblem(problem, sub) return end
		status:Hide()
		statusSub:Hide()
		body:Show()
		if not state.selected then
			state.selected = byKey[db.selected or ""] or byKey["c" .. CLASS_TRAINER]
		end
		J.DirectoryRanks(function (ranks)
			if ranks then
				state.ranks = ranks
				renderRows()
			end
		end)
		renderList()
		if pending then
			searchFor(pending)
		else
			renderRight()
		end
	end

	search:SetScript("OnTextChanged", function (self)
		local text = self:GetText() or ""
		setShown(self.hint, text == "" and not self:HasFocus())
		if text == lastText then return end
		lastText = text
		J.Debounce("directory-search", 0.4, function ()
			if search:GetText() ~= text then return end
			if text == "" or searchText(text) then
				renderRows()
				renderRight()
			end
		end)
	end)
	search:SetScript("OnEditFocusGained", function (self) self.hint:Hide() end)
	search:SetScript("OnEditFocusLost", function (self) setShown(self.hint, (self:GetText() or "") == "") end)
	search:SetScript("OnEscapePressed", function (self)
		self:SetText("")
		self:ClearFocus()
	end)

	local function shown() return panel:IsVisible() end
	J.On("SECTION_SERVER", function () if shown() then load() end end)

	return {
		load = load,
		search = searchFor,
	}
end

local tab
local pendingSearch
J.AddSection({
	key = "directory", name = "Directory", order = 26, icon = "Interface\\Icons\\INV_Misc_Map_01",
	build = function (panel) tab = build(panel) end,
	onShow = function ()
		if not tab then return end
		local text = pendingSearch
		pendingSearch = nil
		tab.load(text)
	end,
})

-- The Town Guide spell (OPEN:<token>:directory) toggles the tab; .find <words> opens it on a search.
J.On("OPEN", function (key, argument)
	if key ~= "directory" then
		if J.OpenSection then J.OpenSection(key) end
		return
	end
	if not argument then
		J.ToggleSection("directory")
		return
	end
	if Journal.IsTabShown("directory") and tab then
		tab.search(argument)
		return
	end
	pendingSearch = argument
	J.OpenSection("directory")
end)
