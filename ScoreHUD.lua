-- ==============================================================
-- ScoreHUD.lua — compact, title left, translucent section bars
-- Allies (incl. you) on top, Opponents bottom
-- ==============================================================

ScoreHUDData = {}

-- layout knobs
local PANEL_TOP    = 20
local PANEL_RIGHT  = 20
local ROW_HEIGHT   = 22
local SCORE_COL_W  = 56

-- ---- ordering helpers ----
local function build_order()
  local n  = World_GetPlayerCount() or 0
  local lp = Game_GetLocalPlayer()
  if not lp or n <= 0 then return {}, {} end

  local allies, enemies = {}, {}
  for slot = 1, n do
    local p = World_GetPlayerAt(slot)
    if p then
      local rel = Player_ObserveRelationship(lp, p) -- observe FROM local player
      if (p == lp) or (rel == R_ALLY) then
        table.insert(allies,  {slot=slot, p=p})
      else
        table.insert(enemies, {slot=slot, p=p})
      end
    end
  end
  table.sort(allies,  function(a,b) return a.slot < b.slot end)
  table.sort(enemies, function(a,b) return a.slot < b.slot end)
  return allies, enemies
end

local function total_for(entry)
  if entry and type(entry.score) == "number" and entry.score > 0 then
    return entry.score
  end
  if _score and _score.data_context and entry then
    local ctx = Score_GetPlayerDataContext(entry.index)
    if ctx and ctx.score then
      return math.floor(
        (ctx.score.military   or 0) +
        (ctx.score.economy    or 0) +
        (ctx.score.society    or 0) +
        (ctx.score.technology or 0)
      )
    end
  end
  return 0
end

-- one row (prefix "A"/"E", numbered per section)
local function genRowXaml(prefix, idx)
  local k = prefix..idx
  return string.format([[
    <Border CornerRadius="5" Margin="0,0,0,3" Background="#05000000" BorderBrush="#1AFFFFFF" BorderThickness="1" Height="%d" Opacity="0.95">
      <Grid>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="22"/>
          <ColumnDefinition Width="*"/>
          <ColumnDefinition Width="%d"/>
        </Grid.ColumnDefinitions>

        <Border Grid.Column="0" Background="{Binding [%s_HeaderBg]}" CornerRadius="5,0,0,5">
          <TextBlock Text="%d" Foreground="#FFFFFF" FontSize="10" FontWeight="Bold"
                     HorizontalAlignment="Center" VerticalAlignment="Center"/>
        </Border>

        <TextBlock Grid.Column="1"
                   Text="{Binding [%s_Name]}"
                   Foreground="{Binding [%s_NameColor]}"
                   FontSize="11" FontWeight="Bold"
                   Margin="6,0,4,0"
                   TextTrimming="CharacterEllipsis"
                   VerticalAlignment="Center"/>

        <TextBlock Grid.Column="2"
                   Text="{Binding [%s_Score]}"
                   Foreground="{Binding [%s_ScoreColor]}"
                   FontSize="11" FontWeight="Bold"
                   HorizontalAlignment="Right"
                   VerticalAlignment="Center"
                   Margin="0,0,2,0"/>
      </Grid>
    </Border>
  ]], ROW_HEIGHT, SCORE_COL_W, k, idx, k, k, k, k)
end

-- translucent section header bar (keep color, see-through)
local function genHeaderXaml(title, tint)
  return string.format([[
    <Border Background="%s" CornerRadius="8" Padding="8,3" Margin="0,2,0,5" Opacity="0.85">
      <TextBlock Text="%s" Foreground="#FFFFFF" FontSize="11" FontWeight="Bold"/>
    </Border>
  ]], tint, title)
end

-- ---------- SHOW ----------
function ShowScoreHUD()
  pcall(function()
    if Rule_Remove and UpdateScoreHUD then Rule_Remove(UpdateScoreHUD) end
    if UI_Remove then UI_Remove("ScoreHUD") end
  end)

  local allies, enemies = build_order()
  local na, ne = #allies, #enemies
  if (na + ne) <= 1 then return end

  local xaml = string.format([[
<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        HorizontalAlignment="Right" VerticalAlignment="Top"
        Margin="0,%d,%d,0" CornerRadius="12"
        Background="#40000000" BorderBrush="#1AFFFFFF" BorderThickness="1">

  <Grid>
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto"/>
      <RowDefinition Height="*"/>
    </Grid.RowDefinitions>

    <Border Grid.Row="0" Background="#0D87CEEB" CornerRadius="12,12,0,0" Padding="12,5" Opacity="1">
      <TextBlock Text="PLAYER SCORE" Foreground="#FFFFFF" FontSize="12" FontWeight="Bold"
                 HorizontalAlignment="Left" Margin="2,0,0,0"/>
    </Border>

    <StackPanel Grid.Row="1" Orientation="Vertical" Margin="8,5,8,8" Opacity="1">]],
    PANEL_TOP, PANEL_RIGHT)

  if na > 0 then
    xaml = xaml .. genHeaderXaml("ALLIES",    "#302E8B57")
    for i = 1, na do xaml = xaml .. genRowXaml("A", i) end
  end
  if ne > 0 then
    xaml = xaml .. genHeaderXaml("OPPONENTS", "#30D32F2F")
    for j = 1, ne do xaml = xaml .. genRowXaml("E", j) end
  end

  xaml = xaml .. [[
    </StackPanel>
  </Grid>
</Border>]]

  UI_AddChild("ScarDefault","XamlPresenter","ScoreHUD", {
    IsHitTestVisible=false,
    Xaml=xaml,
    DataContext=UI_CreateDataContext(ScoreHUDData)
  })

  UpdateScoreHUD()
  Rule_AddInterval(UpdateScoreHUD, 1)
end

-- ---------- UPDATE ----------
function UpdateScoreHUD()
  local allies, enemies = build_order()
  if (not allies or #allies==0) and (not enemies or #enemies==0) then return end

  local function fill(block, prefix)
    for i, rec in ipairs(block or {}) do
      local p     = rec.p
      local entry = Core_GetPlayersTableEntry(p)

      -- Unicode-safe name (no Loc_ToAnsi): use LocString directly.
      local disp  = entry and Player_GetDisplayName(entry.id)
      local name  = (disp and disp.LocString) or ("Player "..tostring(rec.slot))

      local total = total_for(entry)
      local c     = Player_GetUIColour(p)
      local hex   = string.format("#%02X%02X%02X", c.r, c.g, c.b)

      local k = prefix..i
      ScoreHUDData[k.."_Name"]       = name
      ScoreHUDData[k.."_Score"]      = tostring(total)
      ScoreHUDData[k.."_HeaderBg"]   = hex
      ScoreHUDData[k.."_NameColor"]  = hex
      ScoreHUDData[k.."_ScoreColor"] = hex
    end
  end

  fill(allies,  "A")
  fill(enemies, "E")

  UI_SetDataContext("ScoreHUD", ScoreHUDData)
end

-- ---------- HIDE ----------
function HideScoreHUD()
  pcall(function()
    if Rule_Remove and UpdateScoreHUD then Rule_Remove(UpdateScoreHUD) end
    if UI_Remove then UI_Remove("ScoreHUD") end
  end)
end

-- ---------- optional tiny nudge ----------
function SetScoreHUDOffsets(top, right)
  if top   then PANEL_TOP   = top   end
  if right then PANEL_RIGHT = right end
  ShowScoreHUD()
end
function SetScoreColWidth(w)  if w  then SCORE_COL_W = w  ShowScoreHUD() end end
function SetScoreRowHeight(h) if h  then ROW_HEIGHT  = h  ShowScoreHUD() end end
