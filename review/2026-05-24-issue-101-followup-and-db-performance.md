Author: mud-rev

## Code Review Report — Re-review & Database Performance Analysis

**Issue:** #101 — Redesign frontend-backend data architecture
**Branch:** `dev` (commit `ed91837`)
**Reviewer:** mud-rev
**Date:** 2026-05-24

### Re-review Verdict

APPROVED

All MUST FIX and SHOULD FIX items from the initial review are resolved. The developer's implementation is sound. **A separate MUST FIX database performance finding (full table scans on hot paths) is documented below; it is independent of the visualization architecture change and should be addressed in a follow-on commit.**

---

## Part 1 — Re-review of Visualization Locking Fix

### Summary

The developer correctly implemented the shared-lock / lock-free build / unique-lock write pattern for all three visualization handlers, including a `for (;;)` retry loop with stale-revision detection. The SQL duplication in `buildAllocatorVisualization` is eliminated by routing through `loadVisualPoints`. The `Promise.all` parallelism issue is naturally resolved. The concurrency test is correctly wired via `VisualizationBuildHook` injection. All 9 C++ tests pass, 25 frontend tests pass, TypeScript and build are clean.

---

### Finding #1 — MUST FIX (Prior) → RESOLVED

**`handleHeatmapVisualization` / `handleHilbertVisualization` / `handleAllocatorVisualization` holding `std::unique_lock` during heavy DB reads**

`src/service_visualization.cpp` — all three handlers

The developer implemented the correct pattern. Each handler now:

1. Checks the cache under a `std::shared_lock` (fast path, non-exclusive).
2. If a cache miss, opens a detached read-only `PoolDb` (`SQLite::OPEN_READONLY`) for file-backed databases — no service mutex held during the DB read.
3. Falls back to a `std::shared_lock` in-process path for in-memory (test) databases.
4. Writes the result under `std::unique_lock` only after the build completes, with a stale-revision check and early-return if another caller already populated the cache.

```cpp
// Cache hit under shared lock (non-blocking)
{
    std::shared_lock lock(mu_);
    ...
    if (it != visCache_.end() && it->second.revision == revision && !it->second.heatmap.is_null()) {
        return jsonResponse(it->second.heatmap);
    }
}

// Build WITHOUT holding any lock (detached read-only connection)
if (useDetachedVisualizationReader(cfg_)) {
    PoolDb readDb(cfg_, SQLite::OPEN_READONLY, false);
    built = buildHeatmapVisualizationFromDb(readDb, puzzle);
} else {
    std::shared_lock lock(mu_);
    built = buildHeatmapVisualizationFromDb(db_, puzzle);
}

// Write under exclusive lock — fast (only a JSON move + revision check)
std::unique_lock lock(mu_);
if (visualizationRevisionLocked(puzzle.id) != revision) continue;  // ← stale, retry
...
```

The `PoolDb` detached reader correctly sets `PRAGMA query_only=ON` and skips WAL setup and schema migration:

```cpp
if ((openFlags & SQLite::OPEN_READONLY) != 0) {
    exec("PRAGMA query_only=ON");
    return;
}
```

**Verdict:** Correctly resolved. ✓

---

### Finding #2 — SHOULD FIX (Prior) → RESOLVED

**`buildAllocatorVisualization` duplicated `loadVisualPoints` SQL query**

`src/service_visualization.cpp` — `buildAllocatorVisualizationFromDb`

The function now calls `loadVisualPoints(db, puzzle, false)` (with `includeBlocked = false`, which is the correct deliberate choice for the allocator view). The SQL duplication is gone. ✓

---

### Finding #3 — SHOULD FIX (Prior) → RESOLVED (indirectly)

**`loadAllVisualizationsForCurrentPuzzle` fires 3 concurrent requests that serialize at the backend mutex**

`frontend/src/dashboard.ts:427-432`

No frontend change was needed. With the exclusive-lock bottleneck removed, all three backend handlers now run genuinely in parallel. ✓

---

### New NIT — `for(;;)` loop has a theoretical retry amplification scenario

`src/service_visualization.cpp` — all three handlers

The retry loop continues if `visualizationRevisionLocked(puzzle.id) != revision` at write time, meaning that if work assignments arrive faster than a visualization build completes, the handler retries indefinitely. In practice, work assignments arrive at most once per worker per ~10 minutes on a running puzzle, and builds complete in < 100 ms (23 ms measured on production data), so the window for continuous invalidation is effectively zero. The pattern is safe and the behavior is correct; this is noted for completeness only.

No action required.

---

### Test Coverage Re-assessment

**New test added:** `"handleStats remains responsive while visualization rebuild is in flight"` (`tests/test_visualization.cpp:112-154`)

The test:
1. Creates a file-backed `PoolService` (required for the detached reader path to be exercised).
2. Injects a `VisualizationBuildHook` that blocks on a `std::promise` after setting `buildStarted`.
3. Launches `handleHeatmapVisualization` in a background thread; waits up to 250 ms for the hook to fire.
4. Asserts `handleStats` completes within 150 ms while the build is blocked.
5. Releases the build and asserts the visualization response is 200.

This directly proves the MUST FIX is resolved and will catch any regression that reintroduces exclusive locking during the build phase. ✓

**Remaining coverage gap (non-blocking):** No test verifies that `vis_revision` increments after a work assignment or submit. Presence is tested; increment is not. Low risk given the explicit `invalidateVisualizationLocked()` callsites, but a follow-on test would complete the behavioral coverage.

---

## Part 2 — Database Performance Analysis

### Scope

Production database: `/tmp/pool.db` (live copy as of 2026-05-24)
- `chunks`: 19,194 rows (18,945 non-test)
- `blocked_vchunk_ranges`: 168,464 rows
- `workers`: 16 rows (14 currently assigned)
- DB size: ~81 MB, WAL mode

All queries were analyzed with `EXPLAIN QUERY PLAN` on the production schema. Query source locations are cross-referenced against the C++ application code.

---

### DB Finding #1 — MUST FIX — `reclaimTimedOutChunks` full table scan on chunks

**File:** `src/work_service.cpp` (background reclaimer, fires every 60 seconds)

**Query:**
```sql
UPDATE chunks
SET status = 'open', worker_name = NULL, prev_worker_name = worker_name, assigned_at = NULL, heartbeat_at = NULL
WHERE status = 'assigned'
  AND is_test = 0
  AND heartbeat_at < datetime('now', ?)
```

**EXPLAIN QUERY PLAN:**
```
SCAN chunks
```

**Impact:** Full scan of all 19,194 rows every 60 seconds, evaluating `status`, `is_test`, and `heartbeat_at` for each row. As the chunk table grows (the pool intends to cover 2^66 keys), this scan cost grows linearly with total chunk history.

**Fix:**
```sql
CREATE INDEX idx_chunks_status_heartbeat ON chunks (status, heartbeat_at)
WHERE is_test = 0;
```

With this partial index, SQLite will use `SEARCH chunks USING INDEX idx_chunks_status_heartbeat (status=? AND heartbeat_at<?)` — scanning only the small number of rows with `status = 'assigned'` and an expired heartbeat (typically 0–5 rows in a healthy pool).

**Risk:** Low. The index is partial (`WHERE is_test = 0`), matching the query predicate exactly. No schema migration required beyond adding the index. **Add via `addColumnIfMissing`-style guard or a bare `CREATE INDEX IF NOT EXISTS`** in `PoolDb::migrate()`.

---

### DB Finding #2 — MUST FIX — Worker reactivation full table scan on chunks

**File:** `src/work_service.cpp` (worker reactivation reclaim, runs on every `handleWork` call for a known worker)

**Query:**
```sql
UPDATE chunks
SET status = 'open', worker_name = NULL, prev_worker_name = worker_name, assigned_at = NULL, heartbeat_at = NULL
WHERE worker_name = ?
  AND status = 'assigned'
```

**EXPLAIN QUERY PLAN:**
```
SCAN chunks
```

**Impact:** Full scan of all 19,194 rows on every work request from any worker (16 workers, each polling every few seconds). This fires roughly once per heartbeat interval per worker — potentially 16+ times per minute.

**Fix:**
```sql
CREATE INDEX idx_chunks_worker_status ON chunks (worker_name, status);
```

With this index, SQLite uses `SEARCH chunks USING INDEX idx_chunks_worker_status (worker_name=? AND status=?)` — touching only the handful of rows assigned to that worker (typically 1–3 rows per worker).

**Risk:** Low. `worker_name` and `status` are already queried together in other paths. The index serves both the reclaim UPDATE and any future per-worker lookups.

---

### DB Finding #3 — SHOULD FIX — Stats endpoint ORDER BY causes temp B-tree sort every 5 seconds

**File:** `src/service_stats.cpp` (chunks query in stats handler, polled every 5 seconds by the frontend)

**Query (representative):**
```sql
SELECT id, status, start_hex, end_hex, worker_name, found_address, alloc_generation, ...
FROM chunks
WHERE puzzle_id = ?
  AND is_test = 0
ORDER BY id ASC
```

**EXPLAIN QUERY PLAN:**
```
SEARCH chunks USING INDEX idx_chunks_puzzle_status (puzzle_id=?)
USE TEMP B-TREE FOR ORDER BY
```

**Impact:** The existing `idx_chunks_puzzle_status (puzzle_id, status)` is used for the `puzzle_id` filter, but does not satisfy the `ORDER BY id ASC`. SQLite builds a temporary B-tree to sort 18,945 rows on every stats poll (every 5 seconds). This is a constant ~18 K sort running 12 times per minute.

**Fix:**
```sql
CREATE INDEX idx_chunks_puzzle_id_ordered ON chunks (puzzle_id, id)
WHERE is_test = 0;
```

With this covering partial index, `ORDER BY id ASC` is satisfied by the index order. SQLite uses `SEARCH chunks USING INDEX idx_chunks_puzzle_id_ordered (puzzle_id=?)` with no temp sort. The `is_test = 0` partial condition also eliminates test rows at the index level.

**Risk:** Low-medium. Verify that `loadVisualPoints` (`src/service_visualization.cpp`) also benefits — it runs the same `WHERE puzzle_id = ? AND is_test = 0 ORDER BY id ASC` pattern, so this index serves both the stats path and the visualization build path.

---

### DB Finding #4 — SHOULD FIX — `alloc_generation` GROUP BY causes temp B-tree sort every 5 seconds

**File:** `src/service_stats.cpp` (generation aggregation, polled every 5 seconds)

**Query:**
```sql
SELECT alloc_generation, COUNT(*)
FROM chunks
WHERE puzzle_id = ?
  AND is_test = 0
GROUP BY alloc_generation
ORDER BY alloc_generation
```

**EXPLAIN QUERY PLAN:**
```
SEARCH chunks USING INDEX idx_chunks_puzzle_status (puzzle_id=?)
USE TEMP B-TREE FOR GROUP BY
```

**Impact:** Temp B-tree for `GROUP BY alloc_generation` on 18,945 rows every 5 seconds.

**Fix:**
```sql
CREATE INDEX idx_chunks_puzzle_generation ON chunks (puzzle_id, alloc_generation)
WHERE is_test = 0;
```

This allows SQLite to use `SEARCH chunks USING INDEX idx_chunks_puzzle_generation (puzzle_id=?)` and read `alloc_generation` values in index order, eliminating the temp sort for the `GROUP BY` and `ORDER BY`.

**Risk:** Low. The `alloc_generation` column is text and low-cardinality (`'legacy_random_shards_v1'`, `'affine_bijection_v1'`, `'feistel_v1'`, `NULL`). The index will be small and fast to build.

---

### DB Finding #5 — NIT — `idx_blocked_vchunk_lookup` is redundant

**Table:** `blocked_vchunk_ranges`

**Existing indexes:**
```sql
CREATE UNIQUE INDEX idx_blocked_vchunk_unique ON blocked_vchunk_ranges (puzzle_id, start_vchunk, end_vchunk, source);
CREATE INDEX idx_blocked_vchunk_lookup ON blocked_vchunk_ranges (puzzle_id, start_vchunk ASC);
```

`idx_blocked_vchunk_lookup (puzzle_id, start_vchunk)` is a prefix of `idx_blocked_vchunk_unique (puzzle_id, start_vchunk, end_vchunk, source)`. SQLite can use the unique index for any query that `idx_blocked_vchunk_lookup` would serve. The duplicate index wastes ~2 MB of storage and is updated on every insert/delete of `blocked_vchunk_ranges` rows (168,464 rows → each import touches both indexes unnecessarily).

**Fix:**
```sql
DROP INDEX IF EXISTS idx_blocked_vchunk_lookup;
```

Remove the `CREATE INDEX` statement from `PoolDb::migrate()` and add a migration step that drops it if it exists on upgraded databases.

**Risk:** Very low. All query access patterns on `blocked_vchunk_ranges` use `(puzzle_id, start_vchunk)` as a prefix, which is already covered by `idx_blocked_vchunk_unique`.

---

### DB Summary Table

| # | Severity | File | Query | Plan | Fix |
|---|----------|------|-------|------|-----|
| 1 | MUST FIX | `src/work_service.cpp` | `UPDATE … WHERE status='assigned' AND heartbeat_at < …` | `SCAN chunks` (19K rows, every 60 s) | `CREATE INDEX idx_chunks_status_heartbeat ON chunks (status, heartbeat_at) WHERE is_test = 0` |
| 2 | MUST FIX | `src/work_service.cpp` | `UPDATE … WHERE worker_name=? AND status='assigned'` | `SCAN chunks` (19K rows, every work request) | `CREATE INDEX idx_chunks_worker_status ON chunks (worker_name, status)` |
| 3 | SHOULD FIX | `src/service_stats.cpp` | `SELECT … WHERE puzzle_id=? AND is_test=0 ORDER BY id ASC` | temp B-tree sort 18K rows, every 5 s | `CREATE INDEX idx_chunks_puzzle_id_ordered ON chunks (puzzle_id, id) WHERE is_test = 0` |
| 4 | SHOULD FIX | `src/service_stats.cpp` | `SELECT alloc_generation, COUNT(*) … GROUP BY alloc_generation` | temp B-tree, every 5 s | `CREATE INDEX idx_chunks_puzzle_generation ON chunks (puzzle_id, alloc_generation) WHERE is_test = 0` |
| 5 | NIT | `src/db.cpp` | n/a | Redundant index on `blocked_vchunk_ranges` | `DROP INDEX IF EXISTS idx_blocked_vchunk_lookup` |

---

### Recommended Migration Additions to `PoolDb::migrate()`

```cpp
// DB Finding #1 — reclaimer full scan
exec("CREATE INDEX IF NOT EXISTS idx_chunks_status_heartbeat "
     "ON chunks (status, heartbeat_at) WHERE is_test = 0");

// DB Finding #2 — worker reactivation full scan
exec("CREATE INDEX IF NOT EXISTS idx_chunks_worker_status "
     "ON chunks (worker_name, status)");

// DB Finding #3 — stats + visualization ORDER BY sort
exec("CREATE INDEX IF NOT EXISTS idx_chunks_puzzle_id_ordered "
     "ON chunks (puzzle_id, id) WHERE is_test = 0");

// DB Finding #4 — alloc_generation GROUP BY sort
exec("CREATE INDEX IF NOT EXISTS idx_chunks_puzzle_generation "
     "ON chunks (puzzle_id, alloc_generation) WHERE is_test = 0");

// DB Finding #5 — redundant index cleanup
exec("DROP INDEX IF EXISTS idx_blocked_vchunk_lookup");
```

All five statements are idempotent (`IF NOT EXISTS` / `IF EXISTS`) and safe to add to the existing migration function without a version gate.

---

### Positive Observations

1. **WAL mode is correctly configured** — `PRAGMA journal_mode=WAL` and `PRAGMA synchronous=NORMAL` in `PoolDb` allow concurrent readers alongside the single writer. This is the correct choice for the visualization detached reader pattern.
2. **`idx_chunks_vchunk_hex_span (puzzle_id, vchunk_start_hex, vchunk_end_hex, status)`** is used efficiently by `loadOccupiedRanges` — the allocator's hot path (called on every work assignment) performs `SEARCH … USING INDEX idx_chunks_vchunk_hex_span` with no temp sorts.
3. **`idx_blocked_vchunk_unique`** serves as a covering index for the `loadVisualPoints` blocked-ranges scan — 168,464 rows are read in index order without a table lookup, measured at ~23 ms on production data.
4. **`idx_chunks_puzzle_status (puzzle_id, status)`** efficiently serves the work assignment `SELECT … WHERE puzzle_id=? AND status='open'` hot path.

---

### Required Next Steps for Developer

1. **Add the four new index DDL statements to `PoolDb::migrate()`** in `src/db.cpp` — cover Findings #1 (#2 (#3 and (#4. Use `CREATE INDEX IF NOT EXISTS` for forward compatibility. (MUST FIX / SHOULD FIX)
2. **Drop the redundant `idx_blocked_vchunk_lookup` index** — add `DROP INDEX IF EXISTS idx_blocked_vchunk_lookup` to the migration and remove its `CREATE INDEX` statement. (NIT)
3. **(Optional)** Add a test that verifies `vis_revision` increments after a work assignment + submit cycle — to complete behavioral coverage of the invalidation path. (Non-blocking follow-on)
