# Testing

## C++ Unit Tests (Catch2 + CTest)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Tests are built automatically (`PUZZPOOL_BUILD_TESTS=ON` by default). Each test binary
links against the `puzzpool_core` static library (all sources except `main.cpp`).

### Test coverage

| File | What is verified |
|------|-----------------|
| `tests/test_hex_bigint.cpp` | `isValidHex`, `hexToInt`, `intToHex`, `normalizeHex`, `ceilDiv`, `minBig`/`maxBig`, `bigToDec`, `normalizedRange`, `bitLength` |
| `tests/test_permutation.cpp` | Feistel: determinism, bounds `[0,n)`, 100k-sample injectivity, edge keyspace sizes; Affine: bounded, deterministic, 10k injectivity |
| `tests/test_submission.cpp` | `submitDone` (exact/overscan accepted, underscan rejected+reclaimed, wrong worker, missing fields, negative); `submitFound` (valid, deduplication, empty array, invalid hex); `clearTestChunkIfNeeded` |
| `tests/test_allocator.cpp` | `upsertWorker` (new/fresh), `assignWork` (valid chunk, idempotent, two-worker non-overlap), `reclaimChunk`, `existingAssignedChunk` (found/nullopt), `reclaimTimedOutChunks` (backdated/fresh) |

### In-memory isolation

All component tests use `:memory:` as the SQLite path (via `memConfig()` in
`tests/test_helpers.hpp`). No files are read or written; tests are fully reproducible
and safe to run in parallel.

### Performance benchmarks

Permutation benchmarks are tagged `[.benchmark]` and skipped by CTest. Run them explicitly:

```bash
./build/tests/test_permutation '[.benchmark]' --benchmark-no-analysis
```

---

## TypeScript Type Check

```bash
npm run build --prefix frontend
```

This runs `tsc --noEmit` (strict type check) followed by `vite build` (bundle to
`public/index.html`). A clean exit means zero type errors and a valid build output.

---

## Smoke Test (local server)

```bash
./update.sh

./build/bin/puzzpool &
SERVER_PID=$!
sleep 1
curl -sf http://127.0.0.1:8888/api/v1/stats | python3 -m json.tool
kill $SERVER_PID 2>/dev/null
rm -f pool.db
echo '[OK] smoke test passed'
```

---

## Manual API Tests

**Scenario 1 — Normal worker lifecycle**
```bash
BASE_URL=http://127.0.0.1:8888

# 1. Request work
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","hashrate":1000000}' | python3 -m json.tool

# 2. Send heartbeat (keep job alive)
curl -s -X POST $BASE_URL/api/v1/heartbeat \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","job_id":1}' | python3 -m json.tool

# 3. Submit completion (keys_scanned required)
curl -s -X POST $BASE_URL/api/v1/submit \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","job_id":1,"status":"done","keys_scanned":500000000}' | python3 -m json.tool

# 4. Verify stats updated
curl -s $BASE_URL/api/v1/stats | python3 -c "
import sys, json; d = json.load(sys.stdin)
print('completed_chunks:', d['completed_chunks'])
print('total_keys_completed:', d['total_keys_completed'])
"
```

**Scenario 2 — Test chunk verification**
```bash
# 1. Admin: set a test chunk with known keys
curl -s -X POST $BASE_URL/api/v1/admin/set-test-chunk \
  -H 'Content-Type: application/json' \
  -d '{"start_hex":"0x5fffffffffff000000","end_hex":"0x5fffffffffff100000"}' | python3 -m json.tool

# 2. Worker requests work — should receive the test chunk
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker2","hashrate":500000}' | python3 -m json.tool
# Verify start_key matches test chunk start

# 3. Second worker should NOT receive the test chunk
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker3","hashrate":500000}' | python3 -m json.tool
```

**Scenario 3 — Key found**
```bash
curl -s -X POST $BASE_URL/api/v1/submit \
  -H 'Content-Type: application/json' \
  -d '{
    "name":"testworker",
    "job_id":1,
    "status":"FOUND",
    "findings": [
      {
        "found_key":"0000000000000000000000000000000000000000000000000000000000000042",
        "found_address":"1A1zP1eP5QGefi2DMPTfTL5SLmv7Divf"
      }
    ]
  }' | python3 -m json.tool

# Verify it appears in stats
curl -s $BASE_URL/api/v1/stats | python3 -c "
import sys, json; d = json.load(sys.stdin)
print('finders:', d['finders'])
"
```

**Scenario 4 — Admin token**
```bash
# Without token (should fail if ADMIN_TOKEN is set)
curl -s -X GET $BASE_URL/api/v1/admin/puzzles
# → {"error":"unauthorized"}

# With correct token
curl -s -X GET $BASE_URL/api/v1/admin/puzzles \
  -H "X-Admin-Token: $ADMIN_TOKEN" | python3 -m json.tool
```

---

## Database Inspection

```bash
sqlite3 pool.db

SELECT status, COUNT(*) FROM chunks GROUP BY status;
SELECT * FROM findings;
SELECT * FROM workers ORDER BY last_seen DESC;
```
