Author: mud-rev

## Complexity Analysis — Frontend (canvas.ts, dashboard.ts)

**Date:** 2026-05-23
**Scanner:** `complexity-optimizer analyze_complexity.py`
**Scope:** `frontend/src/canvas.ts`, `frontend/src/dashboard.ts`
**Stack:** TypeScript + Vite, no test runner for canvas code
**No files were modified.** This is a report only.

---

### Summary

The scanner flagged ~80 loop-nesting leads; most are benign Canvas 2D draw loops. After manual inspection, **3 genuine algorithmic problems** were found plus **1 unthrottled O(N) mousemove handler** introduced in the previous review cycle. All are pre-existing except item 4 (handler left without debounce after the ksCanvas debounce was removed in this PR).

---

### Finding A — HIGH: `getAllocatorChunks` + sort called 5× per redraw cycle (O(5 N log N) redundant sorts)

**Files:** `canvas.ts:304-312, 328-330, 332-344, 536-543`; `dashboard.ts:429-438`

Every `updateDashboard` call (every 5 s, plus every tab switch) runs this sequence:

```
updateDashboard()
  redrawAll()
    drawAllocatorDiagnostics(getFilteredChunks())
      sortedById = getAllocatorChunks()     ← sort #1: filter + sort by id  O(N log N)
      sortedByS  = sortedById.sort(s)      ← sort #2: sort by s             O(N log N)
      → passed to 5 sub-functions (correctly shared)
  
  exportNormalizedGapMetrics(getFilteredChunks())  ← O(N) filter again
    computeNormalizedGapMetrics()
      getAllocatorNormalizedGaps()
        getAllocatorSortedStarts()
          getAllocatorChunks()              ← sort #3: filter + sort by id  O(N log N)
          .sort(s)                         ← sort #4: sort by s             O(N log N)
        computeNormalizedGapsFromSorted()
      .slice().sort()                      ← sort #5: sort gaps array       O(N log N)
```

**Current complexity:** O(5 N log N) per `updateDashboard`, called every 5 seconds + on each tab switch.

**Problem:** `drawAllocatorDiagnostics` and `exportNormalizedGapMetrics` operate on the *same* filtered chunks (same filter, same call, same `chunksVis` snapshot) but sort independently with no sharing.

`getFilteredChunks()` is also called twice — once for `redrawAll` and once for `exportNormalizedGapMetrics` — doing the same O(N) filter pass both times.

**Recommended fix:**

```typescript
// dashboard.ts — updateDashboard(), after chunksVis = data.chunks_vis ?? []:
chunksVis = data.chunks_vis ?? [];
updateAllocatorGenerationFilterCounts();

const filtered = getFilteredChunks();  // single O(N) filter
redrawAll(filtered);                   // pass pre-filtered
const ngm = exportNormalizedGapMetrics(filtered);  // reuse same array
```

```typescript
// canvas.ts — expose pre-sorted variant of exportNormalizedGapMetrics:
export function exportNormalizedGapMetrics(chunks: ChunkVis[], _sortedByS?: ChunkVis[]) {
  return computeNormalizedGapMetrics(chunks, _sortedByS);
}
```

After `drawAllocatorDiagnostics` computes `sortedByS`, pass it directly to `exportNormalizedGapMetrics`. This eliminates sorts #3, #4, and the second `getFilteredChunks()` call:

```typescript
// redrawAll — share pre-sorted arrays between diagnostics and gap metrics:
function redrawAll(filtered?: ChunkVis[]): void {
  const chunks = filtered ?? getFilteredChunks();
  heatmapBuckets = drawHeatmap(hmCanvas, applyHeatmapLayerFilter(chunksVis, hmLayerFilter));
  const { sortedByS } = drawAllocatorDiagnostics(allocCanvases, gapMetricsEl, chunks);
  drawHilbert(hilCanvas, applyLayerFilter(chunksVis, hilLayerFilter));
  // return sortedByS so updateDashboard can pass it to exportNormalizedGapMetrics
}
```

**Estimated improvement:** O(5 N log N) → O(2 N log N + N). For N = 50 000 chunks the redundant sorts account for ~3 × 800 000 comparisons that are discarded immediately.

**Risk:** Low. The data snapshot (`chunksVis`) does not change between the two calls; sharing the sorted copy is safe. Requires `drawAllocatorDiagnostics` to return `sortedByS` (or accept a callback).

---

### Finding B — HIGH: Unthrottled O(N) scan on every `mousemove` over the Hilbert canvas

**File:** `dashboard.ts:538-552`

```typescript
hilCanvas.addEventListener('mousemove', (e: MouseEvent) => {
  const rect = hilCanvas.getBoundingClientRect();
  // ...
  showTooltip(tooltip, e,
    applyLayerFilter(chunksVis, hilLayerFilter)          // O(N) filter — new array
      .filter(c => c.s <= cell_e && c.e >= cell_s)       // O(N) second scan
  );
});
```

Two O(N) passes run on every raw `mousemove` event (≥60 events/s while the user moves the cursor). The removed `ksCanvas` mousemove handler had an explicit `requestAnimationFrame` debounce (removed in this PR); the Hilbert handler never got one.

**Current complexity:** O(2N) per `mousemove` event, capped by screen refresh only by browser scheduling (not by code). For N = 50 000 chunks: ~100 000 comparisons every ~16 ms = ~6 M comparisons/s during active cursor movement.

**Recommended fix:** add the same rAF debounce the old ksCanvas handler had, and pre-filter once:

```typescript
let _hilMoveFrame: number | null = null;
hilCanvas.addEventListener('mousemove', (e: MouseEvent) => {
  if (_hilMoveFrame !== null) return;
  _hilMoveFrame = requestAnimationFrame(() => {
    _hilMoveFrame = null;
    const rect = hilCanvas.getBoundingClientRect();
    const hx = Math.floor(((e.clientX - rect.left) / rect.width)  * HILBERT_N);
    const hy = Math.floor(((e.clientY - rect.top)  / rect.height) * HILBERT_N);
    if (hx < 0 || hx >= HILBERT_N || hy < 0 || hy >= HILBERT_N) return;
    const index  = getHilbertD(HILBERT_N, hx, hy);
    const totalCells = HILBERT_N * HILBERT_N;
    const cell_s = index / totalCells;
    const cell_e = (index + 1) / totalCells;
    showTooltip(tooltip, e,
      applyLayerFilter(chunksVis, hilLayerFilter).filter(c => c.s <= cell_e && c.e >= cell_s)
    );
  });
});
```

**Estimated improvement:** O(2N) per event → O(2N) per animation frame (at most once per 16 ms regardless of event rate). Effectively up to 60× reduction in comparisons during active cursor movement.

**Risk:** Very low. Identical to the pattern that was already in use for the 1D bar canvas.

---

### Finding C — MEDIUM: `getAllocatorSortedStarts` sorts twice (redundant sort-by-id before sort-by-s)

**File:** `canvas.ts:311-313`

```typescript
function getAllocatorSortedStarts(chunks: ChunkVis[]): ChunkVis[] {
  return getAllocatorChunks(chunks)   // filter + sort by id   ← unnecessary
    .slice()
    .sort((a, b) => a.s - b.s);      // sort by s
}
```

`getAllocatorChunks` sorts by `id` (for the scatter plot). `getAllocatorSortedStarts` only needs chunks sorted by `s` for gap analysis — the intermediate sort by `id` is never used here and is immediately discarded.

**Current complexity:** O(2 N log N) where O(N log N) is wasted on a sort that is thrown away.

**Recommended fix:**

```typescript
function getAllocatorSortedStarts(chunks: ChunkVis[]): ChunkVis[] {
  return chunks
    .filter(c => Number.isFinite(c.id) && Number.isFinite(c.s) && Number.isFinite(c.e))
    .sort((a, b) => a.s - b.s);  // sort by s directly
}
```

**Risk:** Low. `getAllocatorSortedStarts` is only used by `getAllocatorNormalizedGaps` → `computeNormalizedGapMetrics`, none of which depend on id ordering.

---

### Finding D — LOW: `applyHeatmapLayerFilter` / `applyLayerFilter` called redundantly inside `redrawAll` and tooltip handlers

**File:** `dashboard.ts:65-68, 551`

`redrawAll` calls:
- `applyHeatmapLayerFilter(chunksVis, hmLayerFilter)` — O(N) filter
- `applyLayerFilter(chunksVis, hilLayerFilter)` — O(N) filter

The tooltip mousemove handler also calls `applyLayerFilter(chunksVis, hilLayerFilter)` independently.

These filtered views change only when `chunksVis` is updated (every 5 s) or when the user changes a filter button. Caching the last-filtered result and invalidating it only on change would avoid redundant scans during tooltip hover while a redraw is in progress.

**Risk:** Low — but requires care to avoid stale cache if `chunksVis` is mutated in-place (it is currently reassigned, not mutated, so a simple `let cachedHilChunks` with a `chunksVis` reference comparison would work).

---

### Summary Table

| # | Location | Current | After fix | Impact on tab switch | Risk |
|---|----------|---------|-----------|----------------------|------|
| A | `canvas.ts:536`, `dashboard.ts:429-438` | O(5 N log N) sorts/update | O(2 N log N) | Reduces JS blocking time ~40% per 5s tick | Low |
| B | `dashboard.ts:538-552` | O(2N)/mousemove, unthrottled | O(2N)/frame (≤60fps cap) | Not on tab switch path; fixes jank during hover | Very low |
| C | `canvas.ts:311-313` | O(2 N log N) in sort-by-s | O(N log N) | Reduces JS blocking time, feeds into Finding A | Low |
| D | `dashboard.ts:65-68,551` | O(N) filter re-run | O(1) cache hit | Minor, not on hot path | Low |

**Biggest immediate win:** Fixing Finding A (sharing `sortedByS` between `drawAllocatorDiagnostics` and `exportNormalizedGapMetrics`) eliminates 3 of the 5 O(N log N) sorts per `updateDashboard` call with minimal code change.

**Tab switching root cause is the backend mutex**, not frontend complexity. See `review/2026-05-23-tab-switching-slowdown-root-cause.md`.
