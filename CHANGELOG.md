# Changelog

## 2026-10-11 — PR0 readiness preparation

- Added experimental Steam Frame status, readiness evidence and a public ROM-free VR host-test workflow.
- Replaced the private RT64 pin with its public upstream base plus SHA-256-pinned RT64/Plume VR source overlays.
- No public build or release was made. Full native builds still require a locally supplied game ROM.

## 2026-10-11 — Current-source PR0 verification

- Updated the tested source to `17795fe5301a403553b01cb21e2344ce50be020b`; ROM-free production-source CMake/CTest passed 1/1 locally with public pinned dependencies.
- The latest exact-source GitHub job did not start because of the account billing/spending-limit block; the earlier successful run is retained in PR0_METADATA.json.
- No full ARM64 application build was run because the scoped project/build inventory contained no supported game ROM.

## 2026-10-11 — Public experimental source

- Made the repository public and confirmed the ROM-free GitHub validation passes without private secrets or game data.
