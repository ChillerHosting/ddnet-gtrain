# GTrain

A server-side [DDNet](https://github.com/ddnet/ddnet) mod for training Gores maps.

On every spawn you are placed at a random spot between the start and the finish line. You hover frozen for one second, then you play. Kill to get a new random spot.

## Usage

Build the server as described in the [DDNet building guide](docs/BUILDING.md) and start it with a Gores map:

```sh
./DDNet-Server "sv_map <map>"
```

`sv_gametype` defaults to `gtrain`. The mod forces `sv_solo_server 1` and `sv_kill_delay 0`.

The map needs a start and a finish line, otherwise training is disabled.
