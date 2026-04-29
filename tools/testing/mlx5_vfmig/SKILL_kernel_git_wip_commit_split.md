# SKILL: Split a large kernel WIP commit into a clean, bisectable series

Internal: not part of the upstream submission. Sibling to
`cursor_plans/`. Keep on the development branch; drop before any
subset of this work is sent upstream.

## When to use this

You have one oversized commit (the "WIP commit" -- typically 1k+
lines, often 5k+, touching many files) that has accumulated multiple
logical changes. The branch is otherwise clean and you want to:

- Convert the WIP into N atomic, independently-buildable commits,
  each implementing one logical change.
- Preserve the final tree state exactly, modulo files you explicitly
  drop (e.g. internal planning docs).
- Apply standardized commit trailers (`Signed-off-by`, `Assisted-by`)
  across every commit in canonical kernel order.
- Optionally strip internal subject prefixes (`L0:`, `L1:`, etc.).

Worked example: the mlx5_vfmig SAVE/LOAD work split a 5k-line
21-file WIP into three logically-grouped commits, on top of which a
further 11 follow-on commits (six L0..L3 kernel patches + five
internal tooling/doc patches) were cherry-picked, for a total
14-commit clean series with bit-identical tree state to the original
branch.

## Strategy

The technique is **"apply WIP changes, then progressively trim down"**,
not "build each commit from scratch". This is faster and ensures the
final tree state exactly matches the WIP's final state.

Three-stage flow:

1. Cherry-pick the WIP `--no-commit` to bring all changes into the
   index + working tree.
2. Save full WIP file copies aside so you can rehydrate at the end.
3. For each target commit in dependency order: trim files to the
   commit's intended state, build, stage, commit. The final commit
   restores from the saved copies, guaranteeing exact equivalence.

After all commits exist, run `git rebase -x` once to apply trailer
normalization (and optionally subject-prefix stripping) across the
whole series.

## Steps

### 1. Set up a fresh branch from before the WIP commit

```
git checkout -b <new-branch> <wip-sha>~
```

The original branch stays untouched as a safety net. Don't delete
it until you've verified the new branch's tree equivalence (step 11).

### 2. Apply the WIP commit's full contents

```
git cherry-pick --no-commit <wip-sha>
```

Verify with `git status`; every file the WIP touched should be staged.

### 3. Set aside files that don't belong in this WIP-derived series

Two sub-cases:

**Drop entirely** (e.g. stale planning docs):
```
rm -rf <path/to/dropped/files/>
```

**Belong in a later commit** (e.g. test harness, tooling, plan docs):
```
git rm --cached <path>           # remove from index
mv <path> .git/wip-staging/...   # park outside working tree if a
                                 # subsequent cherry-pick would
                                 # otherwise refuse to overwrite it
```

### 4. Save full-WIP-state copies of files that the LAST commit needs

```
mkdir -p .git/wip-staging/full
cp <each-file-the-final-commit-needs> .git/wip-staging/full/
```

These are the rehydration source for the final commit in the
breakdown. Without them you'd have to reconstruct the WIP's final
state from diffs, which is error-prone.

### 5. Trim files to commit-1 state and commit

For each file that needs partial content:
- Use `StrReplace` / manual edits to remove blocks that belong in
  later commits.
- Or write the file from scratch if commit 1 introduces it as a
  skeleton (vfmig.c was authored this way for the framework commit).

Then:
```
git add <files>
make <relevant-objects>     # build verification: NON-NEGOTIABLE
git commit -m "..."
```

### 6. Repeat for commits 2 .. N-1

Each intermediate commit ADDS the next layer's content to the files
on top of what commit-1 left them with. Build between every commit;
cherry-picks that apply cleanly often hide subtle build breakage
(e.g. a function declaration added in one commit but its caller
added in another).

### 7. For commit N, restore from saved copies

```
cp .git/wip-staging/full/* <destination/paths>
git add <files>
make <relevant-objects>
git commit -m "..."
```

This guarantees the final tree matches the WIP's final state
exactly. Verify with `git diff <wip-sha> HEAD -- <kernel-paths>`
which should show only the differences attributable to dropped
files.

### 8. Cherry-pick subsequent commits from the original branch

For follow-on commits that exist after the WIP on the original
branch:

```
git cherry-pick <followup-sha>
```

If a follow-on commit modifies a file you've intentionally dropped
or moved to a later commit, you'll get a modify/delete conflict.
Resolution pattern:

```
# save the cherry-picked version aside so you can apply it later
cp <conflicting-file> .git/wip-staging/<commit-id>/<file>
git rm -f <conflicting-file>           # mark conflict resolved-as-absent
git cherry-pick --continue
```

### 9. Add files for any commits that come after the WIP-derived ones

For internal tooling / docs / harness commits that wrap up the
series:

```
# pull from the original branch's final state
git checkout <original-branch> -- <path>
git add <path>
git commit -m "..."

# OR rehydrate from earlier saved copies
cp .git/wip-staging/<phase>/<file> <path>
git add <path>
git commit -m "..."
```

### 10. Apply trailer normalization

Write a small idempotent script that:
- Strips any auto-injected unwanted trailers (e.g. `Made-with: Cursor`)
- Strips any pre-existing copies of your canonical trailers
- Re-adds them in the canonical kernel order: `Assisted-by` BEFORE
  `Signed-off-by` (kernel convention; `git interpret-trailers`
  defaults to alphabetical, which puts them in the wrong order)

Example `.git/fix-trailers.sh`:

```bash
#!/bin/bash
set -euo pipefail

git log -1 --format='%B' \
  | sed -e '/^Made-with: Cursor$/d' \
        -e '/^Assisted-by: Cursor:/d' \
        -e '/^Signed-off-by: Raphael Norwitz/d' \
  | git interpret-trailers \
        --trailer 'Assisted-by: Cursor:claude-opus-4.7-high' \
        --trailer 'Signed-off-by: Raphael Norwitz <rnorwitz@nvidia.com>' \
  | git commit --amend -F -
```

Run across the whole series in one rebase:

```
chmod +x .git/fix-trailers.sh
git rebase -x .git/fix-trailers.sh <pre-WIP-sha>
```

Idempotent by design: re-running strips and re-adds the same
trailers and produces no new commits.

### 11. Strip internal subject prefixes (optional)

For commits with internal layer prefixes (`L0:`, `L1a:`, `L2:`, `L3:`)
that shouldn't go upstream:

Example `.git/strip-l-prefix.sh`:

```bash
#!/bin/bash
set -euo pipefail

git log -1 --format='%B' \
  | sed -e '1s/^mlx5_vfmig: L[0-9][a-z]*: /mlx5_vfmig: /' \
  | git commit --amend -F -
```

```
chmod +x .git/strip-l-prefix.sh
git rebase -x .git/strip-l-prefix.sh <pre-WIP-sha>
```

### 12. Verify tree equivalence

```
git diff --stat <original-branch> HEAD
```

Should show ONLY the files you intentionally dropped, with the
expected line counts. Anything else is a bug -- usually a forgotten
re-stage in step 6 or a bad merge in step 8.

### 13. Spot-check bisectability

```
# build the earliest commit in isolation -- most likely to have
# bisect issues since it's the most carved-down
git stash
git checkout <commit-1>
make <relevant-objects>
git checkout <branch-tip>
git stash pop                       # if anything was stashed
```

For very long series, also build a middle commit. Building the tip
is implied by step 7.

## Common gotchas

1. **Cherry-pick vs untracked working-tree files**: if a cherry-pick
   wants to write a file that's already present as untracked (e.g.
   from a prior `git rm --cached`), it fails with "untracked working
   tree files would be overwritten". Work around by `mv`'ing those
   files to `.git/wip-staging/` before the cherry-pick, then
   `mv`'ing back after.

2. **Modify/delete conflict on a later-commit file**: covered in
   step 8. Save aside, `git rm -f`, continue. Re-add in the right
   later commit.

3. **Forgetting to re-stage after edits**: `git cherry-pick --no-commit`
   stages files at their cherry-picked content. Subsequent edits to
   those files are NOT auto-staged. After every edit, explicit
   `git add <file>` -- or check `git diff` (unstaged) before commit
   and `git diff --cached` (staged) after.

4. **Trailer order**: kernel convention is `Assisted-by` BEFORE
   `Signed-off-by`. `git interpret-trailers` with default config
   alphabetizes, putting them in the wrong order. Always strip and
   re-add in the order you want via the normalization script.

5. **Auto-injected trailers**: tooling (Cursor included) may inject
   `Made-with: Cursor` or similar. The normalization script must
   strip these explicitly. After running, spot-check
   `git log -1 --format='%B'` on a sample of commits.

6. **Build between every commit**: cherry-picks that apply cleanly
   often hide subtle bisect-breakage. Building between commits
   catches "function declared in commit N, caller added in N+2" -- 
   N+1 is broken in isolation. Doing this incrementally is much
   cheaper than discovering it during review.

7. **Don't push until verification passes**: keep both branches
   local until step 12 confirms tree equivalence. Force-pushing a
   broken split is hard to recover from cleanly.

## References

- `Documentation/process/submitting-patches.rst` -- kernel commit
  conventions and trailer order.
- `git interpret-trailers --help` -- trailer manipulation primitives.
- The original mlx5_vfmig WIP split: `vf-save-restore-cleanup-2`
  branch, 14 commits derived from `vf-save-restore-cleanup-1`'s 11
  commits + 1 WIP. Tree equivalence verified by `git diff --stat
  vf-save-restore-cleanup-1 vf-save-restore-cleanup-2` showing only
  5 stale internal planning docs as the difference.
