# GTrain

A server-side [DDNet](https://github.com/ddnet/ddnet) mod for training Gores maps: practice random parts of a map over and over, alone or against friends.

## How it works

- Every spawn puts you at a random spot between the start and the finish line, always above freeze and more than 50 path tiles past the start line, since map starts are usually easy. Spots and flags are never farther from the start line than the finish line is, or farther from the finish line than the start line is, so areas behind either line and long dead ends are left out. You hover frozen for one second, then play.
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
| `/team <player>` | Ask a player to play together in a [team](#teams); `/team` alone leaves it |
| `/accept`, `/a` | Join the team of the last player who asked you, within a minute |
| `/distance <50-1000>` | Your goal distance in tiles, at least 100 on Deranged maps; `0` uses the server default |
| `/nodj` | Toggle starting attempts without double jump; touching the ground gives it back |
| `/forwards` | Toggle goals that only lead towards the finish line, see [below](#forwards) |
| `/scorelimit <0-1000>` | Points needed to win a fight (default 10, `0` for no limit) |
| `/shotgun`, `/grenade`, `/laser` (or `/rifle`), `/ninja` | Toggle keeping that weapon in every attempt; ninja never runs out. Captures with kept weapons earn no [ELO](#elo) |
| `/freeplay` | Toggle training without flags, paths and captures. `/tc` or `/telecursor` starts you on the tile you aim at, and `/r` returns there; in `/spec` or `/pause`, it starts you at the middle of your view and unpauses you, so the start can be anywhere on the map. In a fight, the leader chooses the start of everyone |
| `/trail` | Toggle showing the particle trail to the flag; it is your own choice, also in fights and while spectating |
| `/settings [player]` | Show your settings or another player's; in a fight, the leader's settings that apply to it |
| `/points`, `/elo [player]` | Show your ELO and rank, or another player's |
| `/top5`, `/top`, `/top5points`, `/top5elo [rank]` | Show five players of the ELO ladder, starting at the rank (1 by default) |
| `/rank`, `/profile [player]` | Show your [stats](#stats) and playtime, or another player's |
| `/top5captures`, `/top5attempts`, `/top5respawns`, `/top5playtime [rank]` | Show five players of these ladders |
| `/help` | List these commands, plus `/map`, `/mapinfo` and `/cmdlist` |

`/distance` and `/scorelimit` show the current value without an argument. In a fight or a team, only the leader can change these settings, and they apply to the whole group. Leaving a fight keeps its settings, and joining one adopts its settings, including removing weapons it has disabled.

### Forwards

Goals normally lead in any direction from your spot, also back towards the start. With `/forwards`, every goal lies on a shortest path from your spot to the finish line, so each run goes the way the map is played. Spots closer to the finish than your goal distance are skipped then, so goals keep their full distance; on maps shorter than it, goals lead from the spots farthest from the finish. Like `/distance`, it applies to new goals, and retries keep the current one.

## ELO

Every capture earns points, and your ELO shows them on a scale from 0 to 9999. Both only grow, unless an admin [bans runs](#banning-runs), and players only see the ELO, the points stay internal. They are stored per player name in the server's database, like DDNet points.

- A capture earns the points of every rated tile you went through unfrozen, each once, by how hard that tile is (see [tile ratings](#tile-ratings)): its difficulty comes out as a category and stars, which earn 100 per tile at Easy ★☆☆ and 1.5 times as many for each step above that, also between whole steps. Hard parts earn more than easy parts of the same capture, and tiles passed frozen earn nothing. These are the points per tile of a 100 tile goal, of which rated tiles earn half: captures go through more tiles than their goal is long, and adding up the tiles makes the hard ones count for more than an average would.

  | | ★☆☆ | ★★☆ | ★★★ |
  | --- | --: | --: | --: |
  | Easy | 100 | 150 | 225 |
  | Main | 338 | 506 | 759 |
  | Hard | 1139 | 1709 | 2563 |
  | Insane | 3844 | 5767 | 8650 |
  | Extreme | 12975 | 19462 | 29193 |
  | Deranged | 43789 | 65684 | 98526 |

  Deranged goes on above Extreme without an end: its stars go as far as the tiles' difficulty does, each one worth 1.5 times the one before (★★★★: 147789). Deranged maps keep their own rules: goals aim for at least 100 tiles, also when the server default is shorter, and speed doesn't count, see below.

- Longer goals are worth more than their length, since every tile is another chance to fail: each tile earns the square root of the goal's distance over 100 times the table's points, so twice the distance, with twice the tiles, earns about 2.8 times as many. Per tile, 50 tile goals earn 0.71 times the table's points, 200 tiles 1.41 times and 500 tiles 2.24 times.
- That is multiplied by your speed compared to a typical one for the capture's difficulty, the mean of its tiles, from 12 path tiles per second on Easy down to 6 on Extreme ★★★ and on to at least 2 above it (a bit below the speeds of the first captures on the KoG maps). Faster captures earn up to 4 times as much, slow ones still earn at least half. Only the time of the successful attempt counts. Deranged maps have no speed multiplier, every capture there counts as `1.0x speed`.
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
- Whether a map earns ELO comes from its category in the `record_maps` table (`Server`): Easy, Main, Hard, Insane, Extreme and Deranged maps do, whatever their stars; other categories and maps missing there don't. Without tile ratings, for example with `sv_gtrain_tile_ratings 0`, the map's category and stars (`Stars`) decide the difficulty instead: every tile of the goal's distance earns at that step. `/help` shows what the current map earns.
- The capture message shows the ELO you gained, your speed factor and the mean difficulty of its tiles, e.g. `(2 fails, +0.42 ELO, 1.3x speed, Hard ★★☆)`.
- Reaching the flag frozen still captures it and scores in fights, but earns no ELO: `(0 fails, +0 ELO (frozen))`.
- Neither do attempts with kept weapons, also when the weapons were on for only part of the attempt: `(0 fails, +0 ELO (kept weapons))`. Weapons turned off again before the release don't count.
- In a fight, only the player capturing the flag earns ELO. [Teams](#teams) share theirs. Freeplay and servers where rcon cheats were used never earn any.
- Outside fights, the scoreboard and the server browser show each player's ELO as their score.

## Stats

Besides the ELO, the server counts these for every player name, on every map:

- **Attempts**: lives at a spot, from the release until a capture, death, `/r` or a new spot. Only attempts of at least 2 seconds count, not counting the hover or pauses, but captures always do.
- **Respawns**: spots given up by killing or dying after at least one attempt of 2 seconds there. Fight members dying respawn at the same spot, so only the fight leader's deaths count. `/r` is no respawn.
- **Captures**: flags captured.
- **Playtime**: time in the game, not paused or spectating.

Freeplay has no attempts, and rcon cheats stop everything but the playtime from counting. Changes are saved every minute, when leaving or renaming, on map changes and before showing any stats, so the ladders are always up to date.

## Fights

- Everyone in a fight trains at the same spot with the same flag. `/fight <player>` joins that player's fight; if you are in another one, only you switch, and the rest of your old fight goes on without you.
- Each capture scores a point and announces the standings. The first to reach the score limit wins the fight, and all scores reset.
- The fight's final standings show as a broadcast to everyone in it, and the next attempt hovers for 10 seconds instead of 1. Members respawning meanwhile hover until then too.
- The leader's death starts a new attempt for everyone. Other members respawn at the current spot.
- `/r` only retries the player who used it, the leader included.
- Fights show up as teams in the scoreboard, with fight points as scores instead of ELO. The leader is shown as `*name*`; the server browser keeps the real name.
- Leaving a fight resets your score. A fight with one player left ends and that score resets too.
- Paused and `/spec` players can fight; players in the spectator team cannot.

## Teams

Teams play together instead of solo: teammates collide, hook and hit each other, and share the map's switches, like a DDRace team.

- `/team <player>` asks a player to team up, `/accept` or `/a` joins within a minute. Members can ask more players, who join the same team. The one who asked first leads it, shown as `*name*`, and chooses the [settings](#commands) everyone plays with.
- Everyone hovers at a spot of their own near the leader's, at least two tiles apart and no closer to the goal where the map allows, and returns there with `/r`. The goal has a flag for each member, spread around it the same way, all blue.
- Each member takes one flag, any of them, and plays on to help the others: `'name' took a flag, 1 left.` Once every flag is taken, everyone earns their capture and the team starts at a new spot together: `'name' captured a flag for their team in 3.20 seconds (0 fails, +0.21 ELO, 1.3x speed, Hard ★★☆)`.
- Each member earns [ELO](#elo) like a capture alone, by the tiles they went through unfrozen and their time, both until they took their flag, divided by the number of members: a team earns what one player would. Being carried frozen earns nothing for those tiles; being pulled along by a teammate's hook counts. A member taking their flag frozen earns nothing, but the flag still counts for the team.
- Any member's death, kill or `/r` acts for the whole team, also after taking a flag: nobody earns anything from that attempt, and the team starts over at a new spot after deaths, at the same one after `/r`.
- `/team` alone leaves it, and the others start over at a new spot. A team with one player left ends, so does a member's leaving for the spectators.
- Teams and [fights](#fights) don't mix: fights race solo, so a team can't fight and a fight can't team up.

## Pathfinding

The walkable tile graph is built once when the map loads. Paths never go through solid, freeze or death tiles, never pass stoppers against their direction, and diagonal steps can't cut corners. The one way through freeze is falling straight down it onto unfreeze, with nothing but freeze and empty tiles in between: no hookable or unhookable tiles, stoppers, teleporters, speedups or deep freeze. Frozen tees can't steer, so these paths only go down, every tile fallen counts, and they can't be entered or left sideways. Teleporters count as path steps; checkpoint teleporters are skipped because they depend on each player's history. Every new flag stores its distance to all nearby tiles, so redrawing the path when you leave it costs no search. If you stray more than the goal distance plus 32 tiles behind your start, the path is hidden until you come back.

## Tile ratings

Every spot of a map gets a difficulty from simulating tees there with [ddnet_physics](https://github.com/Teero888/ddnet_physics), the server's physics: 20 tees each, starting at 16 units per tick in a random direction, with random inputs held for 5 to 30 ticks. They aim their hook at something hookable in reach and hold it 70% of the time, and jump at a quarter of the inputs. A tee is done when it freezes, dies or a checkpoint teleporter sends it back, or after 100 ticks. The sooner, the harder: the survival is the logarithm of 100 over the mean ticks they lasted. How the spot is built counts too: in how many of 32 directions the hook would grab within reach and in how many it hits unhookable tiles first, how far the nearest wall, the freeze below and the ceiling are. Open spots are harder than their survival says, the more so the safer they seem: tees survive long there, but getting through takes hooks at the edge of their reach. All of it is weighted to fit the KoG ratings, in their steps. Tiles that are no spots have no rating.

A spot is rated from only 20 tees, so single spots come out far harder or easier than their surroundings. Each tile therefore takes the mean difficulty of the rated tiles within 3 tiles of it (a disc). On the replayed lain captures, this ranked the maps closer to their categories and stars than single tiles (Spearman 0.895 instead of 0.876 by the points the tiles earn, 0.845 instead of 0.812 among Insane and Extreme maps), and the 5 hardest tiles of a capture earn 18% of its points instead of 38%. A curve fitted to the KoG ratings turns each tile's difficulty into a category and stars (`GTrainElo::TileStep`), so the map's own rating isn't needed: the KoG ratings only set the scale. A capture earns every rated tile you went through unfrozen once, by its own step; tiles without a rating earn nothing. The mean difficulty of those tiles is the capture's difficulty, which its message shows and which sets its typical speed. See [ELO](#elo).

On the 664 rated KoG maps with a path from the start to the finish line, the mean difficulty along it ranks the maps like their categories and stars (Spearman 0.965, 0.816 among Insane and Extreme maps, cross-validated; the survival alone ranks them 0.954 and 0.774). The curve puts the maps 0.59 steps off their rating on average, 95% of them within 1.5 steps. Within a map, 100 tile stretches differ by about one step: the easiest parts of a typical Extreme map rate like Insane ★☆☆, its hardest like Extreme ★★★. Playing along the race, longer simulations, counting hopeless falls as freezing early and counting how often the tees get further along the race did not tell the maps apart better.

The ratings are saved per map version in `gtrain_ratings/<map>_<sha256>.gtr` of the server's save directory, and are made again when the spots or the simulation differ. Rating a map takes seconds on a few cores (the largest KoG map, with 306k spots, took 7 s on 7 threads), on threads of their own at the lowest priority; everyone spectates until it is done, with the progress broadcast. `gtrain_rate_tiles` makes them beforehand (`-o`), and compares maps with their ratings (`-r`): it writes the route from the start to the finish line with the simulated ticks of every spot on it. The route keeps away from walls and freeze where there is room, like players do, instead of hugging them like the shortest path.

```sh
./gtrain_rate_tiles [-j <threads>] [-e <start exclusion>] [-p <settings>] [-o <ratings> [-f]] [-r <route csv> [-w <wall weight>] [-i <image ppm>]] <map>
```

`-p` tries other inputs, like `-p aimpercent=50,startspeed=8` (see `CGTrainRatingSettings`), and the route file has the samples and features of every spot on it to fit the weights (`gtrain_tiles.cpp`) and the curve again. New defaults need a new `CGTrainTileRatings::VERSION`.

## Server setup

Build the server as described in the [DDNet building guide](docs/BUILDING.md) and start it with a Gores map that has a start and a finish line. The tile ratings need the `ddnet_physics` submodule: `git submodule update --init --recursive`.

```sh
./DDNet-Server "sv_map <map>"
```

`sv_gametype` defaults to `gtrain`. The mod forces `sv_solo_server 1`, `sv_no_weak_hook 1` and `sv_kill_delay 0`, but keeps the map's `player_collision` and `player_hooking` tunings for its teams: solo players never touch, and DDNet clients predict that from their solo flag. Chat commands for races, DDRace teams and practice, like `/practice`, `/save` or `/lock`, are removed from the chat and its command completion; rcon keeps them. `/team` is GTrain's own.

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
| `sv_gtrain_record_runs` | 0 | Store captures that earn ELO in the database, with the start state and inputs that replay them and the points, ELO and speed multiplier they earned |
| `sv_gtrain_start_exclusion` | 50 | Path distance in tiles from the start line in which no spots or flags are placed; maps entirely within it keep all their spots. Applies on map change |
| `sv_gtrain_tile_ratings` | 1 | Rate the spots of each map, see [tile ratings](#tile-ratings); with 0 the map's category and stars decide the ELO of its captures. Applies on map change |
| `sv_require_chat_flag_to_chat` | 0 | Set to 1 so new players must show the chat bubble before chatting during their first 20 seconds; commands are unaffected (from [ddnet-insta](https://github.com/ddnet-insta/ddnet-insta/commit/9b7c415b19510dc04157dfaa05596c077ca02ec6)) |

### Stored runs

Each row of `record_gtrain_runs` is one player's capture, with the points, ELO and speed multiplier it earned, `Ticks` from the release until the flag and the encoded run in `Run`: a format version, the raw size and the zlib-compressed data. Solo runs (version 1) are the tee's state at the release and the input applied in every tick until the capture. A team's run (version 2, `CGTrainTeamRun`) is stored whole in the row of every member who earned ELO with it: for each member their name, client id, ticks until their flag and a solo run's data from the shared release until the last flag, in the order the world moves the tees. Teammates collide, so replays put them in one DDRace team, spawn them in that order and apply the inputs by client id like the server does (`ReplayGTrainTeamRun`).

### Banning runs

Runs stored with `sv_gtrain_record_runs` can be shadow-banned, for example of a map that turned out too easy for its rating or of a cheating player. Banned runs stay in the `record_gtrain_runs` table with `Banned` set, so they can still be replayed, but the points and captures they earned are taken off the players' stats. Nobody is told.

| Rcon command | Description |
| --- | --- |
| `ban_runs <map> [player]` | Ban the runs on the map, `*` for every map, only those of the player if given |
| `unban_runs <map> [player]` | Lift the ban, giving the points and captures back |

The console logs how many runs, points and players it changed. Players on the server see their new ELO right away, those on other servers once they rejoin. Runs stored before runs had their points (before November 2026) only lose their capture, their points stay. Remove a map from the votes before banning its runs, and ban players while they are offline, so no new run is stored while the ban is written.
