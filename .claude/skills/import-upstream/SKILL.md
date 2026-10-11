---
name: import-upstream
description: "Bring a new MZ Labs SwitchRecomp release into this fork: import the zip onto the upstream branch (as released, then formatted), merge into main, resolve conflicts with our fixes, rebuild and re-verify. Use when the user has a new SwitchRecomp release zip or asks to update from upstream."
---

# Import an upstream release

1. **Import.** `scripts/import-upstream.sh ~/Downloads/SwitchRecomp_vN.zip vN`. It commits the release as released
   (tagged `upstream-vN`) and then formatted with `scripts/format.sh`, on the `upstream` branch only. The working
   tree and `main` are untouched. It needs clang-format 23, like every format.
2. **Read what changed.** `git diff upstream~2 upstream --stat` (the formatted previous release against the
   formatted new one), then the files our fixes touch (`git log --format=%s upstream..main`). A release can fix the
   same bug differently, and then our commit should go.
3. **Merge.** On a branch: `git switch -c merge-vN main && git merge upstream`. For each conflict, read both sides and
   the commit message of ours (`git log -L` on the region). Keep upstream's change unless ours fixes something it
   still gets wrong, and say which in the merge commit message.
4. **New third-party binaries.** If the zip added prebuilt libraries, they stay out: add a submodule and a step in
   `scripts/bootstrap.sh` instead. If it added a new top-level Windows toolchain folder, add it to the excludes in
   `scripts/import-upstream.sh` and `.gitignore`.
5. **Rebuild and verify.** Bootstrap if `third_party/` changed, build both presets, and run the game
   (`swr-run`, a few runs). If the release changed `codegen.py`, `a64.py`, `cpu.h`, `cpu_helpers.c` or the `ABI`
   number, recompile the game first.
6. **Merge into main** once verified, and update ROADMAP.md if the release changed what is done or planned.

The release is the source of truth for its own code: never edit the `upstream` branch by hand.
