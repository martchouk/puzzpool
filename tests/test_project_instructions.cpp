// tests/test_project_instructions.cpp
//
// Contract test for agent-instructions/PROJECT_INSTRUCTIONS.md.
//
// The canonical project instructions are an executable contract: they decide
// which stage a role may act in, what it may mutate, which evidence it owns,
// and how a report is published. A later edit that reflows or rewrites the
// document must not silently drop one of those boundaries, so each is pinned
// here by the phrase that carries it.
//
// Matching runs against a whitespace-normalized copy of the document, so
// re-wrapping a paragraph does not break a pin, while deleting the clause does.
// Every pin is proved sensitive: the same checker is re-run against a copy with
// that one clause removed, and must report exactly that clause as missing.
//
// A pin only protects the words it quotes. An earlier revision pinned a
// multi-part obligation by one phrase from its opening sentence, so deleting
// the sentence that carried the actual requirement left the pin — and the
// test — intact. Each part of an obligation therefore gets its own pin, and
// `hardeningMutations()` below replays the edits that previously went
// undetected, so that regression cannot return.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef PUZZPOOL_PROJECT_INSTRUCTIONS_PATH
#error "PUZZPOOL_PROJECT_INSTRUCTIONS_PATH must be defined by the build"
#endif

namespace {

struct Clause {
    std::string id;
    std::string text;
};

// Boundaries that must be present. Each `text` must appear exactly once, and
// no `text` may be a substring of another, so removing one is detectable on
// its own.
const std::vector<Clause>& requiredClauses()
{
    static const std::vector<Clause> clauses{
        // Stage authority.
        {"stage-authority-source", "The WorkPackage's current status and role"},
        {"stage-authority", "authorize only the current stage"},
        {"prose-never-widens-authority", "never widens that authority to a later stage"},
        {"transition-requires-valid-entry", "an exact `valid_transitions` entry"},
        {"transition-requires-authoritative-mechanism",
         "the run's authoritative transition mechanism"},
        {"tooling-is-not-authority",
         "mutation tooling is never authority to perform a later stage's work by hand"},
        {"single-final-issue-comment", "The single final issue comment"},
        {"transition-report-truthful", "applied versus recommended transitions"},

        // Read-only assessment stages.
        {"implementation-edit-window", "edit the working tree only in implementation stages"},
        {"assessment-role-list",
         "The Architect, UI Designer, Reviewer, Tester, and Product Owner assessment"},
        {"assessment-read-only", "read-only against the assigned 40-hex candidate revision"},
        {"assessment-no-candidate-mutation", "never edit, commit, or push the candidate"},
        {"probe-must-be-restored", "must be restored before the stage ends"},
        {"probe-clean-status", "verified by a clean `git status`"},
        {"head-movement-invalidates-evidence", "invalidates prior evidence"},

        // Candidate publication and artifacts. The gate's prerequisites are
        // pinned one by one: the sentence that announces the gate is not
        // evidence that the conditions guarding it survived.
        {"publication-gate-first-push", "The first push"},
        {"publication-gate-first-pull-request", "the first pull request"},
        {"publication-gate", "including a draft, are a publication gate rather than a checkpoint"},
        {"publication-gate-work-committed",
         "until the planned code, tests, coverage, and documentation are committed"},
        {"publication-gate-clean-tree", "the working tree is clean"},
        {"publication-gate-exact-head-evidence",
         "the exact-head verification record is complete"},
        {"checkout-free-publisher", "publish the report with exactly that adapter and no other route"},
        {"publisher-refusal-reported", "no artifact ref was published and why"},
        // Each forbidden answer to a refusal is pinned separately.
        {"publisher-refusal-no-checkout", "checking out a report branch"},
        {"publisher-refusal-no-candidate-mutation", "mutating the candidate"},
        {"publisher-refusal-no-direct-git", "falling back to direct Git"},
        {"publisher-refusal-no-invented-artifact",
         "citing an artifact reference that was never published"},
        {"draft-path-declared", "only to a writable role-private path that the WorkPackage declares"},
        {"no-alternative-draft-locations", "do not probe alternative locations"},
        {"no-workspace-root-fallback", "do not fall back to a file at the workspace root"},

        // Single-owner exact-head evidence.
        {"developer-owns-evidence", "owns one complete exact-head verification record"},
        {"evidence-covers-backend-and-frontend",
         "covering the backend and frontend commands above"},
        {"evidence-reports-40-hex-head", "reported together with the 40-hex head"},
        {"successors-reuse-evidence", "run independent risk-directed checks and reuse that record"},
        {"repeat-suite-missing-evidence",
         "Repeat the complete suite only when the evidence is missing or unverifiable"},
        {"repeat-suite-head-moved", "the head moved"},
        {"repeat-suite-integration-risk", "broad integration risk requires it"},
        {"repeat-suite-diff-conflict", "the evidence conflicts with the diff"},
        {"no-overlapping-suites", "Never run overlapping complete suites on the shared host"},

        // Headless execution. The detachment ban and the durable-capture route
        // are obligations in their own right, not decoration on the
        // background-execution ban.
        {"no-background-execution", "never request provider background execution"},
        {"no-shell-detachment", "never detach a command with a shell or terminal tool"},
        {"no-replacement-retry", "never launch a replacement retry"},
        {"foreground-durable-capture", "Use an authorized foreground durable-capture route"},
        {"serial-bounded-commands",
         "run bounded, named commands serially at one immutable head"},
        {"timeout-requires-inspection", "inspect the original process, its log, and its receipt"},

        // Executable verification route.
        {"node-age-ctest-route",
         "ctest --test-dir build -R \"^test_check_node_version_age$\" --output-on-failure"},
    };
    return clauses;
}

// Commands and routes that must not reappear once superseded.
const std::vector<Clause>& forbiddenClauses()
{
    static const std::vector<Clause> clauses{
        {"legacy-node-age-shell-command", "bash tests/test_check_node_version_age.sh"},
    };
    return clauses;
}

// One edit a reviewer could make to weaken the document, and the pins that must
// report it. `removed` is deleted from the document verbatim; the result must
// name exactly `detectedBy` as missing.
struct Mutation {
    std::string id;
    std::string removed;
    std::vector<std::string> detectedBy;
    bool includeInCombinedProof = true;
};

// Semantic weakenings that earlier revisions of this test accepted in silence.
// Each is checked from the canonical baseline. Some intentionally overlap so
// that deleting either subject and deleting both subjects are separate probes;
// one non-overlapping representative per obligation group is also replayed in
// the combined proof below.
const std::vector<Mutation>& hardeningMutations()
{
    static const std::vector<Mutation> mutations{
        {"stage-authority-without-current-status",
         "current status and ",
         {"stage-authority-source"},
         false},
        {"stage-authority-without-role", "and role ", {"stage-authority-source"}, false},
        {"stage-authority-without-status-or-role",
         "current status and role ",
         {"stage-authority-source"}},

        {"issue-comment-without-single", "single ", {"single-final-issue-comment"}, false},
        {"issue-comment-without-final", "final ", {"single-final-issue-comment"}, false},
        {"issue-comment-without-single-or-final",
         "single final ",
         {"single-final-issue-comment"}},

        {"assessment-without-architect", "Architect, ", {"assessment-role-list"}, false},
        {"assessment-without-ui-designer", "UI Designer, ", {"assessment-role-list"}, false},
        {"assessment-without-reviewer", "Reviewer, ", {"assessment-role-list"}, false},
        {"assessment-without-tester", "Tester, ", {"assessment-role-list"}, false},
        {"assessment-without-product-owner", "and Product Owner ", {"assessment-role-list"}},
        {"probe-without-clean-status-evidence",
         "verified by a clean `git status`",
         {"probe-clean-status"}},

        {"publication-gate-without-first-push",
         "The first push and ",
         {"publication-gate-first-push"},
         false},
        {"publication-gate-without-first-pull-request",
         "the first pull request, ",
         {"publication-gate-first-pull-request"},
         false},
        {"publication-gate-without-push-or-pull-request",
         "The first push and the first pull request, ",
         {"publication-gate-first-push", "publication-gate-first-pull-request"}},
        {"publication-gate-prerequisites",
         "Do not create either until the planned code, tests, coverage, and documentation "
         "are committed, the working tree is clean, and the exact-head verification record "
         "is complete.",
         {"publication-gate-work-committed",
          "publication-gate-clean-tree",
          "publication-gate-exact-head-evidence"}},

        {"draft-refusal-probes-alternative-locations",
         "do not probe alternative locations",
         {"no-alternative-draft-locations"}},

        {"evidence-without-backend-coverage",
         "the backend and ",
         {"evidence-covers-backend-and-frontend"},
         false},
        {"evidence-without-frontend-coverage",
         "and frontend commands above",
         {"evidence-covers-backend-and-frontend"},
         false},
        {"evidence-without-backend-or-frontend-coverage",
         "covering the backend and frontend commands above",
         {"evidence-covers-backend-and-frontend"}},
        {"evidence-without-reported-head",
         "reported together with the 40-hex head",
         {"evidence-reports-40-hex-head"}},

        {"repeat-suite-without-missing-evidence-condition",
         "only when the evidence is missing or unverifiable, ",
         {"repeat-suite-missing-evidence"},
         false},
        {"repeat-suite-without-head-movement-condition",
         "the head moved, ",
         {"repeat-suite-head-moved"},
         false},
        {"repeat-suite-without-integration-risk-condition",
         "broad integration risk requires it, ",
         {"repeat-suite-integration-risk"},
         false},
        {"repeat-suite-without-diff-conflict-condition",
         "or the evidence conflicts with the diff",
         {"repeat-suite-diff-conflict"},
         false},
        {"repeat-suite-without-any-repeat-condition",
         "only when the evidence is missing or unverifiable, the head moved, broad integration "
         "risk requires it, or the evidence conflicts with the diff",
         {"repeat-suite-missing-evidence",
          "repeat-suite-head-moved",
          "repeat-suite-integration-risk",
          "repeat-suite-diff-conflict"}},

        {"shell-detachment-ban",
         "never detach a command with a shell or terminal tool, and ",
         {"no-shell-detachment"}},

        {"publisher-refusal-forbidden-routes",
         ", mutating the candidate, falling back to direct Git, or citing an artifact "
         "reference that was never published",
         {"publisher-refusal-no-candidate-mutation",
          "publisher-refusal-no-direct-git",
          "publisher-refusal-no-invented-artifact"}},

        {"foreground-durable-capture",
         "Use an authorized foreground durable-capture route. Where none exists, run "
         "bounded, named commands serially at one immutable head.",
         {"foreground-durable-capture", "serial-bounded-commands"}},
    };
    return mutations;
}

std::string readInstructions()
{
    const std::string path{PUZZPOOL_PROJECT_INSTRUCTIONS_PATH};
    std::ifstream in{path};
    REQUIRE(in.is_open());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string contents = buffer.str();
    REQUIRE_FALSE(contents.empty());
    return contents;
}

// Collapses every run of whitespace to a single space so a pinned phrase keeps
// matching after the document is re-wrapped.
std::string normalize(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

std::vector<std::string> missingClauseIds(const std::string& normalizedDoc)
{
    std::vector<std::string> missing;
    for (const Clause& clause : requiredClauses()) {
        if (normalizedDoc.find(normalize(clause.text)) == std::string::npos) {
            missing.push_back(clause.id);
        }
    }
    return missing;
}

std::vector<std::string> presentForbiddenIds(const std::string& normalizedDoc)
{
    std::vector<std::string> present;
    for (const Clause& clause : forbiddenClauses()) {
        if (normalizedDoc.find(normalize(clause.text)) != std::string::npos) {
            present.push_back(clause.id);
        }
    }
    return present;
}

std::string eraseFirst(const std::string& text, const std::string& needle)
{
    const std::size_t pos = text.find(needle);
    REQUIRE(pos != std::string::npos);
    std::string out = text;
    out.erase(pos, needle.size());
    return out;
}

std::vector<std::string> sortedCopy(std::vector<std::string> values)
{
    std::sort(values.begin(), values.end());
    return values;
}

bool isKnownClauseId(const std::string& id)
{
    const std::vector<Clause>& clauses = requiredClauses();
    return std::any_of(clauses.begin(), clauses.end(), [&id](const Clause& clause) {
        return clause.id == id;
    });
}

std::string join(const std::vector<std::string>& values)
{
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += value;
    }
    return out;
}

} // namespace

TEST_CASE("project instructions state every pinned workflow boundary", "[instructions]")
{
    const std::string doc = normalize(readInstructions());

    const std::vector<std::string> missing = missingClauseIds(doc);
    INFO("missing clauses: " << join(missing));
    CHECK(missing.empty());
}

TEST_CASE("project instructions drop the superseded direct node-age command", "[instructions]")
{
    const std::string doc = normalize(readInstructions());

    const std::vector<std::string> present = presentForbiddenIds(doc);
    INFO("superseded clauses still present: " << join(present));
    CHECK(present.empty());
}

TEST_CASE("each pinned clause appears exactly once", "[instructions]")
{
    const std::string doc = normalize(readInstructions());

    for (const Clause& clause : requiredClauses()) {
        INFO("clause: " << clause.id);
        // A duplicated phrase would make the removal probe below pass while a
        // copy of the clause survived, so uniqueness is part of the contract.
        CHECK(countOccurrences(doc, normalize(clause.text)) == 1);
    }
}

TEST_CASE("no pinned clause is contained in another", "[instructions]")
{
    const std::vector<Clause>& clauses = requiredClauses();
    for (const Clause& outer : clauses) {
        for (const Clause& inner : clauses) {
            if (outer.id == inner.id) {
                continue;
            }
            INFO("clause '" << inner.id << "' inside clause '" << outer.id << "'");
            CHECK(normalize(outer.text).find(normalize(inner.text)) == std::string::npos);
        }
    }
}

// Sensitivity proof for the two assertions above: without it, a checker that
// silently matched nothing would still report a passing document.
TEST_CASE("removing any pinned clause fails precisely that clause", "[instructions]")
{
    const std::string doc = normalize(readInstructions());
    REQUIRE(missingClauseIds(doc).empty());

    for (const Clause& clause : requiredClauses()) {
        INFO("clause: " << clause.id);
        const std::string mutated = eraseFirst(doc, normalize(clause.text));
        const std::vector<std::string> missing = missingClauseIds(mutated);
        INFO("missing after removal: " << join(missing));
        CHECK(missing == std::vector<std::string>{clause.id});
    }
}

// Regression for obligations previous revisions let a reviewer delete without
// failing. Every mutation starts from the canonical document, including the
// deliberately overlapping single-subject and grouped-subject probes.
TEST_CASE("deleting one hardening obligation fails exactly its pins", "[instructions]")
{
    const std::string doc = normalize(readInstructions());
    REQUIRE(missingClauseIds(doc).empty());

    for (const Mutation& mutation : hardeningMutations()) {
        INFO("mutation: " << mutation.id);
        REQUIRE_FALSE(mutation.detectedBy.empty());
        for (const std::string& id : mutation.detectedBy) {
            INFO("clause: " << id);
            REQUIRE(isKnownClauseId(id));
        }

        const std::string mutated = eraseFirst(doc, normalize(mutation.removed));
        const std::vector<std::string> missing = missingClauseIds(mutated);
        INFO("missing after removal: " << join(missing));
        CHECK(sortedCopy(missing) == sortedCopy(mutation.detectedBy));
    }
}

// A compatible representative from every obligation group is also removed at
// once. Overlapping alternative probes are skipped because their representative
// grouped deletion already protects the same semantic carriers.
TEST_CASE("deleting all hardening obligations together is detected", "[instructions]")
{
    std::string mutated = normalize(readInstructions());
    REQUIRE(missingClauseIds(mutated).empty());

    std::vector<std::string> expected;
    for (const Mutation& mutation : hardeningMutations()) {
        if (!mutation.includeInCombinedProof) {
            continue;
        }
        mutated = eraseFirst(mutated, normalize(mutation.removed));
        expected.insert(expected.end(), mutation.detectedBy.begin(), mutation.detectedBy.end());
    }

    const std::vector<std::string> missing = missingClauseIds(mutated);
    INFO("missing after removing every hardening obligation: " << join(missing));
    CHECK(sortedCopy(missing) == sortedCopy(expected));
}

TEST_CASE("reintroducing a superseded command is detected", "[instructions]")
{
    const std::string doc = normalize(readInstructions());
    REQUIRE(presentForbiddenIds(doc).empty());

    for (const Clause& clause : forbiddenClauses()) {
        INFO("clause: " << clause.id);
        const std::string mutated = doc + " Also run: " + clause.text;
        const std::vector<std::string> present = presentForbiddenIds(mutated);
        CHECK(present == std::vector<std::string>{clause.id});
    }
}
