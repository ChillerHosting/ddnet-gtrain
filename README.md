# GTrain

A server-side [DDNet](https://github.com/ddnet/ddnet) mod for training Gores maps.

On every spawn you are placed at a random spot between the start and the finish line. You hover frozen for one second, then you play. Kill to get a new random spot. Use `/retry` or `/r` to restart at the same position with the same goal, initial freeze and a fresh timer.

Each player gets a personal blue CTF flag, placed 100 shortest-path tiles away by default. Gun trail particles trace the path toward the flag during the initial freeze and throughout the attempt. A configurable fixed radius keeps space around the player clear. Turns are rounded within traversable tiles. Collecting the flag plays the CTF capture sound and immediately places you at a new training spot with a new goal. Solo captures announce the capture time to the server. The capture timer starts when the initial freeze ends and excludes pauses.

Use `/fight <player name>` to train together at the same position with the same goal. Joining a player already fighting links everyone into one group; joining two existing groups merges them. The leader's death or any member's capture starts a new attempt for everyone in the group. Other members' deaths affect only themselves; they respawn at the current attempt's start with the same goal. Each capture adds one win for the finisher and announces everyone's fight scores, without a finish time. Deaths and retries keep the standings; leaving clears your wins. The leader can use `/retry` or `/r` to retry the current goal for the whole group; other members retry only themselves. Use `/fight` without a name to leave.

`/team` and `/practice` are disabled in GTrain to keep fight groups intact.

Use `/freeplay` to toggle training without flags, path particles or captures. In a fight group, only the leader can toggle it, and it applies to the whole group. Joining a group adopts its mode; leaving keeps that mode for your solo play. Turning freeplay off starts a fresh training attempt. Random training spawns and `/retry` remain available in freeplay.

Fight groups appear together as teams in the scoreboard. The leader has scoreboard score 1 and everyone else has 0. Joining keeps the target group's leader; when the leader leaves or disconnects, another member becomes leader.

The traversable tile graph is built once on map load. A bounded breadth-first search chooses a goal at the requested path distance and supplies the initial route. Players reuse that route while moving along it; deviations use A* with shared scratch buffers, limited to five searches per second per player. Visit stamps avoid clearing the map on each search. A* uses Chebyshev distance on maps without teleports and a zero heuristic when teleports could shortcut that distance. No flowfield is constructed. Snapshots draw only visible particles, capped at 256 particles and 256 route tiles per player. Diagonal steps require both adjacent side tiles to be traversable, preventing corner cutting through solid or freeze tiles. Paths avoid solid, freeze and death tiles, and ordinary/evil teleports are directed path steps with no particle line across the jump. Checkpoint teleports are excluded because their destinations depend on character history. If no eligible goal exists at the requested distance, the farthest eligible goal within that distance is used.

## Usage

Build the server as described in the [DDNet building guide](docs/BUILDING.md) and start it with a Gores map:

```sh
./DDNet-Server "sv_map <map>"
```

`sv_gametype` defaults to `gtrain`. The mod forces `sv_solo_server 1` and `sv_kill_delay 0`.

Training settings:

- `sv_gtrain_goal_distance 100`: goal distance in eight-neighbor pathfinding steps; each cardinal or diagonal step counts as one tile, applied to new attempts.
- `sv_gtrain_path_clear_radius 64`: fixed particle-free radius in world units (64 is two tiles), independent of player speed. Changes apply immediately. Set to 0 to show the entire visible path.
- `sv_gtrain_path_particle_spacing 96`: distance between path particles in world units (96 is three tiles). Changes apply immediately. A particle also marks the route's endpoint outside the clear radius.

The map needs a start and a finish line, otherwise training is disabled.
