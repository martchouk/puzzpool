Author: rita-rev

## Code Review Report

**PR:** #116 — fix: remove sticky-column visual treatment regression (closes #115)
**Issue:** #115
**Branch:** `fix/115-sticky-columns-background`
**Reviewer:** rita-rev
**Date:** 2026-07-07

### Verdict
CHANGES REQUESTED

### Summary
The change removes the separator/shadow treatment and brings sticky headers into scope as planned, but the hover implementation still drops the sticky cells' opaque fill. The current tests all pass, yet they only assert the presence of a hover overlay and miss the cascade interaction that reintroduces transparency on hovered sticky cells.

### Documentation Check
- `README.md` — not needed for this CSS/test change
- API reference — not needed
- `CHANGELOG.md` — not needed
- `docs/architecture/` — not needed
- Inline code docs — not needed
- `.env.example` — not needed

### Findings

#### MUST FIX — blocking
1. `frontend/index.html:532,581-584` — `tbody tr:hover td { background: rgba(255,255,255,0.02); }` still applies to sticky cells on hover and resets their `background-color` to a translucent fill. The new sticky hover rule only adds `background-image`, so hovered pinned cells no longer stay opaque and horizontally scrolled content can bleed through again, which regresses the stated AC-4 behavior. Fix this by preserving an opaque background color in the sticky hover state or by excluding sticky cells from the generic hover shorthand.
2. `frontend/src/accessibility.test.ts:124-128` — the rewritten hover assertion checks only for the `background-image` overlay and does not guard the final hovered sticky-cell background from the regression above. Add coverage that proves hovered sticky cells retain an opaque base, not just that the overlay rule exists.

#### SHOULD FIX — non-blocking but important
None.

#### NIT / SUGGESTION — optional
None.

### Test Review

**Test suite result:** After `npm ci`, `npm run typecheck`, `npx vitest run`, and `npm run build` all passed. Vitest reported 37/37 tests passing across 3 files.

**Test files reviewed:**
- `frontend/src/accessibility.test.ts` — reviewed the sticky-column regression coverage added for per-side backgrounds, separator removal, header tint, hover, and dim behavior.

**Coverage assessment:**
- Well covered: sticky positioning is preserved; separator and shadow declarations are removed; header tint overlay is asserted; the dim override removal is asserted.
- NOT covered (missing tests): the effective hovered sticky-cell background after the generic row-hover rule and sticky-cell hover rule cascade together.

**Test quality findings:**
1. MUST FIX — `frontend/src/accessibility.test.ts:124-128` does not verify that hovered sticky cells remain opaque after cascade resolution, so the current regression passes the suite.

**Overall test verdict:** Inadequate until the hover-opacity regression is covered.

### Security Assessment
No concerns identified.

### Performance Notes
No concerns identified.

### Positive Observations
- The separator/shadow removal is clean and tightly scoped to the sticky-column rules.
- Reusing the header-scoped sticky rule for the cyan tint overlay is the right seam for the plan-review blocker.
- Verification is fast and reproducible once frontend dependencies are installed.

### Required Next Steps for Developer
1. Adjust the sticky hover CSS so pinned cells keep an opaque base while hovered.
2. Extend `frontend/src/accessibility.test.ts` to fail when the generic hover rule can override sticky-cell opacity.
3. Re-run `npm run typecheck`, `npx vitest run`, and `npm run build`, then return the PR for review.

### Routing
Transitioning `status:code-review` -> `status:in-development` because the PR has a blocking correctness regression in the sticky hover state and needs developer changes before it can move forward.
