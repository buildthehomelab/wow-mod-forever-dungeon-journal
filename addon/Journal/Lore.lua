-- Lore: the codex's data from the server, what you've read, the Library of books you've read in
-- the world, and the banner that shows when you discover an entry or shelve a book while playing.
-- The tab itself is LoreTab.lua.

local J = Journal
local Unescape = J.Unescape

J.LORE_KINDS = { [0] = "Overview", [1] = "Places", [2] = "People", [3] = "History" }
J.LORE = { UNKNOWN = 0, KNOWN = 1, FOUND = 2 }

local function readDB()
	JournalCharDB.loreRead = JournalCharDB.loreRead or {}
	return JournalCharDB.loreRead
end

-- Found while playing and not opened yet (books keep their own key, "b<book>").
function J.LoreIsNew(e)
	return e.state == J.LORE.FOUND and not readDB()[e.readKey or e.id]
end

function J.LoreMarkRead(id)
	if not readDB()[id] then
		readDB()[id] = true
		J.Fire("LORE_READ", id)
	end
end

-----------------------------------------
-- server data

-- Every zone with lore: { list = { { id, group, known, total, era, name } }, byId, known, total, here }.
local zones, zonesAt
function J.LoadLoreZones(done, force)
	if not J.Has(J.HELLO_LORE) then done(nil) return end
	if not force and zones and GetTime() - zonesAt < 5 then done(zones) return end
	J.Fetch("Y", "Y", nil, function (result)
		local out = { list = {}, byId = {}, known = tonumber(result.meta[1]) or 0, total = tonumber(result.meta[2]) or 0,
			here = tonumber(result.meta[3]) or 0 }
		for _, r in ipairs(result.rows) do
			if r[1] == "Z" then
				local z = { id = r[2], group = tostring(r[3]), known = r[4], total = r[5], era = r[6],
					name = Unescape(tostring(r[7] or "-")) }
				table.insert(out.list, z)
				out.byId[z.id] = z
			end
		end
		return out
	end, function (out)
		if out then zones, zonesAt = out, GetTime() end
		done(out)
	end)
end

-- A zone's entries: { { id, kind, state, era, title, hint } }, by kind and the zone's own order.
local zoneEntries, zoneEntriesAt = {}, {}
function J.LoadLoreZone(id, done, force)
	if not J.Has(J.HELLO_LORE) then done(nil) return end
	if not force and zoneEntries[id] and GetTime() - zoneEntriesAt[id] < 5 then done(zoneEntries[id]) return end
	J.Fetch("U" .. id, "U", { tostring(id) }, function (result)
		local list = { known = tonumber(result.meta[2]) or 0, total = tonumber(result.meta[3]) or 0 }
		for _, r in ipairs(result.rows) do
			if r[1] == "E" then
				table.insert(list, { id = r[2], kind = r[3], state = r[4], era = r[5], zone = id,
					title = Unescape(tostring(r[6] or "-")), hint = J.NonEmpty(Unescape(tostring(r[7] or "-"))) })
			end
		end
		return list
	end, function (list)
		if list then zoneEntries[id], zoneEntriesAt[id] = list, GetTime() end
		done(list)
	end)
end

-- A discovered entry's text: { id, zone, kind, title, text }. Kept for the session.
local texts = {}
function J.LoadLoreEntry(id, done)
	if texts[id] then done(texts[id]) return end
	if not J.Has(J.HELLO_LORE) then done(nil) return end
	J.Fetch("V" .. id, "V", { tostring(id) }, function (result)
		local parts = {}
		-- One piece of the text per row, kept as it came (asked raw: " 1066." stays text).
		for _, r in ipairs(result.rows) do table.insert(parts, Unescape(r[1] or "-")) end
		local meta = result.meta
		return { id = tonumber(meta[1]) or id, zone = tonumber(meta[2]) or 0, kind = tonumber(meta[3]) or 1,
			title = Unescape(meta[4] or "-"), text = table.concat(parts) }
	end, function (entry, err)
		if entry then texts[id] = entry end
		done(entry, err)
	end, true)
end

J.On("LORE_FOUND", function ()
	zonesAt = 0
	for k in pairs(zoneEntriesAt) do zoneEntriesAt[k] = 0 end
end)

-----------------------------------------
-- the Library

-- The books you've read: { { id, time, title, where } }, by title.
local library, libraryAt
function J.LoadLibrary(done, force)
	if not J.Has(J.HELLO_LIBRARY) then done(nil) return end
	if not force and library and GetTime() - libraryAt < 5 then done(library) return end
	J.Fetch("J", "J", nil, function (result)
		local list = {}
		for _, r in ipairs(result.rows) do
			if r[1] == "B" then
				table.insert(list, { id = r[2], time = tonumber(r[3]) or 0, title = Unescape(tostring(r[4] or "-")),
					where = J.NonEmpty(Unescape(tostring(r[5] or "-"))) })
			end
		end
		table.sort(list, function (a, b) return a.title:lower() < b.title:lower() end)
		return list
	end, function (list)
		if list then library, libraryAt = list, GetTime() end
		done(list)
	end)
end

-- A book's pages: { id, title, pages = { "text", ... } }. Kept for the session.
local books = {}
function J.LoadBook(id, done)
	if books[id] then done(books[id]) return end
	if not J.Has(J.HELLO_LIBRARY) then done(nil) return end
	J.Fetch("P" .. id, "P", { tostring(id) }, function (result)
		local pages = {}
		local count = tonumber(result.meta[2]) or 0
		for i = 1, count do pages[i] = "" end
		-- Each row: "<page>.<piece of its text>", kept as it came (asked raw).
		for _, r in ipairs(result.rows) do
			local page, piece = (r[1] or ""):match("^(%d+)%.(.*)$")
			page = tonumber(page)
			if page and page >= 1 and page <= count then pages[page] = pages[page] .. Unescape(piece) end
		end
		for i = 1, count do pages[i] = J.FillPlaceholders(pages[i]) end
		return { id = tonumber(result.meta[1]) or id, title = Unescape(result.meta[3] or "-"), pages = pages }
	end, function (book, err)
		if book then books[id] = book end
		done(book, err)
	end, true)
end

J.On("BOOK_FOUND", function () libraryAt = 0 end)

-----------------------------------------
-- the discovery banner

local QUEUE_MAX = 6
local SHOW_FOR = 4.5
local FADE = 0.8

local banner = CreateFrame("Button", "JournalLoreBanner", UIParent)
banner:SetSize(320, 64)
banner:SetPoint("TOP", UIParent, "TOP", 0, -150)
banner:SetFrameStrata("HIGH")
banner:Hide()
banner:SetBackdrop({
	bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
	edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
	tile = true, tileSize = 32, edgeSize = 24,
	insets = { left = 6, right = 6, top = 6, bottom = 6 },
})
banner.icon = banner:CreateTexture(nil, "ARTWORK")
banner.icon:SetSize(36, 36)
banner.icon:SetPoint("LEFT", banner, "LEFT", 14, 0)
banner.icon:SetTexture("Interface\\Icons\\INV_Misc_Book_11")
banner.label = banner:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
banner.label:SetPoint("TOPLEFT", banner.icon, "TOPRIGHT", 10, -2)
banner.label:SetPoint("RIGHT", banner, "RIGHT", -14, 0)
banner.label:SetJustifyH("LEFT")
banner.title = banner:CreateFontString(nil, "OVERLAY", "GameFontHighlightLarge")
banner.title:SetPoint("TOPLEFT", banner.label, "BOTTOMLEFT", 0, -4)
banner.title:SetPoint("RIGHT", banner, "RIGHT", -14, 0)
banner.title:SetJustifyH("LEFT")
J.LoreBanner = banner

local queue = {}
local shownAt

local function showNext()
	local item = table.remove(queue, 1)
	if not item then
		banner:Hide()
		return
	end
	banner.item = item
	if item.book then
		banner.label:SetText("Added to your library")
		banner.icon:SetTexture("Interface\\Icons\\INV_Misc_Book_09")
	else
		local kind = J.LORE_KINDS[item.kind] or "Lore"
		banner.label:SetText("Lore discovered" .. (item.kind ~= 0 and (" - " .. kind) or ""))
		banner.icon:SetTexture("Interface\\Icons\\INV_Misc_Book_11")
	end
	banner.title:SetText(item.title)
	if #queue > 0 then banner.label:SetText(banner.label:GetText() .. "  |cff808080(+" .. #queue .. ")|r") end
	banner:SetAlpha(1)
	banner:Show()
	shownAt = GetTime()
	if not JournalDB.loreQuiet then PlaySound("igQuestLogOpen") end
end

banner:SetScript("OnUpdate", function (self)
	if not shownAt then return end
	local age = GetTime() - shownAt
	if age >= SHOW_FOR + FADE then
		shownAt = nil
		showNext()
	elseif age > SHOW_FOR then
		self:SetAlpha(1 - (age - SHOW_FOR) / FADE)
	end
end)

banner:SetScript("OnClick", function (self, button)
	local item = self.item
	shownAt = nil
	wipe(queue)
	self:Hide()
	if button ~= "RightButton" and item then
		if item.book then J.OpenLibrary(item.book) else J.OpenLore(item.zone, item.id) end
	end
end)
banner:RegisterForClicks("LeftButtonUp", "RightButtonUp")
J.SetTooltip(banner, "Click to read it in the Journal. Right-click to dismiss.", "ANCHOR_BOTTOM")

J.On("LORE_FOUND", function (id, zone, kind, title)
	if JournalDB.loreBanner == false then return end
	if #queue >= QUEUE_MAX then table.remove(queue, 1) end
	table.insert(queue, { id = id, zone = zone, kind = kind, title = title })
	if not banner:IsShown() then showNext() end
end)

J.On("BOOK_FOUND", function (book, title)
	if JournalDB.loreBanner == false then return end
	if #queue >= QUEUE_MAX then table.remove(queue, 1) end
	table.insert(queue, { book = book, title = title })
	if not banner:IsShown() then showNext() end
end)

-- Opens the Lore tab's Library on a book.
function J.OpenLibrary(book)
	J.OpenLore("library", book)
end

-- Opens the Lore tab on a zone, and an entry in it.
function J.OpenLore(zone, entry)
	J.loreTarget = { zone = zone, entry = entry }
	J.OpenSection("lore")
	J.Fire("LORE_TARGET")
end

-----------------------------------------

local HELP = "|cffffd200/lore|r opens the Lore tab, |cffffd200/lore library|r its Library of the books you've read. "
	.. "|cffffd200/lore banner|r turns the discovery banner on or off, |cffffd200/lore sound|r its sound."

SLASH_JOURNALLORE1 = "/lore"
SlashCmdList.JOURNALLORE = function (msg)
	msg = (msg or ""):lower():match("^%s*(.-)%s*$")
	if msg == "banner" then
		JournalDB.loreBanner = JournalDB.loreBanner == false and true or false
		J.Print("lore discovery banner " .. (JournalDB.loreBanner and "on." or "off."))
	elseif msg == "sound" then
		JournalDB.loreQuiet = not JournalDB.loreQuiet or nil
		J.Print("lore discovery sound " .. (JournalDB.loreQuiet and "off." or "on."))
	elseif msg == "help" then
		J.Print(HELP)
	elseif msg == "library" or msg == "books" then
		J.OpenLibrary()
	else
		J.ToggleSection("lore")
	end
end
