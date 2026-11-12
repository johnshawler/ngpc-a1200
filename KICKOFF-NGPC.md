# Starting a new Neo Geo Pocket Color → Amiga 1200 port

## Before the session

1. On GitHub, make the new repository from **johnshawler/a1200-final** ("Use this template"). Don't work in
   a1200-final itself: it is the knowledge base every port starts from.
2. Start a Claude Code session with the **new** repository attached, and with read access to
   johnshawler/a1200-final if you want the session to bring its lessons back there at the end.
3. Upload the cartridge image (`.ngc`, or `.ngp` for a mono game) and, if you have it, the console's BIOS image
   (64 KB): the tools take the system's character set from it. Neither may be committed: the repository is
   public.
4. If the `amiga-ngpc-port` skill is installed in Claude (`skills/amiga-ngpc-port/SKILL.md`), it applies by
   itself; the first message below works with or without it.

## The first message (paste, then fill in the names)

```text
Port the Neo Geo Pocket Color game I uploaded (<file name>) to the Amiga 1200. The game is <name>; use
"<short_name>" as its name in the tools (lower case, letters and digits). The BIOS image is <file name, or
"not available">.

Before anything else read PLAYBOOK-NGPC.md (all of it, the lessons log too), PIPELINE.md, BRIEF.md and
CLAUDE.md. Then run tools/new_port_ngpc.sh <short_name> <cartridge image> [BIOS image] and follow the
playbook's first-day checklist.

Tell me early, before writing code:
- how much of the cartridge holds data, and whether that fits a stock A1200 at all;
- what the game uses that the tools do not model yet (the "uses:" line of tools/ngpref, and section 8 of the
  playbook): raster effects, sound samples, the serial link, saved games, mono mode;
- the load per scene for both processors (main CPU and sound program) and your verdict for a stock A1200 and
  for 4 MB fast RAM;
- how you would show the 160 x 152 picture on the Amiga screen (small, stretched or cropped): I decide.

Keep the images and everything made from them out of git. Commit and push as you go. Before finishing, write
REPORT.txt, add an entry to the playbook's lessons log, fold general lessons into the playbook, and open a pull
request against johnshawler/a1200-final that brings the improved playbook and tools back (not this game's
files).
```

## If the session stops or restarts

Everything that matters is in the repository: tell the new session "continue the port: read PLAYBOOK-NGPC.md,
PIPELINE.md, NOTES.md and the git log first, then carry on from the last commit".

## What to expect

- Working now: looking at a game on the reference machine, measuring it, translating its code to C and to 68k
  assembly and proving the translation, building and running it on the emulated Amiga with a correct picture
  (for Neo Turf Masters 4.5 new pictures a second of the 13.7 the game makes) and the music and effects of its own sound program, recorded.
- Not there yet (PLAYBOOK-NGPC.md section 8): a display that holds 50 Hz and full game speed. The first game the
  tools ran, Neo Turf Masters, needs fast RAM for its data alone and keeps 95% of game time.
  A port is real work over several sessions, and the first ones will spend much of it on the tools.
