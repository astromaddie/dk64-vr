# Public VR dependency overlays

These patches carry the VR-specific RT64 and Plume source integration while keeping required submodules on public upstream commits.

- RT64: [rt64/rt64](https://github.com/rt64/rt64), base `43373749dac9bbc1b653e6a02aed40a9e1783bed`, under its upstream MIT license and notices.
- Plume: [renderbag/plume](https://github.com/renderbag/plume), base `d890ac899e505fb30040e037a4037cdeca68f033`, under its upstream MIT license and notices.
- `rt64-vr.patch` and `plume-vr.patch` preserve upstream license and notice files. Checksums and application steps are enforced by `tools/vr/prepare-public-dependencies.sh`.

Apply only to the exact clean public bases. The bootstrap refuses modified dependency worktrees.
