Author: mud-rev

## Code Review Report — DB Index Fixes

**Issue:** #101 — Redesign frontend-backend data architecture (DB performance follow-up)
**Commit:** `16b2173` (`Add chunk hot-path indexes`)
**Branch:** `dev`
**Reviewer:** mud-rev
**Date:** 2026-05-24

### Verdict

APPROVED

All five findings from the DB performance review are correctly addressed. All 9 C++ tests pass, 25 frontend tests pass, and `EXPLAIN QUERY PLAN` confirms every new index is selected by the SQLite planner for its target query.

---

### Documentation Check

- `README.md` — not needed (no setup steps or user-facing behavior changed)
- API reference — not needed (no endpoint or response shape changes)
- `CHANGELOG.md` — not needed (no CHANGELOG in project)
- `docs/architecture/` — not needed (does not exist)
- Inline code docs — not needed (migration DDL is self-documenting)
- `.env.example` — not needed (no new env vars)

---

### Findings

#### MUST FIX — none

#### SHOULD FIX — none

#### NIT / SUGGESTION — optional

**1. Four new indexes are declared twice in `migrate()`**

`src/db.cpp:83-86` (initial DDL block) and `src/db.cpp:153-156` (standalone `exec()` calls)

The four new indexes appear in both the `CREATE TABLE … CREATE INDEX` initial block and as standalone `exec()` calls later in the migration. Since all four use `CREATE INDEX IF NOT EXISTS`, the duplication is harmless — the second call is always a no-op regardless of whether the database is fresh or existing.

This follows the same dual-declaration pattern already established in this file for `idx_chunks_vchunk_hex_span` (lines 82 and 139). The behavior is correct and consistent with the codebase convention.

No action required.

---

### Test Review

**Test suite results:**
- C++ (ctest): 9/9 pass
- Frontend (vitest): 25/25 pass
- TypeScript: clean (no type errors)
- Build: clean

**Test file reviewed:**

- `tests/test_visualization.cpp` — new test: `"vis_revision increments after work assignment and submit"` (lines 32–63)

  The test:
  1. Reads `vis_revision` before any mutation (baseline).
  2. Calls `handleWork` → asserts `vis_revision == initialRevision + 1` (strict equality, confirms exactly one invalidation per work assignment).
  3. Calls `handleSubmit` with `status: "done"` → asserts `vis_revision == afterWorkRevision + 1`.

  Both `REQUIRE(code == 200)` guards are in place. The assertions are behaviorally meaningful — they verify `invalidateVisualizationLocked()` is called from both the work assignment path and the submit path, not merely that the field is present. This closes the coverage gap identified in the previous review report. ✓

**Coverage assessment:**
- Well covered: `vis_revision` increment on work assignment and on submit (new); all previously existing visualization endpoint shape, security, and concurrency tests remain.
- NOT covered: no regression test for the DROP of `idx_blocked_vchunk_lookup` (not required — DDL migrations are not typically unit-tested).

**Overall test verdict:** Adequate.

---

### EXPLAIN QUERY PLAN Verification

Verified against a clean SQLite schema matching the new migration. All four target queries select the new index without a temp B-tree:

| Finding | Query | Plan (after fix) |
|---------|-------|-----------------|
| #1 — reclaimer | `UPDATE … WHERE status='assigned' AND is_test=0 AND heartbeat_at<…` | `SEARCH chunks USING INDEX idx_chunks_status_heartbeat (status=? AND heartbeat_at<?)` |
| #2 — worker reactivation | `UPDATE … WHERE worker_name=? AND status='assigned'` | `SEARCH chunks USING INDEX idx_chunks_worker_status (worker_name=? AND status=?)` |
| #3 — stats ORDER BY | `SELECT … WHERE puzzle_id=? AND is_test=0 ORDER BY id ASC` | `SEARCH chunks USING INDEX idx_chunks_puzzle_id_ordered (puzzle_id=?)` — no temp B-tree ✓ |
| #4 — alloc_generation GROUP BY | `SELECT alloc_generation, COUNT(*) … GROUP BY alloc_generation` | `SEARCH chunks USING COVERING INDEX idx_chunks_puzzle_generation (puzzle_id=?)` — covering, no table lookup ✓ |

Finding #5 (redundant index): `idx_blocked_vchunk_lookup` is cleanly absent from the `blocked_vchunk_ranges` creation block, and `exec("DROP INDEX IF EXISTS idx_blocked_vchunk_lookup")` runs in the migration path for existing databases. ✓

---

### Security Assessment

No security-sensitive changes. DDL-only migration plus a behavioural test. No concerns.

---

### Performance Notes

The four new indexes eliminate the two MUST FIX full table scans on `chunks` (19,194 rows) that were firing on every 60-second reclaim cycle and on every work request from 16 active workers. The two SHOULD FIX temp B-tree sorts running every 5 seconds are also eliminated. The `idx_chunks_puzzle_generation` index is used as a covering index, requiring no table row reads for the `alloc_generation` aggregation.

---

### Positive Observations

1. **Migration pattern is consistent**: the dual-declaration of indexes (initial DDL block + standalone `exec()` upgrade path) matches the established codebase convention for `idx_chunks_vchunk_hex_span` exactly.
2. **Strict equality assertions in vis_revision test**: `== initialRevision + 1` rather than `>` catches double-invalidation bugs without being fragile.
3. **All statements are idempotent**: `CREATE INDEX IF NOT EXISTS` and `DROP INDEX IF EXISTS` throughout — safe to run migrate() multiple times.

---

### Required Next Steps for Developer

None. This change is approved as-is. The optional vis_revision test was added as a bonus beyond what was required.
