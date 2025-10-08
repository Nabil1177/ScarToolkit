# overlay_sender_auto_single_rank.py
# v3.2 — Steam-preferred, snapshot on connect + refresh, IPv4 bind, env-port, friendly bind error

import asyncio, json, os, re
from pathlib import Path
import requests, websockets

WS_PORT = int(os.getenv("OVERLAY_WS_PORT", "12346"))  # <-- env override (default 12346)
AOE = "https://aoe4world.com"
CONFIG_FILE = Path(__file__).with_name("overlay_config.json")
session = requests.Session()

# Country code -> full name (extend if needed)
COUNTRIES = {
    "US":"United States","GB":"United Kingdom","AU":"Australia","CA":"Canada","CN":"China",
    "JP":"Japan","KR":"South Korea","RU":"Russia","DE":"Germany","FR":"France","IN":"India",
    "BR":"Brazil","SE":"Sweden","FI":"Finland","NO":"Norway","DK":"Denmark","NL":"Netherlands",
    "PL":"Poland","TR":"Türkiye","ES":"Spain","IT":"Italy","MX":"Mexico","ID":"Indonesia",
    "MY":"Malaysia","PH":"Philippines","SG":"Singapore","TH":"Thailand","TW":"Taiwan",
    "VN":"Vietnam","AR":"Argentina","CL":"Chile","CO":"Colombia","SA":"Saudi Arabia",
    "AE":"United Arab Emirates","EG":"Egypt","ZA":"South Africa","NZ":"New Zealand",
    "BD":"Bangladesh","PK":"Pakistan","IR":"Iran","IQ":"Iraq","IE":"Ireland"
}
def country_full(code:str)->str:
    if not code: return ""
    return COUNTRIES.get(code.upper(), code)

def load_pid():
    try:
        d = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
        s = str(d.get("profile_id") or "").strip()
        return s if s.isdigit() else None
    except Exception:
        return None

def save_pid(pid:str):
    try:
        CONFIG_FILE.write_text(json.dumps({"profile_id": str(pid)}, indent=2), encoding="utf-8")
    except Exception:
        pass

# ---------- Steam detection ----------
def _steam_path_from_registry():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            path, _ = winreg.QueryValueEx(k, "SteamPath")
            return path
    except Exception:
        return None

def detect_steamid64():
    candidates = []
    reg = _steam_path_from_registry()
    if reg:
        candidates.append(os.path.join(reg, "config", "loginusers.vdf"))
    candidates += [
        r"C:\Program Files (x86)\Steam\config\loginusers.vdf",
        os.path.expandvars(r"%PROGRAMFILES(X86)%\Steam\config\loginusers.vdf"),
        os.path.expandvars(r"%PROGRAMFILES%\Steam\config\loginusers.vdf"),
    ]
    for p in candidates:
        if os.path.exists(p):
            try:
                txt = Path(p).read_text(encoding="utf-8", errors="ignore")
                blocks = re.findall(r'\"(\d{17})\"\s*\{([^}]*)\}', txt, flags=re.S)
                # Prefer MostRecent=1
                for sid, body in blocks:
                    if re.search(r'\"MostRecent\"\s*\"1\"', body):
                        return sid
                if blocks:
                    return blocks[0][0]
            except Exception:
                pass
    return None

def map_steam_to_profile_id(steamid):
    try:
        r = session.get(f"{AOE}/api/v0/players/steam/{steamid}", timeout=10)
        if r.status_code == 200:
            data = r.json()
            pid = str(data.get("profile_id") or data.get("id") or "")
            if pid.isdigit(): return pid
    except Exception:
        pass
    # fallback search
    try:
        r = session.get(f"{AOE}/api/v0/players/search?query={steamid}", timeout=10)
        if r.status_code == 200:
            data = r.json()
            if isinstance(data, list) and data:
                pid = str(data[0].get("profile_id") or data[0].get("id") or "")
                if pid.isdigit(): return pid
            elif isinstance(data, dict) and "players" in data and data["players"]:
                pid = str(data["players"][0].get("profile_id") or data["players"][0].get("id") or "")
                if pid.isdigit(): return pid
    except Exception:
        pass
    return None
# ---------- /Steam detection ----------

def last_game(pid:str):
    r = session.get(f"{AOE}/api/v0/players/{pid}/games/last", timeout=10)
    r.raise_for_status()
    return r.json()

def _wr(m):
    if not isinstance(m, dict): return 0, None
    rating = m.get("rating") or 0
    wins   = m.get("wins_count") or m.get("wins") or 0
    games  = m.get("games_count") or m.get("games") or (wins + (m.get("losses_count") or m.get("losses") or 0))
    return rating, (round(100*wins/games) if games else None)

def _rank_from_mode(m):
    if not isinstance(m, dict): return None
    for k in ("rank_level","rankName","league_name","leagueName","league_text"):
        v = m.get(k)
        if isinstance(v, str) and v.strip(): return v.strip()
    if isinstance(m.get("league"), dict):
        name = m["league"].get("name") or m["league"].get("text")
        div  = m["league"].get("division") or m["league"].get("tier")
        if name: return f"{name} {div}" if div else name
    league_id = m.get("league_id") or m.get("leagueId") or m.get("leagueTier")
    division  = m.get("division") or m.get("tier") or m.get("subtier")
    if isinstance(league_id, int):
        names = {0:"Unranked",1:"Bronze",2:"Silver",3:"Gold",4:"Platinum",5:"Diamond",6:"Conqueror"}
        base = names.get(league_id)
        if base:
            if isinstance(division, int) and 1 <= division <= 3:
                roman = {1:"I",2:"II",3:"III"}[division]
                return f"{base} {roman}"
            return base
    return None

def ratings_and_ranks(modes):
    m1 = (modes or {}).get("qm_1v1") or (modes or {}).get("rm_1v1") or {}
    mt = (modes or {}).get("rm_team") or {}
    if not mt:
        best = None
        for k in ("qm_2v2","qm_3v3","qm_4v4","rm_2v2","rm_3v3","rm_4v4"):
            cand = (modes or {}).get(k) or {}
            if not best or (cand.get("rating") or 0) > (best.get("rating") or 0):
                best = cand
        mt = best or {}
    s_rating, s_wr = _wr(m1)
    t_rating, t_wr = _wr(mt)
    s_rank = _rank_from_mode(m1) or "unranked"
    t_rank = _rank_from_mode(mt) or "unranked"
    return s_rating, s_wr, s_rank, t_rating, t_wr, t_rank

def simplify(raw, my_pid):
    players=[]; my_team=None
    for t_idx, team in enumerate(raw.get("teams", []), start=1):
        for p in team:
            pid=str(p.get("profile_id") or p.get("id") or "")
            if my_pid and pid==str(my_pid): my_team=t_idx
            s_rating, s_wr, s_rank, t_rating, t_wr, t_rank = ratings_and_ranks(p.get("modes", {}))
            players.append({
                "name": p.get("name") or "?",
                "country": country_full(p.get("country") or ""),
                "solo_rating": str(s_rating) if s_rating else "-",
                "solo_wr": f"{s_wr}%" if s_wr is not None else "-",
                "solo_rank": s_rank,
                "team_rating": str(t_rating) if t_rating else "-",
                "team_wr": f"{t_wr}%" if t_wr is not None else "-",
                "team_rank": t_rank,
                "team": t_idx
            })
    players.sort(key=lambda x:(x["team"], x["name"].lower()))
    return {"players": players, "meta": {"my_team": my_team}}

async def main():
    clients=set()
    current_js=None
    # --- Prefer Steam mapping; fallback to saved; else prompt ---
    saved = load_pid()
    steamid = detect_steamid64()
    mapped = map_steam_to_profile_id(steamid) if steamid else None
    if mapped:
        if mapped != saved:
            print(f"[detect] SteamID64 {steamid} -> profile_id {mapped} (saved)")
            save_pid(mapped)
        pid = mapped
    else:
        pid = saved

    if not pid:
        print("Paste your numeric AoE4World profile_id (from the URL /players/<id>-name), or press Enter to skip:")
        s=input("> ").strip()
        if s.isdigit(): save_pid(s); pid=s

    print(f"[overlay_sender_auto_single_rank v3.2] WebSocket ws://127.0.0.1:{WS_PORT}")
    if pid: print(f"[info] Using profile_id={pid}")
    else:   print("[warn] No profile_id yet; overlay will stay empty until you enter one.")

    async def handler(ws):
        nonlocal current_js
        clients.add(ws)
        try:
            # Always send something immediately so HTML isn't blank
            await ws.send(current_js or json.dumps({"type":"player_data","data":{"players":[],"meta":{"my_team":None}}}))
            async for msg in ws:
                try:
                    m = json.loads(msg)
                    if m.get("type") == "refresh":
                        await ws.send(current_js or json.dumps({"type":"player_data","data":{"players":[],"meta":{"my_team":None}}}))
                except Exception:
                    pass
        finally:
            clients.discard(ws)

    async def loop():
        nonlocal current_js
        last_sent=None
        while True:
            if pid:
                try:
                    raw = last_game(pid)
                    data = simplify(raw, pid)
                    js = json.dumps({"type":"player_data","data":data})
                    current_js = js
                    tcount = len(raw.get("teams", []))
                    pcount = sum(len(t) for t in raw.get("teams", []))
                    print(f"[poll] teams={tcount} players={pcount} my_team={data['meta']['my_team']}")
                    if clients and js != last_sent:
                        await asyncio.gather(*(c.send(js) for c in list(clients)), return_exceptions=True)
                        last_sent = js
                except Exception as e:
                    print("[poll] error:", e)
            await asyncio.sleep(2)

    try:
        async with websockets.serve(handler, "127.0.0.1", WS_PORT):
            await loop()
    except OSError as e:
        # Friendly note if the port is already taken
        if getattr(e, "winerror", None) == 10048 or getattr(e, "errno", None) in (98, 48, 10048):
            print(f"[sender] Port {WS_PORT} already in use. Another sender is running.")
            return
        raise

if __name__=="__main__":
    asyncio.run(main())
