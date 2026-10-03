# LabTwin Unified Portal Tooling

> Status: ACTIVE. This README is the frontend development, test and build reference for `tools/labtwin-dashboard`. It does not define the product’s current management capability, API ownership, security posture or target-board acceptance state.

The public module scope and acceptance boundaries are maintained in [Module overview](../../../docs/modules.md) and [Validation](../../../docs/VALIDATION.md). The Gemini-S1 board remains the source of LabTwin facts; browser cache and frontend views must not write them back as new truth. Original Portal code is licensed under Apache-2.0; dependencies retain their own licenses.

## Local Development

```powershell
cd D:\openvela\tools\labtwin-dashboard
npm install
npm run dev
```

Open `http://localhost:3000/` for the local development mode. Use only deliberately configured test/demo inputs; do not place credentials or production secrets in frontend configuration, browser storage or test fixtures.

## Test and Build

```powershell
npm test
npm run lint
npm run build
```

For the board static frontend target:

```powershell
npm run dev:board
npm run build:board
```

`dist-board` is the frontend build output intended for firmware resources. Public rebuild and ROMFS synchronization are described in [BUILD.md](../../../docs/BUILD.md). No private image or flashing automation is included.

## Scope Boundaries

TASK-20261002-02 adds `board/**/*.test.tsx` to the unit runner. Run `npm run test:unit`, `npm run lint`, then `npm run build:board`. Agent operation tests cover server-mode password requirements, exact-ID confirmation and cancellation; recorder tests preserve native audio elements across polling. These are browser component tests, not physical microphone or full board HTTP tests.

Only the Integration Owner runs `Sync-BoardPortalToVm.ps1`. It uses `openvela-vm`, requires identical Windows/Ubuntu source commits and verifies every built file's SHA-256 before and after ROMFS copying. `-RemoteTarget` supports an explicit vendor worktree; no hashed bundle is edited by hand. The resulting ROMFS records `SOURCE_REVISION` and `RESOURCE_SHA256SUMS`; packaging must additionally verify the extracted IMG, and physical-page verification remains a post-flash gate.

- Keep PC/local development and the board static target explicit; shared frontend components do not make the browser an alternate backend.
- Treat test/build output, local logs, caches and `node_modules` as tooling artifacts according to the project ignore/commit rules.
- When frontend source changes current behaviour, data/API flow, or known limitations, update the Management Dashboard module document using the Task-ID rules in `DEVELOPMENT_GUIDELINES.md`; do not create a standalone portal changelog.
