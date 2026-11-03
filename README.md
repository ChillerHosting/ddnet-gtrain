# GTrain

A server-side [DDNet](https://github.com/ddnet/ddnet) mod for training Gores maps: practice random parts of a map over and over, alone or against friends.

## How it works

- Every spawn puts you at a random spot between the start and the finish line, always above freeze and more than 50 path tiles past the start line, since map starts are usually easy. You hover frozen for one second, then play.
- While hovering you stand on a small laser door that opens exactly at the release. Clients predict it like any map door, so the start looks the same at every ping (recent DDNet clients). Maps without a switch layer are served with an empty one added, since doors need it.
- A personal blue flag is placed a set number of path tiles ahead (100 by default). A particle trail shows the shortest path to it, `/trail` hides it.
- Reach the flag to capture it. The capture message shows how many fails it took, your time when training alone and the [ELO](#elo) you earned, then you get a new spot.
- The race timer restarts with every attempt, so it always shows the time of your current try.
- Kill for a new spot, or `/r` to retry the same spot and flag. Every retry counts as a fail.
- Every attempt starts like after a kill, also with `/r` and after captures: weapons, powerups and shots from the last attempt are gone, only the weapons kept with `/shotgun` and the like stay.

## Commands

| Command | Description |
| --- | --- |
| Kill | New random spot and flag |
| `/r`, `/retry` | Retry the current spot and flag; practice binds like `/tc`, `/telecursor`, `/invincible`, `/rescue`, `/back`, `/tp`, `/teleport` and `/lasttp` do the same, except `/tc` and `/telecursor` in freeplay |
| `/fight <player>` | Join a player's fight; `/fight` alone leaves it |
| `/distance <50-1000>` | Your goal distance in tiles; `0` uses the server default |
| `/nodj` | Toggle starting attempts without double jump; touching the ground gives it back |
| `/forwards` | Toggle goals that only lead towards the finish line, see [below](#forwards) |
| `/scorelimit <0-1000>` | Points needed to win a fight (default 10, `0` for no limit) |
| `/shotgun`, `/grenade`, `/laser` (or `/rifle`), `/ninja` | Toggle keeping that weapon in every attempt; ninja never runs out. Captures with kept weapons earn no [ELO](#elo) |
| `/freeplay` | Toggle training without flags, paths and captures. `/tc` or `/telecursor` starts you on the tile you aim at, and `/r` returns there; in a fight, the leader chooses the start of everyone |
| `/trail` | Toggle showing the particle trail to the flag; it is your own choice, also in fights and while spectating |
| `/settings [player]` | Show your settings or another player's; in a fight, the leader's settings that apply to it |
| `/points`, `/elo [player]` | Show your ELO and rank, or another player's |
| `/top5`, `/top`, `/top5points`, `/top5elo [rank]` | Show five players of the ELO ladder, starting at the rank (1 by default) |
| `/rank`, `/profile [player]` | Show your [stats](#stats) and playtime, or another player's |
| `/top5captures`, `/top5attempts`, `/top5respawns`, `/top5playtime [rank]` | Show five players of these ladders |
| `/help` | List these commands, plus `/map`, `/mapinfo` and `/cmdlist` |

`/distance` and `/scorelimit` show the current value without an argument. In a fight, only the leader can change these settings, and they apply to the whole group. Leaving a fight keeps its settings, and joining one adopts its settings, including removing weapons it has disabled.

### Forwards

Goals normally lead in any direction from your spot, also back towards the start. With `/forwards`, every goal lies on a shortest path from your spot to the finish line, so each run goes the way the map is played. Spots closer to the finish than your goal distance are skipped then, so goals keep their full distance; on maps shorter than it, goals lead from the spots farthest from the finish. Like `/distance`, it applies to new goals, and retries keep the current one.

## ELO

Every capture earns points, and your ELO shows them on a scale from 0 to 9999. Both only ever grow, and players only see the ELO, the points stay internal. They are stored per player name in the server's database, like DDNet points.

- A capture earns points for every tile of the goal's path distance: 100 on Easy maps with one star, and 1.5 times as many for each star or difficulty step above that. These are the points per tile of a 100 tile goal:

  | | ★☆☆ | ★★☆ | ★★★ |
  | --- | --: | --: | --: |
  | Easy | 100 | 150 | 225 |
  | Main | 338 | 506 | 759 |
  | Hard | 1139 | 1709 | 2563 |
  | Insane | 3844 | 5767 | 8650 |
  | Extreme | 12975 | 19462 | 29193 |

  Deranged maps have no stars and count as Extreme ★★★★★ would, two steps above Extreme ★★★: 65684.

- Longer goals are worth more than their length, since every tile is another chance to fail: points grow with the distance to the power of 1.5, so twice the distance earns about 2.8 times as many. Per tile, 50 tile goals earn 0.71 times the table's points, 200 tiles 1.41 times and 500 tiles 2.24 times.
- That is multiplied by your speed compared to a typical one, from 12 path tiles per second on Easy down to 6 on Extreme ★★★ and 5.1 on Deranged (a bit below the speeds of the first captures on the KoG maps). Faster captures earn up to 4 times as much, slow ones still earn at least half. Only the time of the successful attempt counts.
- The distance is the goal's actual distance, which is shorter than the `/distance` setting when the finish line is closer.
- ELO is logarithmic: it grows by about 760 every time your points double, so new players climb fast and harder maps keep raising it. A single capture barely counts on its own. Rough guide, at a typical speed:

  | Play | ELO |
  | --- | --: |
  | 10 hours of Main ★★★ | 1000 |
  | 100 hours of Hard ★★☆ | 3700 |
  | 100 hours of Insane ★★★ | 5100 |
  | 100 hours of Extreme ★★★ | 6000 |
  | 300 hours of Extreme ★★★, 1.5 times as fast | 8000 |

  9999 takes 1000 hours of Extreme ★★★ at 8000 tiles per hour, all twice as fast as typical: completely inhuman.
- The category and stars come from the map's row in the `record_maps` table (`Server` and `Stars`). Maps of other categories, unrated maps (except Deranged, which has no stars) and maps missing there earn no ELO. `/help` shows what the current map earns.
- The capture message shows the ELO you gained and your speed factor, e.g. `(2 fails, +0.42 ELO, 1.3x speed)`.
- Reaching the flag frozen still captures it and scores in fights, but earns no ELO: `(0 fails, +0 ELO (frozen))`.
- Neither do attempts with kept weapons, also when the weapons were on for only part of the attempt: `(0 fails, +0 ELO (kept weapons))`. Weapons turned off again before the release don't count.
- In a fight, only the player capturing the flag earns ELO. Freeplay never does, and neither does a server where rcon cheats were used.
- Outside fights, the scoreboard and the server browser show each player's ELO as their score.

## Stats

Besides the ELO, the server counts these for every player name, on every map:

- **Attempts**: lives at a spot, from the release until a capture, death, `/r` or a new spot. Only attempts of at least 2 seconds count, not counting the hover or pauses, but captures always do.
- **Respawns**: spots given up by killing or dying after at least one attempt of 2 seconds there. Fight members dying respawn at the same spot, so only the fight leader's deaths count. `/r` is no respawn.
- **Captures**: flags captured.
- **Playtime**: time in the game, not paused or spectating.

Freeplay has no attempts, and rcon cheats stop everything but the playtime from counting. Changes are saved every minute, when leaving or renaming, on map changes and before showing any stats, so the ladders are always up to date.

## Fights

- Everyone in a fight trains at the same spot with the same flag. Joining a player who is already fighting merges both groups.
- Each capture scores a point and announces the standings. The first to reach the score limit wins the fight, and all scores reset.
- The fight's final standings show as a broadcast to everyone in it, and the next attempt hovers for 10 seconds instead of 1. Members respawning meanwhile hover until then too.
- The leader's death starts a new attempt for everyone. Other members respawn at the current spot.
- `/r` only retries the player who used it, the leader included.
- Fights show up as teams in the scoreboard, with fight points as scores instead of ELO. The leader is shown as `*name*`; the server browser keeps the real name.
- Leaving a fight resets your score. A fight with one player left ends and that score resets too.
- Paused and `/spec` players can fight; players in the spectator team cannot.

## Pathfinding

The walkable tile graph is built once when the map loads. Paths never go through solid, freeze or death tiles, never pass stoppers against their direction, and diagonal steps can't cut corners. The one way through freeze is falling straight down it onto unfreeze, with nothing but freeze and empty tiles in between: no hookable or unhookable tiles, stoppers, teleporters, speedups or deep freeze. Frozen tees can't steer, so these paths only go down, every tile fallen counts, and they can't be entered or left sideways. Teleporters count as path steps; checkpoint teleporters are skipped because they depend on each player's history. Every new flag stores its distance to all nearby tiles, so redrawing the path when you leave it costs no search. If you stray more than the goal distance plus 32 tiles behind your start, the path is hidden until you come back.

## Server setup

Build the server as described in the [DDNet building guide](docs/BUILDING.md) and start it with a Gores map that has a start and a finish line:

```sh
./DDNet-Server "sv_map <map>"
```

`sv_gametype` defaults to `gtrain`. The mod forces `sv_solo_server 1` and `sv_kill_delay 0`. Chat commands for races, teams and practice, like `/team`, `/practice` or `/save`, are removed from the chat and its command completion; rcon keeps them.

### Map downloads

Hover doors need a switch layer. The server adds an empty one to maps without it when loading them, so those are served as changed copies. To serve map files as they are, for example from an HTTPS map server with `sv_maps_base_url`, add it to the files once:

```sh
./map_add_switch_layer maps/*.map
```

The tool changes maps in place and skips maps that have a switch layer already. It adds the switch layer to the game group, where the map editor requires it, and moves switch layers from other groups there, like the group of their own that earlier versions added. A map server serves each map as `<map>_<sha256>.map`, and `sv_maps_base_url` is its address ending in `/`. Without one, clients download maps from the game server; keep `sv_map_window` at 25 or below then, since larger windows overflow the connection's resend buffer and can stall downloads.

| Setting | Default | Description |
| --- | --- | --- |
| `sv_gtrain_goal_distance` | 100 | Goal distance in tiles for players who didn't set `/distance` |
| `sv_gtrain_hover_door` | 1 | Hold hovering tees with a laser door; 0 uses zero gravity, which high ping clients mispredict at the release |
| `sv_gtrain_path_clear_radius` | 64 | Particle-free radius around the player in world units (32 per tile); 0 shows the whole path |
| `sv_gtrain_path_particle_spacing` | 192 | Distance between path particles in world units |
| `sv_gtrain_record_runs` | 0 | Store captures that earn ELO in the database, with the start state and inputs that replay them and the ELO and speed multiplier they earned |
| `sv_gtrain_start_exclusion` | 50 | Path distance in tiles from the start line in which no spots or flags are placed; maps entirely within it keep all their spots. Applies on map change |
| `sv_require_chat_flag_to_chat` | 0 | Set to 1 so new players must show the chat bubble before chatting during their first 20 seconds; commands are unaffected (from [ddnet-insta](https://github.com/ddnet-insta/ddnet-insta/commit/9b7c415b19510dc04157dfaa05596c077ca02ec6)) |
