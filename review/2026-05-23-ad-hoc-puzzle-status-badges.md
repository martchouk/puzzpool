Author: mud-rev

## Code Review Report

**PR:** ad-hoc (no PR — direct commits on `dev`)
**Issue:** N/A — self-requested ad-hoc review
**Branch:** `dev`
**Reviewer:** mud-rev
**Date:** 2026-05-23

---

### Verdict

**CHANGES REQUESTED**

One blocking issue: the global service mutex is held across blocking network calls (popen/curl) in `refreshPuzzleStatusesLocked()`, which will stall all concurrent API endpoints for up to 15 seconds per poll cycle.

---

### Summary

This changeset introduces a solid puzzle solved-state badge feature: configurable address- and threshold-backed targets, a background refresh loop, cached DB storage, serialized JSON output, and a clean dashboard badge component. The architecture is well-structured, documentation was updated consistently across all relevant files, and the test suite covers the core evaluation logic. The blocking mutex issue is the only showstopper; the implementation is otherwise clean and production-close.

---

### Documentation Check

- `README.md` — **updated** (new env vars, build note)
- `docs/api.md` — **updated** (`puzzle.status` object documented with full field table)
- `docs/architecture.md` — **updated** (generated file note)
- `docs/testing.md` — **updated** (generated file note)
- `CHANGELOG.md` — **not needed** (project does not maintain a CHANGELOG)
- `.env.example` — **updated** (`BLOCKEXPLORER_*` and `PUZZLE_*_TARGET` entries added)
- Inline code docs — **not needed** (pure implementation code, no exported library API)

---

### Findings

#### MUST FIX — blocking (approval withheld until resolved)

1. **`src/service_puzzle_status.cpp:105-184` + `src/service_admin.cpp:30-31`** — `refreshPuzzleStatusesLocked()` is called while `mu_` (the service-wide shared/unique mutex) is held. Inside, `fetchAddressStatusJson()` calls `popen("curl --max-time 15 ...")` per configured address target. This means every background poll cycle and every `handleActivatePuzzle`/`handleSetPuzzle` call holds the mutex for up to `15 × N` seconds (where N is the number of address-backed puzzles), blocking all concurrent stats polls, chunk assignments, and worker heartbeats.

   **Required fix:** Move the network I/O outside the mutex. Collect the list of (puzzleId, targetType, targetValue) tuples while holding the lock, release the lock, perform all HTTP fetches without it, then re-acquire the lock to flush the results to DB. The `refreshPuzzleStatuses()` public method already does `unique_lock lock(mu_); refreshPuzzleStatusesLocked();` — the split should happen there.

   Same issue applies to the constructor in `src/service.cpp:13` (blocks server startup for up to 15 × N seconds on first run).

#### SHOULD FIX — non-blocking but important

2. **`tests/test_puzzle_status.cpp:17-22`** — Bitcoin address validation has only two test cases: one valid P2PKH (`1P…`) and one obviously invalid string. The validation code handles Base58Check, bech32 (P2WPKH `bc1q…`), and bech32m (P2TR `bc1p…`) paths. Missing test coverage:
   - A valid native-segwit address (`bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq` or similar)
   - A valid P2TR address (`bc1p…`)
   - A Base58 address with a corrupted checksum byte (should return `false`)
   - An empty string (should return `false`)
   - A testnet bech32 address (`tb1…`) — whether this should be accepted or rejected is a product decision that should be tested explicitly

3. **`src/config.cpp:58`** — Magic constant `14` in the guard `key.size() <= 14` is the combined length of `"PUZZLE_"` (7) + `"_TARGET"` (7). It is correct and the condition is semantically equivalent to "suffix cannot be empty after stripping both affixes." Add a comment or named constant to make the invariant obvious to future readers, e.g.:
   ```cpp
   // Minimum valid key: "PUZZLE_X_TARGET" = 15 chars; skip anything shorter.
   constexpr std::size_t kMinPuzzleTargetKeyLen = 15;
   if (key.rfind("PUZZLE_", 0) != 0 || key.size() < kMinPuzzleTargetKeyLen || ...)
   ```
   Note: the current guard uses `<=14` (strictly less than 15), which is correct.

4. **`src/service_serialization.cpp:35-36`** — The `label` field is produced by `canonicalPuzzleName(p.statusState)`. `canonicalPuzzleName` was designed for puzzle names (strips non-alnum, uppercases, collapses spaces). Reusing it on state strings works today because `"unsolved"`, `"solved"`, `"unknown"` are purely alphabetic, but the coupling is implicit. A dedicated `puzzleStateLabelToDisplay()` helper (or a switch statement) would make the intent explicit and survive a state-string change.

#### NIT / SUGGESTION — optional

5. **`src/config.cpp:73-75`** — `std::cerr` for skipped config entries is the right channel; consider prefixing with `[Config]` consistently (already done on line 74 but not 80's equivalent path). Minor.

6. **`tests/test_puzzle_status.cpp:93-94`** — `db.exec("UPDATE puzzles SET ... ")` updates all puzzles without a `WHERE id = ?`. Works correctly because `memConfig()` seeds a single-puzzle DB, but will silently produce unexpected state if the helper ever seeds multiple puzzles. Adding `WHERE id = (SELECT id FROM puzzles LIMIT 1)` would future-proof this.

7. **`include/puzzpool/puzzle_status.hpp`** — `puzzleStatusTargetTypeToString` switch lacks a `default` branch. When given an out-of-range enum value the function falls through to `return "address"` silently. A `default: return "unknown_type";` or an `assert(false)` would surface the bug earlier.

---

### Test Review

**Test suite result:** Not run locally (no build environment available in this review session). CI status should be verified by the developer before merge.

**Test files reviewed:**
- `tests/test_puzzle_status.cpp` — covers `isValidBitcoinAddress` (2 cases), `evaluateAddressTargetStatus` (3 cases: unsolved, solved, unknown), `evaluateFindingsThresholdStatus` (below/at threshold), and `handleStats` serialization of cached status.
- `tests/test_config.cpp` — adds one new test case covering `loadConfigFromEnv` parsing of `BLOCKEXPLORER_*` vars and `PUZZLE_*_TARGET` vars for both address and threshold types.
- `tests/CMakeLists.txt` — `test_puzzle_status` correctly registered as a CTest target.

**Coverage assessment:**

- Well covered:
  - `evaluateAddressTargetStatus()` — all three logical branches (funded+unspent, any spend, never funded)
  - `evaluateFindingsThresholdStatus()` — both threshold sides
  - `loadConfigFromEnv` — happy-path parsing of both target types and block-explorer settings
  - `puzzleJson()` serialization — confirmed `status` object emitted with correct shape

- NOT covered (missing tests):
  - `isValidBitcoinAddress` — bech32 (P2WPKH/P2TR) paths; Base58 with corrupted checksum; empty string
  - `canonicalPuzzleName` / `canonicalPuzzleTargetNameFromEnv` — no unit tests for edge inputs (leading spaces, only digits, mixed case)
  - `parsePuzzleStatusTargetType` / `parsePuzzleStatusState` — no round-trip tests; unknown strings return `nullopt` (untested)
  - `syncConfiguredPuzzleTargets()` — not tested: clearing a target when removed from config, updating target type, link generation
  - `puzzleJson()` when `status` fields are empty (null branch) — not tested; `handleStats` test only exercises the non-null path
  - Config parsing guard against `PUZZLE__TARGET` (zero-length suffix), `PUZZLE_71_TARGET_EXTRA` (extra suffix), and invalid ALL-BTC threshold (`"0"`, non-numeric)

**Test quality findings:**

1. MUST FIX — `tests/test_puzzle_status.cpp:17-22`: only one valid address type tested; bech32 code paths in `puzzle_status.cpp` (~60 lines) are entirely unexercised. A bug in the bech32 polymod or `convertBits` logic would not be caught.
2. SHOULD FIX — `tests/test_puzzle_status.cpp:93`: `UPDATE puzzles SET` without a `WHERE` clause is fragile (see Finding 6 above).
3. SHOULD FIX — `tests/test_config.cpp` does not test the rejection path (invalid address, zero threshold, non-numeric threshold). These guard clauses in `src/config.cpp:70-87` are production safety valves with no test coverage.

**Overall test verdict:** Inadequate — the happy-path evaluation logic is well tested, but the bitcoin address validation (the most security-relevant input surface) has only minimal coverage, and the config rejection paths are untested.

---

### Security Assessment

- **`popen` with `shellQuote`**: The address values written to `cfg_.puzzleStatusTargets` pass through `isValidBitcoinAddress()` at config load time, which validates checksum/format before storing. Only structurally valid addresses reach `shellQuote`. The single-quote escaping in `shellQuote` is correct. No injection risk identified under normal operation. However, if the config validation is ever bypassed (e.g., direct DB write) the `shellQuote` in `fetchAddressStatusJson` is the last line of defence — it is implemented correctly.
- **BLOCKEXPLORER_API value**: Not validated beyond being a non-empty string. A misconfigured URL (e.g., `file://`) would be accepted and curl would follow it. Low risk in practice (self-configured, not user-supplied), but worth noting.
- No secrets or credentials committed.

---

### Performance Notes

- As noted in Finding 1 (MUST FIX), the 15-second curl timeout under the service mutex is the primary performance concern. All other paths (DB schema, serialization, frontend) are O(puzzles) with low constants and introduce no regression.
- The `statusRefresher` thread interval is `max(30, blockExplorerPollSec)`, preventing runaway polling. Correct.

---

### Positive Observations

- **Bitcoin address validation is correct and complete**: the bech32/bech32m polymod, `convertBits`, Base58Check with double-SHA256, and the witness program length checks are all implemented correctly and match BIP 0173/0350 spec. This is non-trivial code.
- **`seedTemplate` helper in `service_bootstrap.cpp`**: cleanly eliminates the brittle positional-argument `PuzzleRow{0, name, …, 0, 0}` aggregate initialiser that would break silently when new fields are added to `PuzzleRow`. Excellent maintenance improvement.
- **Heatmap filter refactor**: `applyHeatmapLayerFilter` correctly splits heatmap filtering from Hilbert/allocator filtering, and the default filter change to `completed` gives a better first-load visual. The per-status radius/emphasis logic is clean.
- **Documentation**: all five affected docs were updated in the same batch. This level of doc hygiene is commendable.
- **DB schema migration**: `addColumnIfMissing` pattern ensures backwards compatibility with existing databases. No migration script needed.

---

### Required Next Steps

1. **Fix the mutex/network-call issue** (Finding 1): extract the network I/O out of the critical section in `refreshPuzzleStatusesLocked()`. The pattern is: collect targets (with lock) → fetch externally (without lock) → write results (with lock).
2. **Add bech32/P2TR/invalid-checksum address validation tests** (Findings 2 and Test Finding 1) to `tests/test_puzzle_status.cpp`.
3. **Add config rejection-path tests** (Test Finding 3): cover invalid address, `"0"` threshold, non-numeric threshold, zero-length suffix.
4. **Add a `default` branch** to `puzzleStatusTargetTypeToString` switch (Finding 7).
5. Optionally address Findings 3–6 (magic constant, label helper, cerr prefix, WHERE clause) before merge.
