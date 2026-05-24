Author: mud-rev

## Code Review Report

**Issue:** #101 — Redesign frontend-backend data architecture
**Branch:** `dev` (ad-hoc commits, no separate PR)
**Reviewer:** mud-rev
**Date:** 2026-05-24

### Verdict

CHANGES REQUESTED

### Summary

Issue #101 implements a significant and well-conceived architectural improvement: heavy chunk-visualization work is moved server-side and served through three dedicated endpoints (`/api/v1/visualization/heatmap`, `/hilbert`, `/allocator`), with server-side aggregation into compact cell tuples. The frontend's mousemove tooltip cost drops from O(N) unthrottled scans to O(1)/O(log N) Map lookups, and the stats payload is stripped of ~1.5 MB of raw chunk rows. The implementation is clean, tests pass, and many prior-review findings are correctly addressed.

One blocking concern remains: the three new visualization handlers acquire `std::unique_lock lock(mu_)` and hold it while executing `buildXVisualization()` — which on a cache miss runs `loadVisualPoints()` (reading all chunks + all 168 208 blocked_vchunk_ranges rows). The pattern is the same class of bug as the original `refreshPuzzleStatuses` mutex issue, just on a different code path. In a production system with active workers, the visualization cache is invalidated on every work assignment, making cache misses common. A single cache-miss rebuild can hold the exclusive lock for hundreds of milliseconds, blocking `handleStats`, `handleWork`, `handleHeartbeat`, and `handleSubmit` for all concurrent callers during that window.

---

### Documentation Check

- `README.md` — not needed (high-level overview, endpoint detail lives in docs/api.md)
- `docs/api.md` — **updated** — new visualization endpoints, `vis_revision`, `found_key` removal, `puzzle.status` shape all documented. ✓
- API reference (in `index.html`) — **updated** — 3 new `<details>` panels added. ✓
- `CHANGELOG.md` — not needed (no CHANGELOG in the project)
- `docs/architecture/` — not needed (does not exist)
- Inline code docs — not needed (private helpers are self-explanatory)
- `.env.example` — not needed (no new env vars)

---

### Findings

#### MUST FIX — blocking

**1. `handleHeatmapVisualization`, `handleHilbertVisualization`, `handleAllocatorVisualization` hold `std::unique_lock lock(mu_)` during heavy DB reads on cache miss**

`src/service_visualization.cpp:351-390`

```cpp
crow::response PoolService::handleHeatmapVisualization(const crow::request& req) {
    std::unique_lock lock(mu_);          // ← exclusive, holds entire handler
    ...
    if (cache.revision != revision || cache.heatmap.is_null()) {
        cache.heatmap = buildHeatmapVisualization(puzzle);  // ← heavy: reads all chunks +
                                                             //   168 208 blocked_vchunk_ranges
    }
    return jsonResponse(cache.heatmap);
}
```

On a cache miss, `buildHeatmapVisualization` → `loadVisualPoints(db_, puzzle, true)` reads every chunk row and every `blocked_vchunk_ranges` row. While the exclusive lock is held, every concurrent `handleStats`, `handleWork`, `handleHeartbeat`, and `handleSubmit` is blocked.

The cache is invalidated on every successful work assignment (`service_work.cpp`), every submit, and every reclaim. In a production system with multiple active workers, the cache may be stale continuously, making cache misses frequent rather than exceptional.

The three concurrent fetch calls from `loadAllVisualizationsForCurrentPuzzle` (frontend `Promise.all`) compound this: three serialized exclusive-lock acquisitions each performing a heavy DB read mean up to 3× the blocking window before the first stats response can proceed.

**Recommended fix** — mirror the pattern already used in `handleActivatePuzzle` / `handleSetPuzzle`:

```cpp
crow::response PoolService::handleHeatmapVisualization(const crow::request& req) {
    // Step 1: cache-hit check under shared lock (fast)
    std::optional<json> cached;
    PuzzleRow puzzle;
    uint64_t revision = 0;
    {
        std::shared_lock lock(mu_);
        auto requested = resolveRequestedPuzzle(db_, req);
        if (requested.error) return std::move(*requested.error);
        puzzle = *requested.puzzle;
        revision = visualizationRevisionLocked(puzzle.id);
        auto it = visCache_.find(puzzle.id);
        if (it != visCache_.end() &&
            it->second.revision == revision &&
            !it->second.heatmap.is_null()) {
            cached = it->second.heatmap;
        }
    }
    if (cached) return jsonResponse(*cached);

    // Step 2: build WITHOUT holding any lock (reads DB; safe: SQLite WAL allows concurrent reads)
    auto built = buildHeatmapVisualization(puzzle);

    // Step 3: write to cache under exclusive lock (fast: only a JSON move)
    {
        std::unique_lock lock(mu_);
        auto& entry = visCache_[puzzle.id];
        entry.revision = revision;
        entry.heatmap = std::move(built);
        cached = entry.heatmap;
    }
    return jsonResponse(*cached);
}
```

Applying this to all three handlers eliminates the blocking window for the common case. Two requests may simultaneously build the same cache entry if they race; that is safe and preferable to serialization.

**Risk:** Low. Same pattern that was applied to status refresh; `loadVisualPoints` only reads, never writes.

---

#### SHOULD FIX — non-blocking but important

**2. `buildAllocatorVisualization` duplicates `loadVisualPoints` SQL query**

`src/service_visualization.cpp:444-467`

`buildAllocatorVisualization` runs its own copy of the chunks query (`SELECT id, status, start_hex, end_hex, alloc_generation FROM chunks …`) rather than calling `loadVisualPoints`. The two implementations will diverge if `loadVisualPoints` is updated (e.g., new filters, generation normalization changes). Notably, `buildAllocatorVisualization` does not include blocked ranges, which is a deliberate design choice, but this should be documented rather than inferred.

**Recommended fix:** Extract `loadAllocatorPoints(db_, puzzle)` (native chunks only, no blocked ranges, sorted by id) as a shared helper, or add a comment to `buildAllocatorVisualization` explaining why it cannot reuse `loadVisualPoints`.

---

**3. `loadAllVisualizationsForCurrentPuzzle` fires 3 concurrent requests that serialize at the backend mutex**

`frontend/src/dashboard.ts:427-432`

```typescript
async function loadAllVisualizationsForCurrentPuzzle(): Promise<void> {
  await Promise.all([
    loadHeatmapVisualizationPanel(),
    loadAllocatorVisualizationPanel(),
    loadHilbertVisualizationPanel(),
  ]);
}
```

Because all three backend handlers currently acquire `std::unique_lock`, the three requests serialize. The `Promise.all` provides no parallelism benefit in practice, and the user sees three staggered "loading" states. This is a consequence of finding #1 — resolving that finding with the `shared_lock` pattern allows the backend to serve all three in parallel, which is when this `Promise.all` begins to help.

**No frontend change needed** once finding #1 is fixed. This is noted to explain why finding #1 matters beyond just the stats/work/submit blocking window.

---

#### NIT / SUGGESTION — optional

**4. `drawHilbert` recomputes `dominantStatus` for every (cell × status) pair**

`frontend/src/canvas.ts:215-235`

The outer loop iterates over 5 statuses; the inner loop visits each occupied cell for every status, recomputing `dominantStatus(cell, ...)` each time. The dominant status for a cell is constant for all 5 iterations. Pre-computing it once per cell before the status loop would halve redundant work. Impact is small (|cells| ≤ occupied Hilbert slots, practically ≪ 65536 in a running puzzle), so this is a minor suggestion.

**5. `allocGenerationFilter` type cast**

`frontend/src/dashboard.ts:122-123`

```typescript
return allocatorVis?.generations[allocGenerationFilter as 'all' | 'legacy' | 'affine' | 'feistel'] ?? null;
```

The cast could be `as keyof typeof allocatorVis.generations` or `as keyof AllocatorVisualizationResponse['generations']` for better type-safety alignment. Low risk since `allocGenerationFilter` is only ever set from `btn.dataset.gen` HTML attributes, but the cast hides a potential stale value.

**6. `performance.test.ts` — structural regex guards**

`frontend/src/performance.test.ts`

These tests verify architectural invariants by regex-matching source file contents. This pattern correctly protects against accidental reversion of the new architecture (e.g., `chunks_vis` reappearing in `types.ts`). However, such tests can pass while logic bugs exist (a renamed function still matched). They are acceptable for regression-guarding but should not substitute for behavioral tests of the visualization loading flow.

---

### Test Review

**Test suite result:**
- C++ (ctest): 9/9 tests pass
- Frontend (vitest): 25/25 tests pass (3 test files)
- TypeScript: `tsc --noEmit` passes, no type errors
- Frontend build: passes (`vite build` — 85.82 kB output)

**Test files reviewed:**

- `tests/test_visualization.cpp` (new) — covers:
  - `vis_revision` presence and `chunks_vis` absence in stats response
  - `found_key` omission from finder responses (E2E: assign → FOUND submit → stats check)
  - `heatmap`, `hilbert`, `allocator` endpoints return expected JSON shapes

- `tests/test_puzzle_status.cpp` (modified) — new test:
  - `"handleStats remains responsive while address status fetch is in flight"` — concurrency test using `std::atomic<bool>` + `std::promise` + `std::async`
  - Verifies `handleStats` completes within 150 ms while `AddressStatusFetcher` is blocked in background
  - Directly proves the prior-review MUST FIX (mutex during network I/O) is resolved

- `frontend/src/performance.test.ts` (new) — 3 structural regression guards:
  - Verifies new API fetch functions exist
  - Verifies `chunks_vis` is absent from `types.ts`
  - Verifies automatic load is gated on puzzle switch, not on every poll interval
  - Verifies per-panel refresh button wiring

- `frontend/src/accessibility.test.ts` (extended) — 9 tests checking:
  - `prefers-reduced-motion` media query presence and correctness
  - `aria-pressed` defaults in HTML and synchronization in JS
  - `<details>`/`<summary>` usage for API panels
  - `content-visibility: auto` on visualization sections
  - Puzzle status chip styling and text transformation
  - Backend status badge animation and icon markup

**Coverage assessment:**

- Well covered:
  - Visualization endpoint response shapes (heatmap, hilbert, allocator)
  - `found_key` removal security invariant
  - `vis_revision` field propagation
  - Per-panel refresh button wiring
  - Accessibility invariants (`prefers-reduced-motion`, `aria-pressed`, `<details>`)
  - Prior-review concurrency fix (status refresh mutex)

- NOT covered — missing tests that should exist:
  - **Cache invalidation behavior**: no test verifies that a work assignment or submit causes a subsequent visualization request to return updated data (i.e., that `invalidateVisualizationLocked` actually causes a cache miss followed by a fresh build). This is the most important behavioral invariant of the caching design.
  - **`vis_revision` increment on invalidation**: no test verifies that `vis_revision` increments after a mutation. The `handleStats` test checks the field is present but does not verify it changes.
  - **Concurrent visualization + stats responsiveness** (the MUST FIX): there is a concurrency test for `handleStats` vs status-refresh, but no equivalent test for `handleStats` responsiveness while a visualization cache rebuild is in flight. Such a test (analogous to the status test) would directly catch a regression of finding #1.
  - **`hilbertIndex` consistency**: no test verifies that the backend linear index and the frontend `getHilbertD`/`getHilbertXY` inverse pair are consistent. A unit test confirming that `getHilbertD(N, getHilbertXY(N, d)[0], getHilbertXY(N, d)[1]) === d` for sampled values would protect against accidental divergence.

**Test quality findings:**

1. MUST FIX — `test_visualization.cpp` has no test for visualization handler responsiveness while another handler holds the lock during a cache rebuild. This is the behavioral companion to the structural MUST FIX finding #1 and is needed to prevent regression.
2. SHOULD FIX — No test verifies `vis_revision` increases after a mutation (work assignment, submit). Currently it's tested for presence only.
3. NIT — `performance.test.ts` tests are structural (source-file string matching) rather than behavioral; they complement but do not replace end-to-end flow testing.

**Overall test verdict:** Partially adequate. The tests for the new endpoints, security invariants, and accessibility regressions are well-designed. The absence of a cache-invalidation behavior test and a visualization-handler concurrency test are gaps that directly relate to the blocking MUST FIX.

---

### Security Assessment

- `found_key` is correctly removed from all dashboard-facing SQL queries (`service_stats.cpp`) and the `FinderEntry` TypeScript interface has no `found_key` field. The `handleStats` omits finders' private keys in both production and test paths. Verified by test and code inspection. ✓
- Admin token is stored in `sessionStorage` (not `localStorage`). Acceptable for an internal single-user admin action; the backend validates it server-side.
- All rendered worker names, puzzle names, and finder addresses go through `esc()` HTML escaping in `dashboard.ts`. Tooltip content uses static `STATUS_NAMES` strings and formatted numbers — no user-controlled HTML. ✓
- No new environment variables or secrets introduced. ✓

---

### Performance Notes

**Improvements:**
- Stats payload drops from ~1.5 MB (raw chunk rows) to ≪1 KB. ✓
- Client-side tooltip scan: O(N) unthrottled mousemove → O(1) Map lookup. ✓
- Client-side visualization: O(5N log N) sorts per update → O(|cells|) rendering of server-aggregated data. ✓

**Regression introduced (Finding #1):**
- Visualization cache rebuild under `std::unique_lock` blocks all concurrent API handlers
- In an active pool, the cache is invalidated on every work assignment, making rebuilds frequent
- Each rebuild can take hundreds of milliseconds (168 208 blocked_vchunk_ranges + all chunks)
- Three concurrent visualization fetches serialize this, creating a window of ~1–3 s of global blocking per page load

---

### Positive Observations

1. **Prior-review MUST FIX was correctly resolved**: `handleActivatePuzzle` and `handleSetPuzzle` call `lock.unlock()` before `refreshPuzzleStatuses()`, eliminating the network I/O under exclusive lock. The constructor dependency injection pattern for `AddressStatusFetcher` enables deterministic concurrency testing and is tested with a well-designed `std::promise`-based test.

2. **`hash32Mix` — backend/frontend parity**: The 32-bit mixing hash for heatmap cell assignment is identical between C++ (`service_visualization.cpp:86-97`) and TypeScript (`canvas.ts:61-72`), including constant values and bit widths.

3. **`hilbertIndex` design is correct and consistent**: The backend assigns linear keyspace slots `floor(s × N²)`, the frontend maps them to canvas (x, y) via `getHilbertXY(N, slot)`, and the tooltip inverse uses `getHilbertD(N, hx, hy)` — forming a coherent space-filling curve layout of 1D keyspace data.

4. **`VisualizationCell` tuple format** (`[index, completed, assigned, reclaimed, found, blocked]`) is space-efficient: 6 numbers vs 6+ named fields per cell for potentially tens of thousands of cells in the JSON payload.

5. **`vis_revision` staleness signaling** is clean and non-intrusive: the frontend marks panels stale without auto-fetching, giving users manual control over heavy visualization reloads.

6. **`<details>`/`<summary>` API panels** replace the previous `hidden="until-found"` proposal, using native browser disclosure semantics with no JavaScript. Collapse state persists within session via the browser's `open` attribute.

7. **All accessibility issues from the modern-web-guidance review are addressed**: `prefers-reduced-motion` media query with `!important` overrides, `aria-pressed` on all filter buttons with JS synchronization, `content-visibility: auto` on visualization panels, and `<details>`/`<summary>` for API panels.

8. **Cache invalidation is wired to all mutation paths**: work assignment, submit, reclaim, activate-puzzle, set-puzzle, and import-ranges all call `invalidateVisualizationLocked`. Coverage appears complete.

---

### Required Next Steps for Developer

1. **Apply the shared-lock / lock-free build / unique-lock write pattern** to all three visualization handlers (`handleHeatmapVisualization`, `handleHilbertVisualization`, `handleAllocatorVisualization`). The exact pattern is already demonstrated in the codebase by `handleActivatePuzzle`. (Finding #1 — MUST FIX)

2. **Add a test for visualization handler responsiveness during a cache rebuild** — analogous to `"handleStats remains responsive while address status fetch is in flight"` in `test_puzzle_status.cpp`: inject a delayed `buildHeatmapVisualization` call (or a DB hook), verify `handleStats` returns within deadline while the rebuild is in progress. (Test coverage gap for MUST FIX)

3. **Either refactor `buildAllocatorVisualization` to call a shared helper** or add an explicit comment explaining why it intentionally duplicates `loadVisualPoints`. (Finding #2 — SHOULD FIX)

4. **(Optional but recommended) Add a test verifying `vis_revision` increments after a mutation** — e.g., call `handleWork`, then `handleStats`, and assert `vis_revision > 0` increased. (Test coverage gap)
