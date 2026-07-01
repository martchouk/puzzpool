# Scores Last Seen Column Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Last seen` column to the `Scores — All Time` dashboard table, backed by `workers.last_seen`, with muted styling for timestamps from the last hour and white styling for older or missing timestamps.

**Architecture:** Extend the `/api/v1/stats` score payload in `src/service_stats.cpp` so each score row carries a nullable `last_seen`, then thread that field through the frontend type layer and table renderer. Keep the age threshold decision in the frontend so the backend stays a pure data provider and the UI can distinguish recent vs stale/null timestamps with a small formatting helper.

**Tech Stack:** C++17, SQLite, nlohmann/json, TypeScript, Vitest, Vite, Catch2, CTest

---

## File Structure

- Modify: `src/service_stats.cpp`
  Add `last_seen` to each score row while preserving the existing total-key aggregation and sort order.
- Modify: `tests/test_handler_validation.cpp`
  Add backend regression tests for score rows with a matching worker and score rows whose worker no longer exists.
- Modify: `frontend/src/types.ts`
  Extend `ScoreEntry` with `last_seen: string | null`.
- Modify: `frontend/src/format.ts`
  Add a small helper for the one-hour recency check so the rendering rule is testable.
- Modify: `frontend/src/format.test.ts`
  Add unit coverage for recent, exact-threshold, stale, null, and invalid timestamp cases.
- Modify: `frontend/src/dashboard.ts`
  Render the new score column, keep `fmtUtc()` formatting, and update the empty-state colspan.
- Modify: `frontend/index.html`
  Add score-specific timestamp classes for the new column without changing shared `.td-time` styling.
- Modify: `frontend/src/accessibility.test.ts`
  Add static regression checks for the header, score-specific stale style, updated score-table rendering hooks, and unchanged worker/finder timestamp classes.
- Modify: `docs/api.md`
  Document the new `scores[].last_seen` field and update the example payload.

### Task 1: Extend the stats payload and lock it with backend tests

**Files:**
- Modify: `tests/test_handler_validation.cpp`
- Modify: `src/service_stats.cpp`

- [ ] **Step 1: Add a failing regression for score rows that still have a matching worker**

```cpp
TEST_CASE("handleStats: score entries include last_seen when worker row exists", "[handler][stats][scores]") {
    Config cfg = memConfig();
    cfg.dbPath = "test-score-last-seen-present.db";
    PoolService svc{cfg};

    crow::request workReq;
    workReq.body = R"({"name":"score-worker","hashrate":1000000})";
    auto workResp = svc.handleWork(workReq);
    REQUIRE(workResp.code == 200);
    const auto jobId = json::parse(workResp.body)["job_id"].get<int64_t>();

    crow::request submitReq;
    submitReq.body = json{
        {"name", "score-worker"},
        {"job_id", jobId},
        {"status", "done"},
        {"keys_scanned", "99999999999999999999"}
    }.dump();
    REQUIRE(svc.handleSubmit(submitReq).code == 200);

    PoolDb db{cfg.dbPath};
    db.exec("UPDATE workers SET last_seen = '2026-07-01 09:30:00' WHERE name = 'score-worker'");

    crow::request statsReq;
    auto stats = json::parse(svc.handleStats(statsReq).body);
    REQUIRE(stats["scores"].size() == 1);
    CHECK(stats["scores"][0]["worker_name"] == "score-worker");
    CHECK(stats["scores"][0]["last_seen"] == "2026-07-01 09:30:00");

    std::remove("test-score-last-seen-present.db");
}
```

- [ ] **Step 2: Add a failing regression for score rows whose worker row has disappeared**

```cpp
TEST_CASE("handleStats: score entries keep null last_seen when worker row is missing", "[handler][stats][scores]") {
    Config cfg = memConfig();
    cfg.dbPath = "test-score-last-seen-missing.db";
    PoolService svc{cfg};

    crow::request workReq;
    workReq.body = R"({"name":"retired-worker","hashrate":1000000})";
    auto workResp = svc.handleWork(workReq);
    REQUIRE(workResp.code == 200);
    const auto jobId = json::parse(workResp.body)["job_id"].get<int64_t>();

    crow::request submitReq;
    submitReq.body = json{
        {"name", "retired-worker"},
        {"job_id", jobId},
        {"status", "done"},
        {"keys_scanned", "99999999999999999999"}
    }.dump();
    REQUIRE(svc.handleSubmit(submitReq).code == 200);

    PoolDb db{cfg.dbPath};
    db.exec("DELETE FROM workers WHERE name = 'retired-worker'");

    crow::request statsReq;
    auto stats = json::parse(svc.handleStats(statsReq).body);
    REQUIRE(stats["scores"].size() == 1);
    CHECK(stats["scores"][0]["worker_name"] == "retired-worker");
    CHECK(stats["scores"][0]["last_seen"].is_null());

    std::remove("test-score-last-seen-missing.db");
}
```

- [ ] **Step 3: Run the backend target to confirm both new tests fail before the implementation**

Run: `ctest --test-dir build --output-on-failure --tests-regex handler`

Expected: the two new `[handler][stats][scores]` checks fail because `scores[]` does not yet contain `last_seen`.

- [ ] **Step 4: Update score aggregation in `src/service_stats.cpp`**

```cpp
struct ScoreAggregate {
    int64_t completedChunks = 0;
    cpp_int totalKeys = 0;
    std::string lastSeen;
    bool hasLastSeen = false;
};

std::map<std::string, ScoreAggregate> scoreMap;
SQLite::Statement dq(db_.raw(), R"SQL(
    SELECT c.worker_name, c.start_hex, c.end_hex, w.last_seen
    FROM chunks c
    LEFT JOIN workers w ON w.name = c.worker_name
    WHERE c.puzzle_id = ?
      AND (c.status = 'completed' OR c.status = 'FOUND')
      AND c.worker_name IS NOT NULL
      AND c.is_test = 0
)SQL");
dq.bind(1, puzzle.id);

while (dq.executeStep()) {
    std::string worker = dq.getColumn(0).getString();
    cpp_int s = hexToInt(dq.getColumn(1).getString());
    cpp_int e = hexToInt(dq.getColumn(2).getString());

    auto& slot = scoreMap[worker];
    slot.completedChunks += 1;
    slot.totalKeys += (e - s);
    totalKeysCompleted += (e - s);

    if (!dq.isColumnNull(3)) {
        slot.lastSeen = dq.getColumn(3).getString();
        slot.hasLastSeen = true;
    }
}
```

- [ ] **Step 5: Serialize the new field without changing score ordering**

```cpp
std::vector<std::pair<std::string, ScoreAggregate>> scoreVec(scoreMap.begin(), scoreMap.end());
std::sort(scoreVec.begin(), scoreVec.end(), [](const auto& a, const auto& b) {
    return a.second.totalKeys > b.second.totalKeys;
});

for (const auto& [worker, stats] : scoreVec) {
    scores.push_back({
        {"worker_name", worker},
        {"completed_chunks", stats.completedChunks},
        {"total_keys", bigToDec(stats.totalKeys)},
        {"last_seen", stats.hasLastSeen ? json(stats.lastSeen) : json(nullptr)}
    });
}
```

- [ ] **Step 6: Re-run the backend tests and then the full C++ suite**

Run: `ctest --test-dir build --output-on-failure --tests-regex handler`

Expected: both new score tests pass and existing handler tests stay green.

Run: `ctest --test-dir build --output-on-failure`

Expected: the full C++ suite passes.

- [ ] **Step 7: Commit the backend change set**

```bash
git add src/service_stats.cpp tests/test_handler_validation.cpp
git commit -m "feat: expose score last-seen timestamps"
```

### Task 2: Extend the frontend score contract and make the age rule testable

**Files:**
- Modify: `frontend/src/types.ts`
- Modify: `frontend/src/format.ts`
- Modify: `frontend/src/format.test.ts`

- [ ] **Step 1: Add the new API field to `ScoreEntry`**

```ts
export interface ScoreEntry {
  worker_name: string;
  completed_chunks: number;
  total_keys: string;
  last_seen: string | null;
}
```

- [ ] **Step 2: Write failing unit tests for the one-hour age check**

```ts
import { describe, it, expect } from 'vitest';
import { esc, isRecentUtc } from './format.ts';

describe('isRecentUtc()', () => {
  const now = Date.parse('2026-07-01T10:00:00Z');

  it('returns true for timestamps within the last hour', () => {
    expect(isRecentUtc('2026-07-01 09:30:00', 60 * 60 * 1000, now)).toBe(true);
  });

  it('returns true for timestamps exactly one hour old', () => {
    expect(isRecentUtc('2026-07-01 09:00:00', 60 * 60 * 1000, now)).toBe(true);
  });

  it('returns false for timestamps older than one hour', () => {
    expect(isRecentUtc('2026-07-01 08:59:59', 60 * 60 * 1000, now)).toBe(false);
  });

  it('returns false for null timestamps', () => {
    expect(isRecentUtc(null, 60 * 60 * 1000, now)).toBe(false);
  });

  it('returns false for invalid timestamps', () => {
    expect(isRecentUtc('not-a-date', 60 * 60 * 1000, now)).toBe(false);
  });
});
```

- [ ] **Step 3: Run the focused frontend test to verify it fails**

Run: `npm test --prefix frontend -- src/format.test.ts`

Expected: Vitest fails because `isRecentUtc` does not exist yet.

- [ ] **Step 4: Implement the reusable timestamp-age helper**

```ts
export function isRecentUtc(
  s: string | null | undefined,
  maxAgeMs = 60 * 60 * 1000,
  nowMs = Date.now(),
): boolean {
  if (!s) return false;
  const ts = new Date(s + 'Z').getTime();
  if (!Number.isFinite(ts)) return false;
  return nowMs - ts <= maxAgeMs;
}
```

- [ ] **Step 5: Re-run the focused test and frontend type checks**

Run: `npm test --prefix frontend -- src/format.test.ts`

Expected: the new `isRecentUtc()` cases pass.

Run: `npm run build --prefix frontend`

Expected: `tsc --noEmit` and `vite build` both succeed with the extended `ScoreEntry`.

- [ ] **Step 6: Commit the frontend contract/helper change set**

```bash
git add frontend/src/types.ts frontend/src/format.ts frontend/src/format.test.ts
git commit -m "feat: add score last-seen frontend contract"
```

### Task 3: Render the new column and protect the markup with static frontend tests

**Files:**
- Modify: `frontend/index.html`
- Modify: `frontend/src/dashboard.ts`
- Modify: `frontend/src/accessibility.test.ts`

- [ ] **Step 1: Add failing static tests for the new score-table contract**

```ts
it('adds a Last seen column to the all-time scores table', () => {
  expect(html).toMatch(/<th>Last seen<\\/th>/);
});

it('styles stale score timestamps in white with a score-specific class and updates the empty-state colspan', () => {
  expect(html).toMatch(/\\.td-score-time-stale\\s*\\{\\s*color:\\s*#fff;/);
  expect(dashboardTs).toMatch(/emptyRow\\(5, 'No completed work yet'\\)/);
});

it('renders score last_seen values through the recency helper', () => {
  expect(dashboardTs).toMatch(/const lastSeenClass = isRecentUtc\\(s\\.last_seen\\) \\? 'td-score-time' : 'td-score-time td-score-time-stale';/);
  expect(dashboardTs).toMatch(/<td class=\"\\$\\{lastSeenClass\\}\">\\$\\{fmtUtc\\(s\\.last_seen\\)\\}<\\/td>/);
});

it('keeps Visible Workers and Keys Found on the shared td-time class', () => {
  expect(dashboardTs).toMatch(/<td class="td-time">\\$\\{fmtUtc\\(w\\.last_seen\\)\\}<\\/td>/);
  expect(dashboardTs).toMatch(/<td class="td-time">\\$\\{fmtUtc\\(f\\.created_at\\)\\}<\\/td>/);
});
```

- [ ] **Step 2: Run the focused static frontend checks to confirm they fail**

Run: `npm test --prefix frontend -- src/accessibility.test.ts`

Expected: the new score-table assertions fail because the markup and renderer have not been updated.

- [ ] **Step 3: Update the score table header and stale text class in `frontend/index.html`**

```html
.td-time { color: var(--text-secondary); }
.td-score-time { color: var(--text-secondary); }
.td-score-time-stale { color: #fff; }

<thead>
  <tr>
    <th></th>
    <th>Worker</th>
    <th>Keys Completed</th>
    <th>Jobs</th>
    <th>Last seen</th>
  </tr>
</thead>
```

- [ ] **Step 4: Update the score-table renderer in `frontend/src/dashboard.ts`**

```ts
if (!data.scores?.length) {
  stbody.innerHTML = emptyRow(5, 'No completed work yet');
} else {
  stbody.innerHTML = data.scores.map((s, i) => {
    const lastSeenClass = isRecentUtc(s.last_seen) ? 'td-score-time' : 'td-score-time td-score-time-stale';
    return `<tr>
      <td class="td-rank">#${formatIntegerDots(i + 1)}</td>
      <td class="td-name">${esc(s.worker_name)}</td>
      <td class="td-chunks">${formatBigInt(s.total_keys)}</td>
      <td class="td-chunks">${formatIntegerDots(s.completed_chunks)}</td>
      <td class="${lastSeenClass}">${fmtUtc(s.last_seen)}</td>
    </tr>`;
  }).join('');
}
```

- [ ] **Step 5: Import the helper and keep the rendering rule local to the score table**

```ts
import {
  formatBigInt, formatIntegerDots, formatHashrate, fmtUtc, isRecentUtc,
  formatPrecisePercentage, trimHexRange, formatETA,
  renderWorkerProgress, allocatorDiagnosticsHtml, emptyRow, esc,
} from './format.ts';
```

- [ ] **Step 6: Re-run the focused frontend tests and the full frontend suite**

Run: `npm test --prefix frontend -- src/accessibility.test.ts src/format.test.ts`

Expected: both focused test files pass.

Run: `npm test --prefix frontend`

Expected: the full Vitest suite passes.

- [ ] **Step 7: Commit the score-table rendering change set**

```bash
git add frontend/index.html frontend/src/dashboard.ts frontend/src/accessibility.test.ts
git commit -m "feat: render score last-seen column"
```

### Task 4: Update the API docs and run the final verification pass

**Files:**
- Modify: `docs/api.md`

- [ ] **Step 1: Update the `/api/v1/stats` example payload**

```md
"scores": [
  {
    "worker_name": "rig1",
    "completed_chunks": 95,
    "total_keys": "6300000000000",
    "last_seen": "2024-01-15 12:34:56"
  }
]
```

- [ ] **Step 2: Document the new score field and null behavior**

```md
| `last_seen` | string | null | Worker activity timestamp from `workers.last_seen`; `null` when the score row no longer has a matching `workers` record |
```

- [ ] **Step 3: Run the full verification pass**

Run: `ctest --test-dir build --output-on-failure`

Expected: all C++ tests pass.

Run: `npm test --prefix frontend`

Expected: all Vitest tests pass.

Run: `npm run build --prefix frontend`

Expected: the frontend type check and production build pass.

- [ ] **Step 4: Commit the docs + final verification state**

```bash
git add docs/api.md
git commit -m "docs: document score last-seen field"
```

- [ ] **Step 5: Open the implementation PR once the branch is ready**

```bash
git push -u origin <branch-name>
gh pr create --draft --title "feat: add score last-seen column" --body "Fixes #107"
```

## Self-Review

- Spec coverage: backend join, nullable API contract, frontend type update, new score column, white stale/null styling, docs, and both backend/frontend regression coverage are mapped to tasks above.
- Placeholder scan: no `TODO`, `TBD`, or “write tests later” gaps remain.
- Type consistency: `last_seen` stays `string | null` across backend JSON, `ScoreEntry`, renderer, and docs.
