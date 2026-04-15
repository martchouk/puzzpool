# Testing

## Automated Tests (Jest)

```bash
npm test
```

Runs `test/server.test.js` against an in-memory SQLite database (`:memory:`).
No network or file I/O — safe to run in CI.

### Test coverage

| Test | What is verified |
|------|-----------------|
| POST /work — new worker | Returns `job_id`, `start_key`, `end_key`; inserts chunk row |
| POST /work — known worker | Uses stored hashrate; chunk size proportional to hashrate |
| POST /work — no active puzzle | Returns 503 |
| POST /work — reclaimed chunk priority | Reclaimed chunk offered before new random chunk |
| POST /work — test chunk priority | Test chunk offered first; not offered twice |
| POST /submit done | Chunk status → `completed` |
| POST /submit FOUND | Chunk status → `FOUND`; row inserted in `findings` |
| POST /submit wrong worker | Update is no-op (ownership check) |
| POST /heartbeat | `assigned_at` reset; worker `last_seen` updated |
| POST /heartbeat missing fields | Returns 400 |
| GET /stats | Returns expected shape; `total_keys_completed` matches submitted chunks |
| GET /stats no puzzle | Returns `puzzle: null` |
| POST /admin/set-puzzle | Creates puzzle; sets active=1; deactivates others |
| POST /admin/set-puzzle invalid hex | Returns 400 |
| POST /admin/set-test-chunk | Sets test_start_hex/test_end_hex on active puzzle |
| POST /admin/set-test-chunk clear | Clears test chunk |
| ADMIN_TOKEN — missing header | Returns 401 |
| ADMIN_TOKEN — correct header | Returns 200 |

---

## Integration Tests (test.sh)

```bash
bash test.sh
```

Hits the **live deployment** at `https://puzzle.b58.de` by default.
To run against a local server, edit `BASE_URL` at the top of `test.sh`:

```bash
BASE_URL=http://127.0.0.1:8888
```

### Manual test scenarios

**Scenario 1 — Normal worker lifecycle**
```bash
# 1. Request work
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","hashrate":1000000}' | jq .

# 2. Send heartbeat (keep job alive)
curl -s -X POST $BASE_URL/api/v1/heartbeat \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","job_id":1}' | jq .

# 3. Submit completion
curl -s -X POST $BASE_URL/api/v1/submit \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker","job_id":1,"status":"done"}' | jq .

# 4. Verify stats updated
curl -s $BASE_URL/api/v1/stats | jq '.completed_chunks, .total_keys_completed'
```

**Scenario 2 — Test chunk verification**
```bash
# 1. Admin: set a test chunk with known keys
curl -s -X POST $BASE_URL/api/v1/admin/set-test-chunk \
  -H 'Content-Type: application/json' \
  -d '{"start_hex":"0x5fffffffffff000000","end_hex":"0x5fffffffffff100000"}' | jq .

# 2. Worker requests work — should receive the test chunk
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker2","hashrate":500000}' | jq .
# Verify start_key matches test chunk start

# 3. Second worker should NOT receive the test chunk
curl -s -X POST $BASE_URL/api/v1/work \
  -H 'Content-Type: application/json' \
  -d '{"name":"testworker3","hashrate":500000}' | jq .
# Verify start_key is different
```

**Scenario 3 — Key found**
```bash
curl -s -X POST $BASE_URL/api/v1/submit \
  -H 'Content-Type: application/json' \
  -d '{
    "name":"testworker",
    "job_id":1,
    "status":"FOUND",
    "found_key":"0000000000000000000000000000000000000000000000000000000000000042",
    "found_address":"1A1zP1eP5QGefi2DMPTfTL5SLmv7Divf"
  }' | jq .

# Verify it appears in stats
curl -s $BASE_URL/api/v1/stats | jq '.finders'
```

**Scenario 4 — Admin token**
```bash
# Without token (should fail if ADMIN_TOKEN is set)
curl -s -X GET $BASE_URL/api/v1/admin/puzzles
# → {"error":"unauthorized"}

# With correct token
curl -s -X GET $BASE_URL/api/v1/admin/puzzles \
  -H "X-Admin-Token: $ADMIN_TOKEN"
# → {"puzzles":[...]}
```

---

## Database Inspection

```bash
sqlite3 pool.db

SELECT status, COUNT(*) FROM chunks GROUP BY status;
SELECT * FROM findings;
SELECT * FROM workers ORDER BY last_seen DESC;
```
