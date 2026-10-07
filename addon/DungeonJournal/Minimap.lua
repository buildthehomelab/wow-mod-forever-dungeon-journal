-- A minimap button that opens the journal: left-click toggles it, drag moves it around the
-- minimap. /dj minimap hides or shows it.

local DJ = DungeonJournal

local button = CreateFrame("Button", "DungeonJournalMinimapButton", Minimap)
button:SetSize(31, 31)
button:SetFrameStrata("MEDIUM")
button:SetFrameLevel(8)
button:RegisterForClicks("LeftButtonUp", "RightButtonUp")
button:RegisterForDrag("LeftButton")
button:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")

local icon = button:CreateTexture(nil, "BACKGROUND")
icon:SetTexture("Interface\\Icons\\INV_Misc_Book_09")
icon:SetSize(20, 20)
icon:SetPoint("TOPLEFT", button, "TOPLEFT", 7, -5)
icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)
local border = button:CreateTexture(nil, "OVERLAY")
border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
border:SetSize(53, 53)
border:SetPoint("TOPLEFT", button, "TOPLEFT")

local function place()
	local angle = math.rad(DungeonJournalDB.minimap.angle or 200)
	local radius = (Minimap:GetWidth() / 2) + 10
	button:ClearAllPoints()
	button:SetPoint("CENTER", Minimap, "CENTER", math.cos(angle) * radius, math.sin(angle) * radius)
end

button:SetScript("OnDragStart", function (self)
	self:SetScript("OnUpdate", function ()
		local mx, my = Minimap:GetCenter()
		local cx, cy = GetCursorPosition()
		local scale = Minimap:GetEffectiveScale()
		DungeonJournalDB.minimap.angle = math.deg(math.atan2(cy / scale - my, cx / scale - mx))
		place()
	end)
end)
button:SetScript("OnDragStop", function (self) self:SetScript("OnUpdate", nil) end)
button:SetScript("OnClick", function () DJ.Toggle() end)
button:SetScript("OnEnter", function (self)
	GameTooltip:SetOwner(self, "ANCHOR_LEFT")
	GameTooltip:SetText("Dungeon Journal")
	GameTooltip:AddLine("Bosses, abilities, loot and quests of every dungeon and raid.", 1, 1, 1, true)
	local key = GetBindingKey("DUNGEONJOURNAL_TOGGLE")
	GameTooltip:AddLine("Click to open" .. (key and (" (" .. key .. ")") or "") .. ", drag to move.", 0.6, 0.6, 0.6)
	GameTooltip:Show()
end)
button:SetScript("OnLeave", function () GameTooltip:Hide() end)

local function update()
	if DungeonJournalDB.minimap.hide then button:Hide() else button:Show() ; place() end
end

DJ.On("LOGIN", update)
DJ.On("MINIMAP", update)
