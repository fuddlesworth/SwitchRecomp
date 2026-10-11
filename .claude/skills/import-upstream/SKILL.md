---
name: import-upstream
description: "Bring a new MZ Labs SwitchRecomp release into this fork: import the zip onto the upstream branch (as released, then formatted), then port what it changed by hand, since this fork reorganized the code and no longer merges. Use when the user has a new SwitchRecomp release zip or asks to update from upstream."
---

# Import an upstream release

1. **Import.** `scripts/import-upstream.sh ~/Downloads/SwitchRecomp_vN.zip vN`. It commits the release as released
   (tagged `upstream-vN`) and then formatted with `scripts/format.sh`, on the `upstream` branch only. The working
   tree and `main` are untouched. It needs clang-format 23, like every format.
2. **Read what changed.** `git diff upstream~2 upstream --stat` (the formatted previous release against the
   formatted new one), then the files our fixes touch (`git log --format=%s upstream..main`). A release can fix the
   same bug differently, and then our commit should go.
3. **Port.** This fork is a hard fork (decided 2026-10-11): the runtime is reorganized into directories, the big files
   are split and the recompiler is a package, so `git merge upstream` no longer applies. Go through the diff from
   step 2 change by change, find where that code lives now (`docs/ARCHITECTURE.md`, `git log --follow`), and port
   what still matters, one commit per change, each saying which upstream file and release it comes from. Skip what
   our code already does differently, and say so in the PR.
4. **New third-party binaries.** If the zip added prebuilt libraries, they stay out: add a submodule and a step in
   `scripts/bootstrap.py` instead. If it added a new top-level Windows toolchain folder, add it to the excludes in
   `scripts/import-upstream.sh` and `.gitignore`.
5. **Rebuild and verify.** Bootstrap if `third_party/` changed, build both presets, and run the game
   (`swr-run`, a few runs). If the port changed the recompiler, anything in `runtime/abi/` or `SWR_ABI`, recompile the
   game first.
6. **Open the PR** once verified, and update ROADMAP.md if the release changed what is done or planned.

The release is the source of truth for its own code: never edit the `upstream` branch by hand.
