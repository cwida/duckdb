# Out-of-Tree Extension Patches

This file applies to work in `.github/patches/extensions`. Read it before adapting an out-of-tree extension to a DuckDB change, updating an extension's pinned revision, or adding, changing, or removing an extension patch.

## Establish the Patch Base

- Find the extension's `duckdb_extension_load` declaration under `.github/config` and use its exact `GIT_URL` and `GIT_TAG`. Do not create a patch against another branch, tag, or checkout.
- Work from a clean checkout of that `GIT_TAG`. Apply every existing patch for the extension in lexicographic filename order before making a new change. A new patch must be based on the result of all patches that precede it.
- Patch paths are relative to the root of the extension repository, not the DuckDB repository. Generate a raw unified diff with `a/` and `b/` paths that works with both `patch -p1` and `git apply`. Do not use `git format-patch` or include its mail headers.
- Keep each patch focused on one coherent compatibility change. Exclude unrelated formatting, generated-file churn, build artifacts, and other working-tree changes.

## Name and Place Patches

- Store patches in `.github/patches/extensions/<extension>/`.
- Name every new patch `NNNN-short-description.patch`, where `NNNN` is a four-digit, zero-padded sequence number such as `0001` or `0012`.
- Patches are applied in lexicographic filename order. Normally choose the next number after the highest existing numeric prefix so the new patch is applied last. Ensure any dependent patches sort after their prerequisites.
- Use a unique, descriptive filename. Do not imitate legacy unnumbered names or names prefixed with `zzz`; those predate the numbered convention.
- Do not put README files, notes, or any other auxiliary files inside an individual extension directory. The patch application script rejects every entry there that does not end in `.patch`. This `AGENTS.md` belongs in the shared parent directory for that reason.

## Enable Patch Application

- When adding the first patch for an extension, add `APPLY_PATCHES` to the relevant `duckdb_extension_load` declaration. Without it, the patch directory is ignored.
- Keep `APPLY_PATCHES` present for as long as the extension has patches. When removing the final patch, remove `APPLY_PATCHES` as part of the same change. `make extension-patch-check` rejects both patches without the flag and the flag without patches.
- If the extension has multiple configuration branches or load declarations, ensure every applicable declaration that can fetch the patched revision enables patching.

## Supported Local Workflow

The sync workflow keeps existing patches as commits on top of the configured `GIT_TAG` and can export those commits back to patch files:

```shell
DUCKDB_EXTEND=<extension> make sync_out_of_tree_extensions
cd extension/external/<extension>
# Make and verify one focused change.
git add -A
git commit -m 'NNNN-short-description.patch'
cd ../../..
EXPORT_EXTENSION_PATCHES=1 DUCKDB_EXTEND=<extension> make sync_out_of_tree_extensions
```

- Add `APPLY_PATCHES` before the initial sync when creating an extension's first patch.
- The commit subject must exactly match the intended patch filename: one word, ending in `.patch`, unique among the patch commits, and lexicographically later than the preceding commit subject. Use one commit per patch.
- The export rewrites each patch from its corresponding commit. Review all exported files and confirm that earlier patches did not change unexpectedly.
- As an alternative, clone `GIT_URL`, check out `GIT_TAG`, apply existing patches in sorted order, make the focused change, and write the clean `git diff` to the new patch path.

`FORCE_APPLY_PATCHES=1` hard-resets and cleans the synced extension checkout before reapplying patches. It discards local extension work, so use it only when that destruction is intended. `DUCKDB_SKIP_APPLYING_PATCHES=1` is a temporary local debugging bypass; it is not a fix and must not be used to declare the patch work complete.

## Update or Retire Patches

- When changing `GIT_TAG`, start from the new pinned revision and reapply every remaining patch in filename order. Rebase or regenerate patches that no longer apply cleanly.
- Delete patches whose changes are included upstream. Preserve the relative order of patches that remain and remove `APPLY_PATCHES` if no patches remain.
- Do not squash or regenerate unrelated existing patches merely to add a new one.

## Validate the Change

- Build the touched extension and run its relevant tests against the patched checkout. A typical targeted build starts with `DUCKDB_EXTEND=<extension> make reldebug`.
- Run `make extension-patch-check`.
- Run `git diff --check` and inspect every changed or newly generated patch before finishing.
