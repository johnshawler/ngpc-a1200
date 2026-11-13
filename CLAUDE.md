# Amiga 1200 port

Your instructions are in `BRIEF.md`. Read all of it before you do anything else, then follow it exactly.

- The game to port is in `original/`. Your code goes in `src/`, converted data in `assets/`, your own host
  scripts in `tools/`. Copy `kit/main.c` to `src/main.c` and edit the copy.
- Never change `kit/` or the kit's tools (`tools/build.sh`, `bench.sh`, `run_uae.sh`, `gfx.py`).
  `tools/check_kit.sh` and CI fail if you do.
- Run `tools/setup.sh` once. Describe your port in `port.conf`. Then `tools/port.sh` builds it, benches it on
  the emulated stock A1200, runs your host test and checks the gates. The port is done only when it prints
  `PASS`. Never claim a frame rate that `tools/port.sh` did not print.
- NES (or other 6502) ports: read `PLAYBOOK-NES.md` first and reuse its tools. Knowledge about one game goes in
  `tools/games/<game>.py`; before finishing, add an entry to the playbook's lessons log and fold general
  lessons into its sections. That file is how the next port starts better than this one.
- Neo Geo Pocket (Color) ports (TLCS-900/H): read `PLAYBOOK-NGPC.md` first, all of it, and reuse its tools
  (`tools/new_port_ngpc.sh`, `tools/ngpref`, `tools/recomp900.py`, `src/ngpc/`). The port's sources are in
  `src/ngpc/` (its `main.c` is the kit skeleton already). Before finishing, add an entry to that playbook's
  lessons log, fold general lessons into its sections and take what you built out of its "not done yet" list.
- Work in the order of `PIPELINE.md` (milestones reprioritised from what this port found; its backlog lists the
  next items for this port). Read `LEARNINGS.md` before changing the translator or the display.
- Keep `NOTES.md`: one line for every place the brief was unclear, wrong or missing something. Not for departures:
  see below.
- When you finish, or are stuck at a gate after real attempts, write the PORT REPORT in the brief's format
  to `REPORT.txt`.

## Staying on the brief (checked by `tools/check_rules.py`, which `tools/port.sh` and CI run)

- Never depart from the brief. If you believe you must (another class, a route it does not list, something under
  "Never do these"), stop and ask the user first, and wait for the answer. `NOTES.md` records gaps in the brief;
  it never records a departure you made on your own.
- Milestone 1 is a file: `FEASIBILITY.md`, with the row of the Step 1 table you use, the `DispCfg`, the colour count,
  every object with its size, the budget sum and what will be cut. Write it before any game code and keep it up to
  date when the plan changes; `tools/port.sh` builds nothing without it. (The user dropped the approval step for the
  plan: no checksum in `APPROVALS` is needed.)
- `APPROVALS` and `tools/approve.sh` belong to the user (the per-pixel class and exemptions). Never write, edit or
  delete `APPROVALS`, never run `tools/approve.sh`, and never work around `tools/check_rules.py`.
- `CLASS=perpixel` (the soft gates) counts only when the user approved it in `APPROVALS`; otherwise the port is judged
  as planar. Never choose it to get a PASS.
- Every progress report quotes, as `tools/port.sh` printed them: the class, the rules result and the gate lines
  (`late=`, `worst_pct=`, `shown_fps=`, `checksum=`), then PASS or FAIL. Failures first, never a softer number in
  their place.

@BRIEF.md
