Author: rita-rev

## Plan Review Report

**PR:** none — plan review only
**Issue:** #107 — In the Dashboard table "Scores — All Time" add a new column "Last seen"
**Branch:** `dev`
**Reviewer:** rita-rev
**Date:** 2026-07-01

### Verdict
CHANGES REQUESTED

### Summary
The proposed implementation direction is mostly correct: it identifies the required backend join, the frontend contract change, and the need for API documentation plus automated coverage. Approval is withheld because the referenced plan artifact is missing from the repo, and the frontend styling step is not yet specific enough to prevent a regression in the existing shared timestamp styling.

### Documentation Check
- `README.md` — not needed
- API reference — required; the plan correctly includes an update to `docs/api.md`
- `CHANGELOG.md` — not needed (no changelog file in repo)
- `docs/architecture/` — not needed (directory does not exist)
- Inline code docs — not needed
- `.env.example` — not needed

### Findings

#### MUST FIX — blocking (approval withheld until resolved)
1. `docs/superpowers/plans/2026-07-01-scores-last-seen-column.md` — the developer comment identifies this file as the canonical plan artifact, but it is not present anywhere in the worktree or tracked files. A plan review cannot approve a missing artifact. Commit the plan file that is being routed for review so the execution document is durable and reviewable.
2. [frontend/index.html](/Users/john/git/agents/rita-rev/puzzpool/frontend/index.html:509), [frontend/src/dashboard.ts](/Users/john/git/agents/rita-rev/puzzpool/frontend/src/dashboard.ts:569), [frontend/src/dashboard.ts](/Users/john/git/agents/rita-rev/puzzpool/frontend/src/dashboard.ts:596) — the planned scope says stale/null score cells will be styled white "via `frontend/index.html`", but `.td-time` is a shared class already used by both `Visible Workers` and `Keys Found`. Unless the plan explicitly requires a score-specific modifier/class, it leaves room for a global `.td-time` change that would alter out-of-scope tables. Update the plan to require a dedicated score-table timestamp class or equivalent targeted styling, plus regression coverage that existing worker/finder timestamp rendering stays unchanged.

#### SHOULD FIX — non-blocking but important
1. [frontend/src/format.ts](/Users/john/git/agents/rita-rev/puzzpool/frontend/src/format.ts:49) — the planned helper coverage mentions null and invalid timestamps, but the acceptance rule hinges on the exact one-hour cutoff. Add an explicit boundary test for "exactly 1 hour old" versus "older than 1 hour" so the implementation cannot drift on the threshold.

#### NIT / SUGGESTION — optional
1. Consider placing the backend stats-payload regression in `tests/test_visualization.cpp`, which already owns `handleStats` response-shape coverage, instead of expanding the generic validation file further.

### Test Review

**Test suite result:** not run — this work package is a plan review with no implementation changes to execute.

**Test files reviewed:**
- `tests/test_handler_validation.cpp` — existing backend stats coverage is light; the planned matching-worker and missing-worker cases are directionally correct, but the plan should also lock down the exact one-hour frontend boundary separately.
- `frontend/src/format.test.ts` — current coverage only exercises `esc()`, so adding dedicated recency-helper tests here is appropriate.
- `frontend/src/accessibility.test.ts` — current tests are static source assertions; this is a reasonable place to assert the new score-table header/empty-state shape, but not sufficient by itself to protect shared `.td-time` behavior unless the selector strategy is explicit.

**Coverage assessment:**
- Well covered: planned backend cases for a scored worker with a matching `workers` row and one without a row; planned frontend handling for null and invalid timestamps; planned empty-state colspan update.
- NOT covered (missing tests): an explicit exact-60-minute threshold test, and a regression assertion that `Visible Workers` / `Keys Found` keep their existing shared `.td-time` styling while only the new score cells switch to white when stale/null.

**Test quality findings:**
1. MUST FIX — the plan does not currently require a regression test that protects the shared `.td-time` usage in non-score tables.
2. SHOULD FIX — the plan should include an exact-threshold recency test, not just null/invalid cases.

**Overall test verdict:** Inadequate until the plan commits the missing artifact and closes the shared-style and threshold-coverage gaps above.

### Security Assessment
No security concerns identified in the planned change surface.

### Performance Notes
No material performance concerns identified. Joining `workers.last_seen` into the score payload is low-risk at this scale, provided the existing score aggregation and ordering remain intact.

### Positive Observations
1. The plan correctly recognizes this is not a frontend-only task and that `scores[]` currently lacks any `last_seen` data path from `workers`.
2. The plan includes API documentation work up front instead of treating the response-shape change as self-evident.
3. The planned null-handling (`—`) matches the issue refinement and the real possibility of historical scored workers no longer existing in `workers`.

### Required Next Steps for Developer
1. Commit the missing plan file referenced in the issue comment so the review target actually exists in the repository.
2. Amend the frontend portion of the plan to require score-specific timestamp styling rather than any shared `.td-time` change, and add regression coverage for existing worker/finder timestamp behavior.
3. Add an explicit one-hour boundary test to the planned frontend recency-helper coverage.
