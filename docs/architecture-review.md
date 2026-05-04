# Architecture Review — Arbitrary Keyspace Support (Issue #87)

**Review date:** 2026-05-04  
**Reviewer:** core team  
**Scope:** C++ server — virtual chunk allocator, persistence layer, stats, reclaim, worker lifecycle  
**Method:** Code inspection from current `dev` branch, cross-referenced against project docs

---

## 1. Architecture Overview

### Puzzle metadata

Each puzzle row (`puzzles` table) stores:

| Column | Type | Purpose |
|--------|------|---------|
| `start_hex` / `end_hex` | TEXT | Absolute key range boundaries |
| `alloc_strategy` | TEXT | `virtual_random_chunks_v1` or `legacy_random_shards_v1` |
| `alloc_seed` | TEXT | Deterministic permutation seed |
| `alloc_cursor_hex` | TEXT (64-char hex) | Current position in permutation order |
| `virtual_chunk_size_keys` | TEXT | Size of one virtual chunk in keys (decimal string) |
| `virtual_chunk_count_hex` | TEXT (64-char hex) | Total number of virtual chunks |

Both `alloc_cursor_hex` and `virtual_chunk_count_hex` use 64-character zero-padded hex so they can represent values up to 2^256−1 — sufficient for any Bitcoin keyspace.

### Virtual index space

The allocator conceptually divides the key range `[start_hex, end_hex)` into `N = ceil(range / chunk_size)` fixed-size virtual chunks, numbered `[0, N)`. This number is computed using `cpp_int` arithmetic (`src/allocator.cpp:95-106`) and stored only as a hex string; the virtual chunks themselves are **never materialised as database rows**.

### Job generation

When a worker requests work, the allocator:

1. Reads the current `alloc_cursor_hex` as `cpp_int`.
2. Applies a Feistel or affine permutation to map cursor → virtual chunk index, ensuring a pseudo-random traversal order.
3. Computes how many virtual chunks to bundle into a single job based on the worker's hashrate and the target scan duration.
4. Writes **one `chunks` row** covering the contiguous virtual chunk span `[runStart, runEnd)` plus the corresponding absolute key range.
5. Advances the cursor to `(orderIndex + 1) % totalChunks`.

### Persistence model

Only **operational state** is persisted:

- Started/reclaimed/completed job runs (`chunks` table).
- Worker metadata (`workers` table).
- Key findings (`findings` table).

The full virtual chunk space (up to 2^231 entries for All BTC) is never written to the database.

### Worker lifecycle

`assigned` → (timeout or reactivation) → `reclaimed` → `assigned` (to next worker)  
`assigned` → (submit done/FOUND) → `completed` / `FOUND`

### Statistics

Stats are computed on-demand from the `chunks` table. Virtual chunk counts are tracked by summing `(vchunk_end_hex − vchunk_start_hex)` over all chunk rows, using `cpp_int` arithmetic to avoid overflow.

---

## 2. Architecture Decision Record

### ADR-1: Virtual index space modelled as cpp_int, never materialised

**Context:** Bitcoin keyspace puzzle #71 has ~2^67 keys; All BTC has ~2^256 keys. A naive approach of creating one row per virtual chunk would require storing 2^231 rows for All BTC — infeasible.

**Decision:** The total chunk count and cursor are stored as 64-char zero-padded hex TEXT. All arithmetic uses Boost Multiprecision `cpp_int`. The database stores only issued job runs.

**Consequences:** The schema supports any keyspace size. Memory and storage scale with active workers, not with keyspace size.

---

### ADR-2: 64-char zero-padded hex for all large integer columns

**Context:** SQLite has no native bigint type. SQL `<`/`>` comparisons on hex strings are only semantically correct when all values have the same width.

**Decision:** All virtual-domain values (`alloc_cursor_hex`, `virtual_chunk_count_hex`, `vchunk_start_hex`, `vchunk_end_hex`) are written via `intToHex(v, 64)`, producing exactly 64 hex characters.

**Consequences:** Lexicographic SQL ordering matches numeric ordering. Overlap detection (`WHERE vchunk_start_hex < ? AND vchunk_end_hex > ?`) is correct for any value in [0, 2^256).

---

### ADR-3: Job aggregation — one chunk row per job run, not per virtual chunk

**Context:** A worker scanning at 100 MH/s against a 33 M-key virtual chunk size would need ~900 virtual chunks to fill a 5-minute target window. Creating 900 rows per job assignment is wasteful.

**Decision:** A single `chunks` row stores the entire run span `[vchunk_start_hex, vchunk_end_hex)`. The absolute key range is derived from these indices at assignment time and stored in `start_hex`/`end_hex`.

**Consequences:** Row count is bounded by `(active workers × history depth)`, not by keyspace size. Stats must iterate chunk rows and sum spans rather than count rows.

---

### ADR-4: Deterministic permutation for allocation order

**Context:** Sequential allocation would produce obvious patterns and allow workers to predict each other's jobs.

**Decision:** Each allocation uses `permuteIndexFeistel(orderIndex, totalChunks, permKey)` (or affine variant) to map a sequential cursor position to a pseudo-random virtual chunk index. The permutation is seeded from `alloc_seed` and is fully deterministic.

**Consequences:** Same puzzle + seed always produces the same allocation sequence. No two workers can receive overlapping virtual chunk ranges from the same sequential cursor advance.

---

## 3. Dimension-by-Dimension Findings

### D1 — Virtual keyspace representation

| | |
|---|---|
| **Evidence** | `include/puzzpool/types.hpp`: `cpp_int virtualChunkCount`; `src/db.cpp`: `virtual_chunk_count_hex TEXT`; `src/allocator.cpp:106`: `intToHex(chunkCountBig, 64)` |
| **Verdict** | ✅ Pass |

The total virtual chunk count is stored as 64-char hex TEXT and read back as `cpp_int`. The tests in `tests/test_vchunk.cpp:234` confirm counts exceeding INT64_MAX are correctly stored and retrieved.

---

### D2 — Cursor semantics

| | |
|---|---|
| **Evidence** | `include/puzzpool/types.hpp`: `cpp_int allocCursor`; `src/allocator.cpp:307-312`: `setAllocCursor()` writes `intToHex(cursor, 64)`; `src/allocator.cpp:251`: `cpp_int nextCursor = (orderIndex + 1) % totalChunks` |
| **Verdict** | ✅ Pass |

The cursor is bounded by construction: `(orderIndex + 1) % totalChunks` ensures it stays in `[0, totalChunks)`. For All BTC this is `[0, ~2^231)`, handled entirely in `cpp_int`.

---

### D3 — Permutation correctness

| | |
|---|---|
| **Evidence** | `src/allocator.cpp:244-247`: Feistel/affine applied to `orderIndex ∈ [0, totalChunks)`; `src/allocator.cpp:346-352`: `normalizeRunStartForCandidate()` clamps result |
| **Verdict** | ✅ Pass |

Both permutations map `[0, totalChunks) → [0, totalChunks)` bijectively. Run start is further clamped: if `runStart + neededChunks > totalChunks`, start is set to `totalChunks − neededChunks`, guaranteeing the run stays within bounds.

**Note:** Permutation correctness for domains > INT64_MAX is not covered by tests (only seeding is tested in `LargeDomainFixture`). See Risk R3.

---

### D4 — Job aggregation

| | |
|---|---|
| **Evidence** | `src/allocator.cpp:210-218`: `neededChunks = ceilDiv(requestedKeys, vchunkSize)`; `src/allocator.cpp:288-295`: hashrate-to-keys calculation using `cpp_int` |
| **Verdict** | ✅ Pass |

The chain `hashrate → requestedKeys → neededChunks → [runStart, runEnd)` is computed entirely in `cpp_int`. A 100 MH/s worker against a 33 M-key virtual chunk size receives approximately 900 virtual chunks (≈30 B keys), confirmed by `tests/test_vchunk.cpp:256-267`.

---

### D5 — Persistence scope

| | |
|---|---|
| **Evidence** | `src/allocator.cpp:433-450`: one `INSERT INTO chunks` per job; `src/db.cpp:51-68`: `chunks` table schema; `docs/database.md:39-61` |
| **Verdict** | ✅ Pass |

One `chunks` row is created per job assignment. For All BTC (~2^231 virtual chunks), row count is bounded by the number of jobs ever issued, not by the keyspace size.

---

### D6 — Chunk/job boundary precision

| | |
|---|---|
| **Evidence** | `src/allocator.cpp:446-447`: `intToHex(runStart, 64)`, `intToHex(runEnd, 64)`; `src/allocator.cpp:328-339`: SQL overlap check on hex strings |
| **Verdict** | ✅ Pass |

All virtual chunk boundaries are stored as 64-char zero-padded hex. SQL overlap detection (`vchunk_start_hex < ? AND vchunk_end_hex > ?`) is correct because lexicographic ordering matches numeric ordering for same-width hex strings.

---

### D7 — Reclaim correctness

| | |
|---|---|
| **Evidence** | `src/work_service.cpp:87-108`: timeout reclaim via `UPDATE chunks SET status='reclaimed' WHERE heartbeat_at < datetime('now', ?)` and `is_test = 0`; `src/work_service.cpp:26-35`: reactivation batch reclaim |
| **Verdict** | ✅ Pass |

Reclaim operates at the complete chunk row level. No partial reclaims are possible — the SQL `UPDATE` flips the entire row atomically. On reactivation all assigned chunks for the returning worker are reclaimed in a single statement before new work is issued.

---

### D8 — Worker reactivation

| | |
|---|---|
| **Evidence** | `src/work_service.cpp:110-153`: `upsertWorkerAndDetectReactivation()` — checks `last_seen < datetime('now', -REACTIVATE_MINUTES)` |
| **Verdict** | ✅ Pass |

A worker absent for longer than `REACTIVATE_MINUTES` is detected on its next `/work` call. All its previously assigned chunks are reclaimed before new work is issued, preventing a worker from holding stale jobs indefinitely.

---

### D9 — Statistics accuracy

| | |
|---|---|
| **Evidence** | `src/service_stats.cpp:280-305` |
| **Verdict** | ⚠️ Conditional pass — semantics are correct but the field names are ambiguous |

The `virtual_chunks.started` and `virtual_chunks.completed` fields report the **sum of virtual chunk index spans** across all chunk rows:

```cpp
// service_stats.cpp:293-295
started += (e - s);   // e, s are vchunk_end_hex, vchunk_start_hex as cpp_int
if (status == "completed" || status == "FOUND") completed += (e - s);
```

This value equals the number of virtual chunk slots covered — which is numerically correct. However, the JSON field name `started` could be misread as "keys scanned". For a 33 M-key chunk size, a caller must multiply by `virtual_chunk_size_keys` to obtain actual keys. No such computed field is provided. See Risk R2.

---

### D10 — Int64 bottlenecks

| | |
|---|---|
| **Evidence** | `src/allocator.cpp:237, 243, 262, 384, 395, 409` |
| **Verdict** | ⚠️ Conditional pass — safe in practice, fragile by design |

Five `static_cast<int64_t>` sites exist in the allocator:

```cpp
// allocator.cpp:237
int64_t probeLimit = static_cast<int64_t>(std::min(totalChunks, cpp_int(cfg_.maxAllocProbes)));

// allocator.cpp:384, 395, 409
int64_t probes = static_cast<int64_t>(minBig(cpp_int(maxStart + 1), cpp_int(cfg_.maxAllocProbes)));
```

In all cases the value being cast is the result of `min(…, cfg_.maxAllocProbes)` where `maxAllocProbes` is a small integer (default 8192), so the result always fits in int64_t. The casts are **safe in practice** but would silently produce wrong values if `maxAllocProbes` were ever set above `2^63 − 1`, or if the logic were changed without noticing the cast. See Risk R1.

`src/allocator.cpp:304` also casts a `REAL` column to `int64_t`, which is acceptable since it represents a key count derived from hashrate.

---

### D11 — Doc/code consistency

| | |
|---|---|
| **Evidence** | `docs/architecture.md`, `docs/database.md`, `docs/api.md` cross-referenced against source |
| **Verdict** | ✅ Pass |

The existing documentation accurately describes the virtual chunk model, the permutation approach, the chunk lifecycle, and the stats semantics. Minor gap: `docs/architecture.md` does not explicitly discuss the All BTC scenario or the 64-char hex strategy for large values. This review document addresses that gap.

---

## 4. Risk Register

### R1 — Fragile int64 casts in probe loop (LOW)

**Location:** `src/allocator.cpp:237, 243, 262, 384, 395, 409`

**Description:** `static_cast<int64_t>` applied to a `cpp_int` that is the result of `min(totalChunks, maxAllocProbes)`. Safe today because `maxAllocProbes ≤ 8192`. Would silently corrupt values if `maxAllocProbes` were raised above `INT64_MAX` or if surrounding logic changed.

**Recommended fix:**
```cpp
// Replace:
int64_t probeLimit = static_cast<int64_t>(std::min(totalChunks, cpp_int(cfg_.maxAllocProbes)));
for (int64_t offset = 0; offset < probeLimit; ++offset)

// With:
cpp_int probeLimit = std::min(totalChunks, cpp_int(cfg_.maxAllocProbes));
for (cpp_int offset = 0; offset < probeLimit; ++offset)
```

---

### R2 — Ambiguous virtual_chunks stats field (LOW)

**Location:** `src/service_stats.cpp:298-299`

**Description:** `virtual_chunks.started` and `virtual_chunks.completed` report virtual chunk index spans, not key counts. For a 33 M-key chunk size the caller must multiply to get actual keys. No hint is provided in the response.

**Recommended fix:** Add a `virtual_chunk_size_keys` field alongside `virtual_chunks` in the stats response, or rename the subfields to `started_vchunks` / `completed_vchunks` to distinguish them from `total_keys_completed`.

---

### R3 — Permutation not tested for > INT64_MAX domain (LOW)

**Location:** `tests/test_vchunk.cpp:219-279` (LargeDomainFixture)

**Description:** `LargeDomainFixture` confirms that `seedVirtualChunks` succeeds and that the count is stored correctly for the All BTC domain. It does not verify that the Feistel permutation produces non-repeating outputs when iterated, or that allocation produces non-overlapping chunks, at this scale.

**Recommended fix:** Extend `LargeDomainFixture` with a test that assigns chunks to many workers and verifies non-overlap, similar to the existing small-domain test at `tests/test_vchunk.cpp:97-112`.

---

### R4 — Bootstrap stage not tested for large domains (LOW)

**Location:** `src/allocator.cpp:381-415` (`findBeginBootstrapRun`, `findEndBootstrapRun`, `findMidBootstrapRun`)

**Description:** The three bootstrap stages (assign from mid, begin, end of permutation space) are not exercised in the large-domain test fixture. The logic looks correct but is untested at scale.

---

## 5. Mandatory Invariant Verdicts

| # | Invariant | Verdict | Evidence |
|---|-----------|---------|----------|
| I1 | All-BTC keyspace modelable without size inflation | ✅ **Pass** | `virtual_chunk_count_hex` as hex TEXT; cpp_int arithmetic; test confirms count > INT64_MAX |
| I2 | Workers receive practical-sized jobs | ✅ **Pass** | hashrate → neededChunks via cpp_int; test confirms ≈30 B key job for 100 MH/s worker |
| I3 | Full virtual space never persisted as DB rows | ✅ **Pass** | One `chunks` row per job, not per virtual chunk; seeding writes only puzzle metadata |
| I4 | No job overlaps possible | ✅ **Pass** | `rangeIsFree()` hex-string overlap check before every allocation; permutation cursor advances sequentially |
| I5 | Reclaim is exact (job-level, not partial) | ✅ **Pass** | SQL `UPDATE chunks SET status='reclaimed'` flips entire row; no partial reclaim path exists |
| I6 | Large virtual indices not int64-limited | ⚠️ **Conditional Pass** | Core storage and arithmetic use cpp_int; 5 fragile int64 casts in probe loops are safe in practice (result ≤ 8192) but should be removed |
| I7 | Stats correctly separate vchunks / jobs / keys | ⚠️ **Conditional Pass** | Counts are numerically correct; field names `started`/`completed` under `virtual_chunks` are ambiguous — callers must multiply by `virtual_chunk_size_keys` to derive key counts |

---

## 6. Overall Verdict

**CONDITIONAL PASS**

The core architecture is sound and meets the design goals for arbitrarily large keyspaces. All five pass-critical invariants (I1–I5) hold without qualification. The two conditional passes (I6, I7) represent low-risk code-quality and API-clarity issues that do not affect correctness today.

**Required before unconditional pass:**

1. **(R1)** Refactor the 5 `int64_t` probe-loop casts to `cpp_int` in `src/allocator.cpp` — prevents a silent future regression.
2. **(R2)** Clarify the `virtual_chunks` stats semantics — either rename subfields or add `virtual_chunk_size_keys` to the response so callers can derive key counts without guessing.

**Recommended (non-blocking):**

3. **(R3)** Add a large-domain non-overlap test to `LargeDomainFixture`.
4. **(R4)** Add a large-domain bootstrap-stage coverage test.
