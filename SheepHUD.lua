-- SheepHUD.lua — spawn a GAIA herdable sheep at Town Centre (no HUD)

-- Safely fetch the sheep *squad* blueprint
local function GetSheepSquadBP()
    local ok, bp = pcall(BP_GetSquadBlueprint, "gaia_herdable_sheep")
    if ok and bp then return bp end
    -- Uncomment any fallback names if your build uses them:
    -- ok, bp = pcall(BP_GetSquadBlueprint, "gaia_huntable_sheep"); if ok and bp then return bp end
    -- ok, bp = pcall(BP_GetSquadBlueprint, "gaia_sheep");          if ok and bp then return bp end
    return nil
end

-- Best-effort Town Centre position for player p
local function GetTCPos(p)
    local pos = {X=0, Y=0, Z=0}

    -- Primary: engine helper if present
    if GetTownCentrePosition then
        local ok, tc = pcall(GetTownCentrePosition, p)
        if ok and tc and tc.X then return tc end
    end

    -- Fallbacks (only used if available in your build)
    if Player_GetStartingPosition then
        local ok, sp = pcall(Player_GetStartingPosition, p)
        if ok and sp and sp.X then return sp end
    end
    if Camera_GetWorldPosition then
        local ok, cp = pcall(Camera_GetWorldPosition)
        if ok and cp and cp.X then return cp end
    end

    return pos
end

-- Public entry your C++ calls on toggle ON
function ShowSheepHUD()
    local p = Game_GetLocalPlayer(); if not p then return end
    local bp = GetSheepSquadBP();    if not bp then return end
    local pos = GetTCPos(p)

    -- Spawn as GAIA (playerId = 0) so it's herdable/neutral
    -- Uses the safe ENV cheat spawner you already rely on.
    pcall(Squad_CreateAndSpawnTowardENVCheat, bp, 0, pos, pos)
end

-- No-op (needed for your C++ toggle OFF)
function HideSheepHUD()
    -- intentionally empty
end
