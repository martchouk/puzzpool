Author: mud-rev

## Tab Switching Slowdown — Root Cause Analysis

**Date:** 2026-05-23
**Symptom:** Loading time when switching puzzle tabs increased massively after today's changes.

---

### Root cause: backend mutex held during blocking network I/O

The slowdown is a direct consequence of **Finding 1 from the main review** (`src/service_puzzle_status.cpp:105`).

The new `refreshPuzzleStatuses()` acquires the exclusive service mutex `mu_` and then calls `fetchAddressStatusJson()`, which shells out to `curl --max-time 15`. While the mutex is held, *every* concurrent HTTP request that needs `mu_` — including the stats endpoint that tab switching calls — is blocked.

#### Request path on tab click

```
[Browser] Tab click
  → fetchStats(selectedId)
    → GET /api/v1/stats?puzzle_id=X
      → handleStats() → std::shared_lock lock(mu_)   ← BLOCKED if any unique_lock is held
        → reads puzzle + chunks from DB
        → returns JSON (includes new status fields)
  → updateDashboard() paints new tab data
```

#### What holds the exclusive lock

Three places call `refreshPuzzleStatusesLocked()` while holding `std::unique_lock lock(mu_)`:

| Call site | When it fires |
|-----------|--------------|
| `PoolService` constructor (`src/service.cpp:13`) | Server startup — blocks first request until all curl calls complete |
| Background thread (`src/main.cpp:114-121`) | Every `max(30, BLOCKEXPLORER_POLL_SEC)` seconds (default: 600 s) |
| `handleActivatePuzzle` (`src/service_admin.cpp:30`) | When the user clicks the activate toggle next to any tab |
| `handleSetPuzzle` (`src/service_admin.cpp:131`) | When a puzzle is created or reconfigured |

The blocking duration is up to **15 seconds per configured address target** (`--max-time 15` in curl). With N targets, the window is up to 15×N seconds.

#### Why it appears after switching tabs

- If `BLOCKEXPLORER_POLL_SEC` is left at the default (600 s), the background thread fires once every 10 minutes — so the slowness is intermittent (any tab switch landing inside that window stalls for up to 15 s).
- If `BLOCKEXPLORER_POLL_SEC` is set small (e.g. 30 s for testing), the mutex is held ~50 % of the time, making almost every tab switch visibly slow.
- If the user recently clicked the activate toggle (switching which puzzle is active), `handleActivatePuzzle` fires the refresh immediately and synchronously within the lock, blocking the next stat poll.

#### Fix (from the main review, Finding 1)

Lift the network I/O **out of the mutex**:

```cpp
void PoolService::refreshPuzzleStatuses() {
    // Step 1: collect targets while locked (fast DB read)
    std::vector<PuzzleTarget> targets;
    {
        std::unique_lock lock(mu_);
        syncConfiguredPuzzleTargets();
        targets = collectPuzzleTargets();  // reads DB, no network
    }
    // Step 2: fetch externally WITHOUT the lock (slow, up to 15 s × N)
    auto results = fetchAllStatuses(targets);

    // Step 3: write results back while locked (fast DB write)
    std::unique_lock lock(mu_);
    flushStatusResults(results);
}
```

Until that fix is applied, any deployment with a configured `PUZZLE_*_TARGET` will show intermittent (or frequent) tab-switching freezes.
