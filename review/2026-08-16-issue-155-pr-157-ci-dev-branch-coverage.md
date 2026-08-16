# Review — issue #155 / PR #157: run CI for dev-targeted changes

## Subject

| Field | Value |
|---|---|
| Issue | [#155](https://github.com/martchouk/puzzpool/issues/155) — bug: CI does not run for dev-targeted pull requests |
| Pull request | [#157](https://github.com/martchouk/puzzpool/pull/157) — `fix: run CI for dev-targeted changes` |
| Branch | `bugfix/155-ci-dev-coverage` → `dev` |
| Base revision | `d2f5013f1d99517dd61ac6099e57f4f2b26ad2e2` |
| Candidate (head) revision | `ec8beaaadbc5599ddadf0989a06f8a160697a6ff` |
| Merge parents | `789f7e56b457c4c374db017de764e982cb4b901b`, `d2f5013f1d99517dd61ac6099e57f4f2b26ad2e2` |
| PR state | open, not draft, `MERGEABLE` / `CLEAN` |
| Companion PRs | none |
| Pinned project policy | `puzzpool@946634c96908d1a4f8ea5df591a67e8c558a5e75`, profile `standard` |
| Reviewer | mud-rev (reviewer role) |

Candidate immutability: the PR head was re-read immediately before publication and
still equals `ec8beaaadbc5599ddadf0989a06f8a160697a6ff`. No drift; no escalation needed.

## Verdict

**APPROVE** — no blocking findings.

The trigger defect in issue #155 is fixed correctly and minimally, the guard added
against its recurrence is genuinely mutation-sensitive rather than decorative, the
existing jobs are untouched, and documentation is updated on the surfaces the pinned
policy assigns. One non-blocking `SHOULD FIX` concerns where the new guard is wired
in, not whether it works.

## Scope reconstruction

Diff against the base is 5 files, 134 insertions, 2 deletions. The two-dot diff
(`d2f5013..ec8beaa`) is byte-identical in file set and counts to the three-dot diff,
and the merge-base equals the base revision, so the integration merge hides no
additional change.

| File | Nature |
|---|---|
| `.github/workflows/ci.yml` | production (trigger fix + additive `workflow-contract` job) |
| `scripts/check-ci-branch-coverage.sh` | new checker (executable, `100755`) |
| `tests/test_ci_branch_coverage.sh` | new test (executable, `100755`) |
| `README.md` | documentation |
| `docs/testing.md` | documentation |

## Acceptance criteria

| Criterion | Result | Evidence |
|---|---|---|
| Pushes to `main` and `dev` trigger CI | met | `push.branches: [main, dev]` |
| PRs targeting `main` and `dev` trigger CI | met | `pull_request.branches: [main, dev]` |
| Existing backend and frontend jobs unchanged and valid | met | diff touches no line of `build` or `frontend-typecheck`; all three hosted jobs green at the exact candidate |
| Focused regression check prevents removing `dev` coverage | met, with a placement caveat | independently verified mutation-sensitive; see F1 |

## Developer evidence matrix

The matrix in the implementation report is accurate. Every claim I could
independently re-run reproduced, including the exact test counts (`ctest` 16/16,
frontend 40/40, node-age 9/9). The reported hosted run
`31935386571` is bound to `head_sha=ec8beaaadbc5599ddadf0989a06f8a160697a6ff`,
`event=pull_request`, `conclusion=success` — it is a genuine exact-head run, not a
stale or base-branch run.

The report's disclosed limitation (local shell allowlist blocked the inline
`timeout 3 ./build/bin/puzzpool` smoke command) is honestly stated and is covered by
the CTest `test_admin_routes_smoke` plus the hosted `build` job, both of which I
observed passing.

## Findings

### MUST FIX

None.

### SHOULD FIX

**F1 — the new guard is not registered in CTest, so the documented local full-suite
command cannot observe the regression it guards against.**

`tests/test_ci_branch_coverage.sh` is reachable only through the hosted
`workflow-contract` job and a manual `bash tests/test_ci_branch_coverage.sh`. It is
not registered in `tests/CMakeLists.txt`.

This matters for two reasons.

First, it diverges from an explicit, documented convention already established in this
repository. `tests/CMakeLists.txt:26-31` registers the sibling shell test with the
comment: *"It is registered here so `ctest` runs it as part of the ordinary suite
instead of requiring a separate shell invocation."* The new test is the same kind of
dependency-free shell guard and has no property that makes it unsuitable.

Second, the practical consequence is verified, not theoretical. I removed `dev` from
both triggers in a built worktree and re-ran the pre-handoff command the pinned policy
names:

```
$ sed -i '' 's/branches: \[main, dev\]/branches: [main]/g' .github/workflows/ci.yml
$ ctest --test-dir build
100% tests passed, 0 tests failed out of 16
```

The full local suite reports green while the exact defect of issue #155 is present.

There is also a structural point worth recording: the only automatic enforcement of
this contract is a job *inside the very workflow whose triggers it validates*. A guard
whose execution is gated by the condition it asserts can be disabled by the same edit
it exists to catch. Registering the test in CTest adds an enforcement point that does
not depend on the workflow's own trigger evaluation.

*Remedy:* register `tests/test_ci_branch_coverage.sh` in `tests/CMakeLists.txt`
alongside `test_check_node_version_age`, guarded by the existing
`PUZZPOOL_BASH_EXECUTABLE` check and with `WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"`.

*Why this is not blocking:* the acceptance criteria are literally satisfied — a focused
regression check exists, it is mutation-sensitive, and it is documented with its exact
command in both `README.md` and `docs/testing.md`. The delivered change strictly
improves CI coverage over the status quo and introduces no defect or regression.
F1 is hardening of a guard that does work, and is well suited to a follow-up issue.

### Optional

**F2 — the checker rejects two standard YAML spellings of a semantically correct
trigger list.** `branches: ["main", "dev"]` and the idiomatic block-sequence form

```yaml
branches:
  - main
  - dev
```

are both rejected. This is fail-*closed*, which is the correct direction — a false
positive is a loud red check that gets fixed, whereas a false negative would silently
restore issue #155 — and the block-sequence rejection names its own requirement
(`must declare an inline branches list`). The quoted-name case is weaker: it reports
`does not cover required branch 'main'`, which misdirects a maintainer toward the
branch list rather than the quoting. Stripping `"` and `'` during parsing, or naming
quoting in the message, would remove the sharp edge. No action required for this PR.

**F3 — section placement.** `## CI Branch Coverage Contract` was inserted as the first
section of `docs/testing.md`, ahead of the C++ unit tests. Defensible for a fast
check; purely editorial.

## Test assessment

### Full relevant suite — independently re-run at the candidate revision

Run in a detached worktree at `ec8beaaadbc5599ddadf0989a06f8a160697a6ff` with
submodules initialized recursively.

| Command | Result |
|---|---|
| `bash tests/test_ci_branch_coverage.sh` | pass — baseline + 6 rejection probes |
| `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPUZZPOOL_BUILD_TESTS=ON` | pass |
| `cmake --build build --parallel 4` | pass |
| `ctest --test-dir build --output-on-failure` | pass — 16/16, 12.17 s |
| `bash tests/test_check_node_version_age.sh` | pass — 9/9 |
| `npm ci --prefix frontend` | pass |
| `npm test --prefix frontend` | pass — 3 files, 40/40 |
| `npm run build --prefix frontend` | pass |

Hosted checks at the exact candidate: `workflow-contract` pass (4 s),
`frontend-typecheck` pass (15 s), `build` pass (3 m 25 s).

### Changed test files reviewed

**`tests/test_ci_branch_coverage.sh`** — reviewed in full.

The mutation harness is correctly event-scoped: `mutate_event_branches` only clears
`in_event` on the targeted event's `branches:` line, so mutating `push` provably leaves
`pull_request` intact and vice versa. `expect_failure` asserts both that the checker
rejects *and* that it rejects for the expected reason, matched against the specific
error string — this is what stops a mutation that silently fails to apply from being
scored as a pass. Temporary files are isolated under `mktemp -d` with a cleanup trap;
the test writes nothing into the repository and makes no network calls. It runs
`set -euo pipefail`.

### Test-sensitivity assessment (independent anti-vacuity probes)

I did not rely on the developer's probes. I ran my own:

*Probe 1 — neutered checker.* Replacing `scripts/check-ci-branch-coverage.sh` with
`exit 0` must not leave the suite green:

```
$ printf '#!/usr/bin/env bash\nexit 0\n' > scripts/check-ci-branch-coverage.sh
$ bash tests/test_ci_branch_coverage.sh
FAIL: checker accepted a missing workflow    → exit 1
```

*Probe 2 — the actual issue-#155 regression.* Removing `dev` from both triggers:

```
$ bash tests/test_ci_branch_coverage.sh
ERROR: push trigger does not cover required branch 'dev'    → exit 1
```

The test therefore cannot pass vacuously, and it does fail on the precise defect it
exists to prevent.

*Probe 3 — checker behaviour on seven workflow variants the developer did not test:*

| Variant | Exit | Behaviour |
|---|---|---|
| `branches: [main, development]` (near-miss substring) | 1 | correctly rejects — matching is exact, not substring |
| `pull_request` with no branches list | 1 | rejects (only the `push` equivalent was tested upstream) |
| entire `on:` block replaced by `workflow_dispatch` | 1 | rejects |
| `pull_request: branches: [dev]` only | 1 | rejects, names `main` |
| `branches: [ main, dev ]` (inner spaces) | 0 | correctly accepts |
| `branches: ["main", "dev"]` | 1 | false positive — see F2 |
| block-sequence `branches:` list | 1 | false positive — see F2 |

Every failure mode observed is fail-closed. I found no input that makes the checker
accept a workflow which does not actually run CI for `dev`.

### Covered and uncovered paths

Covered: both events × both required branches; missing workflow file; missing inline
branches list; exact-match branch comparison.

Uncovered, all acceptable: `on: [push, pull_request]` array shorthand (rejected though
semantically sufficient — fail-closed); `branches-ignore` (GitHub rejects it alongside
`branches` for the same event, so it is unreachable); a `  push:` key appearing under
`jobs:` before `on:` (not constructible in a valid workflow, since `on:` precedes
`jobs:`).

## Documentation

| Surface | Owner per pinned policy | Status |
|---|---|---|
| `docs/testing.md` | test commands, fixtures, verification strategy | updated — documents both events, both branches, the focused command, and that the test mutates each event independently |
| `README.md` | setup / branch model | updated — branch-model table now states the CI trigger contract and names the guard command |
| `docs/api.md`, `docs/database.md`, `docs/architecture.md`, `docs/security.md`, `.env.example` | — | correctly untouched; no route, schema, dependency-direction, trust-boundary, or configuration change |

Puzzpool maintains no changelog and none was invented. Documentation status: complete
for every relevant surface.

## Security assessment

No security-relevant regression.

- The change adds no secret, no logging, and no new dependency. The new job's only
  action is `actions/checkout@v4`, matching the pinning style already used by both
  existing jobs.
- The trigger uses `pull_request`, **not** `pull_request_target`. This is the important
  property: adding `dev` therefore does not expose repository secrets to code from
  untrusted forks, and does not run fork code in a privileged context.
- The Node.js minimum-release-age guard is untouched; `MINIMUM_NODE_RELEASE_AGE: 1440`
  remains set in `frontend-typecheck`, and the guard's own 9-case unit suite passes.
  No `MINIMUM_NODE_RELEASE_AGE=0` was introduced.
- No `.env`, `ADMIN_TOKEN`, OAuth credential, session key, database, WAL/SHM file, or
  `BINGO_FOUND_KEYS.txt` appears in the diff. `public/index.html` is generated during
  the frontend build and remains ignored via `.gitignore:11` — I confirmed the worktree
  stays clean after `npm run build`, so no generated output is committed.
- The checker reads a repository-tracked file and constructs no command from external
  input.

The new job inherits the workflow's default `GITHUB_TOKEN` permissions rather than
declaring `permissions: contents: read`. That is consistent with both pre-existing jobs
and is not a regression introduced here; tightening it repository-wide belongs to a
separate hardening change, not this fix.

## Persistence and performance assessment

Not applicable to the delivered behaviour. The change touches no SQLite access, no
`PoolDb::migrate()` path, no `puzzle_id`/`worker_name` scoping, no chunk lifecycle or
allocator code, and no keyspace arithmetic. No 256-bit value, half-open range,
hexadecimal representation, or decimal-string transport field is affected. No frontend
rendering or polling path changes.

Performance impact is confined to CI throughput: `dev` now consumes runner minutes it
previously did not, which is the deliberate intent of issue #155. The new
`workflow-contract` job is dependency-free and completed in 4 s hosted, so it does not
meaningfully extend wall-clock time; it runs in parallel with `build`.

## Architecture and policy compliance

- Branch and target: `bugfix/155-ci-dev-coverage` cut from and targeted at `dev` — matches
  the pinned policy and the `bugfix/<ISSUE>-<description>` naming rule in `GIT_HYGIENE.md`.
- Integration by normal merge commit; no rebase, no force-push. The PR body links the
  issue with `Fixes #155` and explains why.
- Commit `789f7e5` uses Conventional Commits with `(closes #155)`. I inspected both
  commit messages: neither contains AI/model/provider co-author trailers.
- Submodules under `third_party/` are preserved and initialize cleanly (`Catch2`,
  `SQLiteCpp` + its `googletest`, `crow`).
- Scope is one issue; no deployment, generated output, or local database state is mixed in.

Typed project-policy exceptions applied: none. No context/profile mismatch: the
`standard` profile is truthfully instantiable for this task.

## Prior findings

None — this is the first review round for PR #157. No finding IDs to carry, and no
re-review delta classification applies.

## Positive observations

- The guard is written to fail closed on every malformed or unrecognized input I could
  construct. For a check whose whole purpose is preventing a silent loss of CI, that is
  the right bias, and it was clearly deliberate rather than accidental.
- `expect_failure` asserts the *reason* for rejection, not merely a non-zero exit. This
  is what makes the mutation suite meaningful, and it is the detail most commonly
  omitted in tests of this shape.
- The checker is dependency-free — no `yq`, no Python, no new CI action — so the guard
  cannot itself become a supply-chain or availability liability.
- The implementation report's disclosure of the blocked local smoke command, and of the
  out-of-scope `npm audit` findings, is accurate and appropriately scoped rather than
  quietly folded into this PR.

## Required next steps

1. **None blocking.** PR #157 may merge to `dev` as it stands.
2. **Follow-up (F1):** open an issue to register `tests/test_ci_branch_coverage.sh` in
   `tests/CMakeLists.txt` so `ctest` — the command the pinned policy names for
   pre-handoff verification — exercises the contract, giving an enforcement point
   independent of the workflow's own triggers.
3. **Optional (F2):** tolerate quoted branch names, or name quoting in the error text.
4. The `npm audit` findings reported by the developer remain out of scope for #155 and
   need their own issue; they are not a reason to hold this PR.

## Transition and routing

This is a manual run with no server-held attempt state and no authoritative transition
mechanism, so **no workflow status label is applied by this review**. The recommendation
below is advisory to the operator, not an applied transition.

Advisory recommendation: PR #157 is approved and ready to merge into `dev`; issue #155
can close on merge via the `Fixes #155` link.
