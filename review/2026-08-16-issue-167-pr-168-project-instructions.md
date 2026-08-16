# Review report — issue #167 / PR #168

## Identity and immutable review target

- Reviewer: `rita-rev`
- Issue: #167 — Harden project instructions for full-workflow Bridge runs
- Pull request: #168 — `bugfix/167-harden-project-instructions` into `dev`
- PR author: `bud-dev`
- PR state: open, ready for review, mergeable
- Assigned and assessed candidate: `7b83cfcfb4120086e332a7181bb097bb87a84787`
- PR base tip at review: `423ca0cd67b0e3c41d6f40c1b0bd563a48bc7a82`
- PR merge base: `d2f5013f1d99517dd61ac6099e57f4f2b26ad2e2`
- Pinned project policy: `puzzpool@946634c96908d1a4f8ea5df591a67e8c558a5e75`, profile `standard`
- Policy manifest digest: `e73dcdb7e87d669c1fa789d5589b3449b801c1dae9b7e0e6664c4173dcdeae76`
- Project-policy binding digest: `4be834ab7872aa6b64d2a319631955e0996a03d3d202df6e8b25d3b34c630d91`
- Review round: 1; consolidated review: false
- Typed project exceptions applied: none
- Companion PRs: none identified
- Issue linkage: PR body contains `Closes #167`

## Verdict

**REQUEST CHANGES.** The policy text contains the requested first-push/first-PR gate, but its new executable contract still passes after both publication events are deleted from that gate. This violates the acceptance requirement that deleting each critical clause produce a precise failure.

## MUST FIX

### REV-167-001 — Candidate-publication subjects remain removable without a contract failure

Classification: `BASELINE_MISS`  
Severity: blocking  
Why not earlier: this is the first valid independent review of `7b83cfc`; the preceding reviewer run was stopped without a verdict and its correction handoff covered four different removable obligation groups.

The production policy at `agent-instructions/PROJECT_INSTRUCTIONS.md:30` says, “The first push and the first pull request, including a draft, are a publication gate…”. The test at `tests/test_project_instructions.cpp:69` pins only the suffix `including a draft, are a publication gate rather than a checkpoint`. It does not pin either “first push” or “first pull request”.

Independent mutation evidence at the exact candidate:

1. Baseline focused CTest passed 1/1.
2. In a disposable checkout, I deleted exactly `The first push and the first pull request, `, leaving the existing lowercase pinned suffix unchanged.
3. `ctest --test-dir build-review -R "^test_project_instructions$" --output-on-failure` still passed 1/1.
4. The policy file was restored; its SHA-256 returned to `c86f666f9a25867ff74fcef65c1b586ccb6bb22318617b315b2fb45e613d503d`, tracked status was clean, and HEAD remained `7b83cfcfb4120086e332a7181bb097bb87a84787`.

Required remedy:

- Add independent pins for the first push and the first pull request, plus mutation cases that delete each one separately and both together.
- Complete the same atomic requirement-to-pin audit for other issue-mandated carriers that the current phrase fragments do not independently protect, including:
  - WorkPackage current status and role as the authority source (`PROJECT_INSTRUCTIONS.md:10` versus test line 50);
  - the single final issue comment (`PROJECT_INSTRUCTIONS.md:16` versus test line 57);
  - the named assessment roles and clean-status restoration evidence (`PROJECT_INSTRUCTIONS.md:20-24` versus test lines 61-64);
  - the prohibition on probing alternative draft locations (`PROJECT_INSTRUCTIONS.md:43` versus test lines 83-84);
  - backend and frontend coverage tied to the reported 40-hex head (`PROJECT_INSTRUCTIONS.md:241-242` versus test line 87);
  - the complete-suite repeat conditions (`PROJECT_INSTRUCTIONS.md:245-247` versus test line 88).
- Add precise deletion probes for every newly pinned atomic obligation and update `docs/testing.md` if its coverage description changes.

Missing or mutation-insensitive tests for a changed required path are blocking under the reviewer contract.

## SHOULD FIX

None.

## Optional suggestions

None.

## Prior handoff concerns

The stopped reviewer produced no valid prior verdict, so there are no formal prior finding IDs to carry. For traceability, the four operator-handoff remediation groups are resolved in this candidate:

- `OPERATOR-H1`: publication-gate prerequisites — resolved by separate committed-work, clean-tree, and exact-head pins plus the grouped mutation.
- `OPERATOR-H2`: shell/terminal detachment prohibition — resolved by an independent `no-shell-detachment` pin and mutation.
- `OPERATOR-H3`: publisher-refusal protections — resolved by separate no-candidate-mutation, no-direct-Git, and no-invented-artifact pins plus the grouped mutation.
- `OPERATOR-H4`: foreground durable capture and serialized immutable-head commands — resolved by separate pins plus the grouped mutation.

Structured assessment equivalent:

- `previous_findings`: empty, because no valid prior review verdict exists.
- `new_findings`: `REV-167-001`, `BASELINE_MISS`, blocking.

## Diff and documentation review

The complete PR diff from merge base `d2f5013` contains four files, 456 insertions, and 2 deletions:

- `agent-instructions/PROJECT_INSTRUCTIONS.md`: fully reviewed. The requested policy clauses are present and the legacy direct node-age command is replaced.
- `tests/test_project_instructions.cpp`: fully reviewed; this is the only changed test file. Its in-memory single-pin and four-group mutations are observable and non-vacuous, but coverage remains incomplete as REV-167-001 demonstrates.
- `tests/CMakeLists.txt`: fully reviewed. The native Catch2 target is registered and receives the canonical instruction path.
- `docs/testing.md`: fully reviewed. It documents the CTest node-age route and the contract test, but its coverage statement currently overstates atomic publication-gate sensitivity.

No API, database, architecture, security, deployment configuration, or frontend contract changed, so no updates were required to `docs/api.md`, `docs/database.md`, `docs/architecture.md`, `docs/security.md`, `README.md`, or `.env.example`.

## Verification and test sensitivity

Developer-owned exact-head evidence was complete and reusable:

- Release configure/build passed.
- Complete CTest passed 17/17.
- Focused project-instruction CTest passed 1/1; direct binary passed 8 cases / 1202 assertions.
- The four previously missed obligation groups produced 13 intended failures in the Developer mutation record.
- Frontend tests passed 40/40; typecheck and production build passed.
- Hosted `workflow-contract`, `build`, and `frontend-typecheck` checks are all successful and bound to `7b83cfcfb4120086e332a7181bb097bb87a84787`.

Independent risk-directed checks:

- Release configuration for a disposable exact-head checkout passed.
- Focused target `test_project_instructions` built successfully. The shell transport preserved the original build after its foreground reporting cap; I inspected that same process and receipt to completion and launched no replacement build.
- Baseline focused CTest passed 1/1.
- The direct-binary command was denied by the lean-context allowlist and was not retried.
- The publication-subject deletion probe still passed 1/1, which is the blocking regression-sensitivity result.
- The restored file hash and clean tracked state were verified at the unchanged candidate.

Covered paths: presence, uniqueness, substring independence, per-pin deletion, the four prior grouped deletions singly and together, and legacy-command reinjection.

Uncovered paths: deletion of critical semantic carriers outside the selected phrase fragments, including the first-push and first-PR subjects and the examples listed in REV-167-001.

Because the executable contract cannot catch a required publication-gate regression, passing broad suites and hosted checks do not cure the blocker.

## Security, persistence, performance, and compatibility

- Security: no runtime trust boundary, credential handling, authorization, or dependency-supply-chain behavior changed. No secret material was found in the diff.
- Persistence/concurrency: no database, migration, locking, ownership, or retention behavior changed.
- Performance: no production hot path, recurring query, allocator, polling, or rendering behavior changed; query-plan and benchmark evidence are not applicable.
- Compatibility: the authored policy intentionally supersedes the repository’s earlier push-immediately/draft-early guidance and replaces only the managed-workspace node-age invocation. Existing Puzzpool product contracts remain unchanged.

## Positive observations

- The correction commit directly addresses all four operator-handoff groups without changing the canonical policy text.
- The test’s positive controls, uniqueness checks, and forbidden-command reinjection avoid vacuous absence assertions.
- The candidate was not moved during review, and the disposable mutation was restored byte-for-byte.

## Required next steps and routing

1. The Developer should repair REV-167-001 on the existing branch without rewriting history.
2. Because the head will change, gather one new complete Developer-owned exact-head backend/frontend record and the focused atomic mutation results.
3. Update the PR metadata to the new 40-hex candidate.
4. Route the new candidate to a fresh independent reviewer.

Transition applied: none. This manual WorkPackage supplies no authoritative transition mechanism, no `valid_transitions`, and no `next_assignee_roles`; no status label was changed.

Advisory operator recommendation: route to the Developer for remediation, then back to independent code review. This recommendation is not an applied workflow transition.
