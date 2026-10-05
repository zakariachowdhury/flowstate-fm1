# Factory Worlds

Every `*.world.json` here is compiled into the firmware by `tools/gen_worlds.py` (run by `tools/build.py`, also
with `--gen-only`) and listed on the device sorted by category, then name. A World that fails `tools/worldc.py
check` fails the build. With no World here the build still works (`WORLD_NFACTORY 0`).

How to write one: [docs/worlds.md](../../docs/worlds.md). The binary format: [docs/design/fwd1-format.md](../../docs/design/fwd1-format.md).
