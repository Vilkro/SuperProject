# About this fork

This is a modded fork of [DreamyCecil's Serious Sam Classics Patch](https://github.com/SamClassicPatch/SuperProject),
used to run community Serious Sam Classic (TFE/TSE) dedicated servers. It adds a server-management
layer on top of the upstream patch: a persistent player database (PlayerDB), IP geolocation
(GeoIP), automated demo recording (DemoManager), a public-server browser (PlayersBrowse), delayed
script scheduling (ScriptScheduler), a work-in-progress rocket-jump tracker (Tracking), plus
multilingual chat, map/player voting, and extra sandbox/world-editing tools.

All changes are marked `// 1111` in the source for easy diffing against upstream.

**For the full write-up of what's added and why, and the server configs/dashboard that use it,
see [Vilkro/ServerUpgrade](https://github.com/Vilkro/ServerUpgrade).**

Built on top of, and would not exist without, DreamyCecil's Classics Patch — see the original
`README.md` below for the full upstream feature set, project layout, and build instructions.
