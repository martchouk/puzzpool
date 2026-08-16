#include <puzzpool/auth.hpp>
#include <puzzpool/config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace puzzpool;

namespace {

constexpr std::int64_t kNow = 1'800'000'000;

Config signingConfig() {
    Config cfg;
    cfg.sessionSigningSecret = "unit-test-signing-secret-not-a-real-key";
    cfg.adminGithubUsers     = parseAdminGithubUsers("Operator-One, operator-two");
    cfg.sessionTtlMinutes    = 60;
    return cfg;
}

std::string sessionCookieFor(const Config& cfg, const std::string& login, std::int64_t expiresAt) {
    SessionIdentity identity;
    identity.login         = login;
    identity.githubId      = 4242;
    identity.expiresAtUnix = expiresAt;
    return encodeSessionToken(cfg, identity);
}

AdminRequestView getView(const std::string& cookieHeader) {
    AdminRequestView view;
    view.cookieHeader = std::string(kSessionCookieName) + "=" + cookieHeader;
    return view;
}

AdminRequestView postView(const std::string& cookieHeader) {
    AdminRequestView view = getView(cookieHeader);
    view.isPost           = true;
    return view;
}

} // namespace

// ── base64url ─────────────────────────────────────────────────────────────────

TEST_CASE("base64UrlEncode round-trips arbitrary bytes without padding", "[auth][base64]") {
    for (const std::string& sample :
         {std::string(""), std::string("f"), std::string("fo"), std::string("foo"),
          std::string("foob"), std::string("fooba"), std::string("foobar")}) {
        const std::string encoded = base64UrlEncode(sample);
        CHECK(encoded.find('=') == std::string::npos);
        const auto decoded = base64UrlDecode(encoded);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == sample);
    }

    // All 256 byte values survive intact, including embedded NUL and 0xff.
    std::string all;
    for (int i = 0; i < 256; ++i) all += static_cast<char>(i);
    const auto decoded = base64UrlDecode(base64UrlEncode(all));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == all);
}

TEST_CASE("base64UrlEncode uses the URL alphabet, never + or /", "[auth][base64]") {
    std::string all;
    for (int i = 0; i < 256; ++i) all += static_cast<char>(i);
    const std::string encoded = base64UrlEncode(all);
    CHECK(encoded.find('+') == std::string::npos);
    CHECK(encoded.find('/') == std::string::npos);
    CHECK(encoded.find('-') != std::string::npos);
    CHECK(encoded.find('_') != std::string::npos);
}

TEST_CASE("base64UrlDecode rejects malformed input", "[auth][base64]") {
    CHECK_FALSE(base64UrlDecode("a").has_value());       // impossible length
    CHECK_FALSE(base64UrlDecode("ab*d").has_value());    // invalid character
    CHECK_FALSE(base64UrlDecode("AB==").has_value());    // padding is not accepted
    CHECK_FALSE(base64UrlDecode("A+/B").has_value());    // standard-alphabet chars
    CHECK_FALSE(base64UrlDecode("AB").has_value());      // non-zero leftover bits
    CHECK(base64UrlDecode("AA").has_value());            // the canonical encoding of "\0"
}

// ── constant-time comparison (AC11) ──────────────────────────────────────────

TEST_CASE("constantTimeEquals matches std::string equality", "[auth][compare]") {
    CHECK(constantTimeEquals("", ""));
    CHECK(constantTimeEquals("secret", "secret"));
    CHECK_FALSE(constantTimeEquals("secret", "secreT"));
    CHECK_FALSE(constantTimeEquals("secret", "secre"));
    CHECK_FALSE(constantTimeEquals("", "s"));
    // Differs only in the final byte — the loop must not short-circuit earlier.
    CHECK_FALSE(constantTimeEquals(std::string(64, 'a'), std::string(63, 'a') + "b"));
    // Embedded NUL bytes are compared, not treated as terminators.
    CHECK(constantTimeEquals(std::string("a\0b", 3), std::string("a\0b", 3)));
    CHECK_FALSE(constantTimeEquals(std::string("a\0b", 3), std::string("a\0c", 3)));
}

TEST_CASE("randomHexToken returns distinct full-length hex", "[auth][random]") {
    const std::string a = randomHexToken(32);
    const std::string b = randomHexToken(32);
    CHECK(a.size() == 64);
    CHECK(b.size() == 64);
    CHECK(a != b);
    CHECK(std::all_of(a.begin(), a.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }));
}

// ── fail-closed signing (AC13 / D14) ─────────────────────────────────────────

TEST_CASE("sessionSigningEnabled is false for an unset or empty secret", "[auth][failclosed]") {
    Config cfg;
    CHECK_FALSE(sessionSigningEnabled(cfg));
    cfg.sessionSigningSecret = "";
    CHECK_FALSE(sessionSigningEnabled(cfg));
    cfg.sessionSigningSecret = "x";
    CHECK(sessionSigningEnabled(cfg));
}

TEST_CASE("signing without a configured secret throws instead of using a default key",
          "[auth][failclosed]") {
    Config cfg;
    SessionIdentity identity{"operator-one", 1, kNow + 60};
    CHECK_THROWS(encodeSessionToken(cfg, identity));
    CHECK_THROWS(signValue(cfg, kSessionPurpose, R"({"exp":1})"));
}

TEST_CASE("a token minted under a secret is rejected once the secret is removed",
          "[auth][failclosed]") {
    const Config configured = signingConfig();
    const std::string token = sessionCookieFor(configured, "operator-one", kNow + 600);

    Config unconfigured = configured;
    unconfigured.sessionSigningSecret.clear();

    SignatureError error = SignatureError::None;
    CHECK_FALSE(decodeSessionToken(unconfigured, token, kNow, &error).has_value());
    CHECK(error == SignatureError::NotConfigured);
}

// ── session token integrity ──────────────────────────────────────────────────

TEST_CASE("a freshly issued session token decodes to the same identity", "[auth][session]") {
    const Config cfg = signingConfig();
    SessionIdentity issued{"Operator-One", 99, kNow + 3600};
    SignatureError error = SignatureError::None;

    const auto decoded = decodeSessionToken(cfg, encodeSessionToken(cfg, issued), kNow, &error);
    REQUIRE(decoded.has_value());
    CHECK(error == SignatureError::None);
    CHECK(decoded->login == "Operator-One");
    CHECK(decoded->githubId == 99);
    CHECK(decoded->expiresAtUnix == kNow + 3600);
}

TEST_CASE("an expired session token is rejected at its exact expiry", "[auth][session]") {
    const Config cfg   = signingConfig();
    const std::string token = sessionCookieFor(cfg, "operator-one", kNow);

    SignatureError error = SignatureError::None;
    CHECK_FALSE(decodeSessionToken(cfg, token, kNow, &error).has_value());
    CHECK(error == SignatureError::Expired);

    // Sensitivity: the same token one second before expiry does verify, so the
    // rejection above is the expiry check and not an unrelated failure.
    CHECK(decodeSessionToken(cfg, token, kNow - 1, &error).has_value());
    CHECK(error == SignatureError::None);
}

TEST_CASE("a tampered payload invalidates the signature", "[auth][session]") {
    const Config cfg = signingConfig();
    std::string token = sessionCookieFor(cfg, "operator-one", kNow + 600);

    // Re-encode the payload with an elevated login, keeping the original tag.
    const auto firstDot  = token.find('.');
    const auto secondDot = token.find('.', firstDot + 1);
    const std::string forgedPayload =
        base64UrlEncode(R"({"login":"attacker","gid":1,"exp":9999999999})");
    const std::string forged =
        token.substr(0, firstDot + 1) + forgedPayload + token.substr(secondDot);

    SignatureError error = SignatureError::None;
    CHECK_FALSE(decodeSessionToken(cfg, forged, kNow, &error).has_value());
    CHECK(error == SignatureError::BadSignature);
}

TEST_CASE("a tampered signature is rejected", "[auth][session]") {
    const Config cfg  = signingConfig();
    std::string token = sessionCookieFor(cfg, "operator-one", kNow + 600);
    token.back()      = (token.back() == 'A') ? 'B' : 'A';

    SignatureError error = SignatureError::None;
    CHECK_FALSE(decodeSessionToken(cfg, token, kNow, &error).has_value());
    CHECK(error == SignatureError::BadSignature);
}

TEST_CASE("a token signed with a different secret is rejected", "[auth][session]") {
    Config mint  = signingConfig();
    Config other = signingConfig();
    other.sessionSigningSecret = "a-completely-different-signing-secret";

    const std::string token = sessionCookieFor(mint, "operator-one", kNow + 600);

    SignatureError error = SignatureError::None;
    CHECK_FALSE(decodeSessionToken(other, token, kNow, &error).has_value());
    CHECK(error == SignatureError::BadSignature);
}

TEST_CASE("malformed session tokens are rejected without throwing", "[auth][session]") {
    const Config cfg = signingConfig();
    SignatureError error = SignatureError::None;

    for (const std::string& bad : {std::string("not-a-token"),
                                   std::string("v1.only-two-parts"),
                                   std::string("v1.a.b.c"),
                                   std::string("v2.YWJj.YWJj"),
                                   std::string("v1..YWJj"),
                                   std::string("v1.YWJj."),
                                   std::string("v1.!!!.YWJj")}) {
        CHECK_FALSE(decodeSessionToken(cfg, bad, kNow, &error).has_value());
        CHECK((error == SignatureError::Malformed || error == SignatureError::BadSignature));
    }

    CHECK_FALSE(decodeSessionToken(cfg, "", kNow, &error).has_value());
    CHECK(error == SignatureError::Missing);
}

TEST_CASE("an OAuth state token does not verify as a session token", "[auth][session]") {
    const Config cfg = signingConfig();
    const std::string stateToken =
        signValue(cfg, kOauthStatePurpose, R"({"n":"abc","exp":9999999999})");

    SignatureError error = SignatureError::None;
    CHECK_FALSE(decodeSessionToken(cfg, stateToken, kNow, &error).has_value());
    CHECK(error == SignatureError::BadSignature);

    // Sensitivity: the same token does verify under its own purpose.
    CHECK(verifySignedValue(cfg, kOauthStatePurpose, stateToken, kNow, &error).has_value());
}

TEST_CASE("a signed payload without a login is rejected", "[auth][session]") {
    const Config cfg = signingConfig();
    const std::string token = signValue(cfg, kSessionPurpose, R"({"login":"","gid":1,"exp":9999999999})");

    SignatureError error = SignatureError::None;
    CHECK_FALSE(decodeSessionToken(cfg, token, kNow, &error).has_value());
    CHECK(error == SignatureError::Malformed);
}

TEST_CASE("githubAvatarUrl is derived from the numeric id", "[auth][session]") {
    CHECK(githubAvatarUrl(583231) == "https://avatars.githubusercontent.com/u/583231?v=4");
}

// ── allow-list (AC6) ─────────────────────────────────────────────────────────

TEST_CASE("parseAdminGithubUsers normalizes case, whitespace and duplicates", "[auth][allowlist]") {
    const auto parsed = parseAdminGithubUsers("  Alice , BOB,alice ,, bob  ,Carol");
    CHECK(parsed == std::vector<std::string>{"alice", "bob", "carol"});
}

TEST_CASE("an empty or blank allow-list grants nobody access", "[auth][allowlist]") {
    CHECK(parseAdminGithubUsers("").empty());
    CHECK(parseAdminGithubUsers("   ").empty());
    CHECK(parseAdminGithubUsers(",,, ,").empty());
    CHECK_FALSE(isAllowedAdminLogin(parseAdminGithubUsers(""), "alice"));
}

TEST_CASE("allow-list matching is case-insensitive", "[auth][allowlist]") {
    const auto allow = parseAdminGithubUsers("Operator-One");
    CHECK(isAllowedAdminLogin(allow, "operator-one"));
    CHECK(isAllowedAdminLogin(allow, "OPERATOR-ONE"));
    CHECK(isAllowedAdminLogin(allow, "Operator-One"));
    CHECK_FALSE(isAllowedAdminLogin(allow, "operator-two"));
    CHECK_FALSE(isAllowedAdminLogin(allow, ""));
    CHECK_FALSE(isAllowedAdminLogin(allow, "operator-one-extra"));
}

// ── cookie parsing ───────────────────────────────────────────────────────────

TEST_CASE("cookieValue extracts the named cookie only", "[auth][cookie]") {
    CHECK(cookieValue("pp_session=abc", "pp_session") == "abc");
    CHECK(cookieValue("a=1; pp_session=abc; b=2", "pp_session") == "abc");
    CHECK(cookieValue("  pp_session = abc ; b=2", "pp_session") == "abc");
    CHECK(cookieValue("", "pp_session").empty());
    CHECK(cookieValue("other=abc", "pp_session").empty());
    // A cookie whose name merely ends with the target name must not match.
    CHECK(cookieValue("xpp_session=abc", "pp_session").empty());
    CHECK(cookieValue("pp_session_extra=abc", "pp_session").empty());
    // A base64url value containing '=' is impossible, but a truncated one is
    // returned verbatim rather than split.
    CHECK(cookieValue("pp_session=a=b", "pp_session") == "a=b");
}

TEST_CASE("issued cookies carry the required attributes (AC3)", "[auth][cookie]") {
    const std::string header = setCookieHeader("pp_session", "value", "Lax", 3600);
    CHECK(header.find("pp_session=value") != std::string::npos);
    CHECK(header.find("HttpOnly") != std::string::npos);
    CHECK(header.find("Secure") != std::string::npos);
    CHECK(header.find("SameSite=Lax") != std::string::npos);
    CHECK(header.find("Path=/") != std::string::npos);
    CHECK(header.find("Max-Age=3600") != std::string::npos);

    const std::string cleared = clearCookieHeader("pp_session");
    CHECK(cleared.find("Max-Age=0") != std::string::npos);
    CHECK(cleared.find("HttpOnly") != std::string::npos);
    CHECK(cleared.find("Secure") != std::string::npos);
}

// ── CSRF proof (AC14) ────────────────────────────────────────────────────────

TEST_CASE("hasSameSiteProof accepts only genuine same-origin evidence", "[auth][csrf]") {
    AdminRequestView view;

    view.secFetchSite = "same-origin";
    CHECK(hasSameSiteProof(view));

    for (const std::string& hostile : {"cross-site", "same-site", "none", "SAME-ORIGIN"}) {
        view.secFetchSite = hostile;
        CHECK_FALSE(hasSameSiteProof(view));
    }

    // Falls back to Origin/Host only when Sec-Fetch-Site is absent.
    view.secFetchSite = "";
    view.host         = "puzzle.example";
    view.origin       = "https://puzzle.example";
    CHECK(hasSameSiteProof(view));

    view.origin = "https://evil.example";
    CHECK_FALSE(hasSameSiteProof(view));

    view.origin = "https://puzzle.example.evil.test";
    CHECK_FALSE(hasSameSiteProof(view));

    view.origin = "puzzle.example"; // no scheme
    CHECK_FALSE(hasSameSiteProof(view));

    // Neither header present: no proof at all.
    view.origin = "";
    CHECK_FALSE(hasSameSiteProof(view));

    view.origin = "https://puzzle.example";
    view.host   = "";
    CHECK_FALSE(hasSameSiteProof(view));
}

// ── central admin guard ──────────────────────────────────────────────────────

TEST_CASE("with no mechanism configured every admin request is denied (AC9)", "[auth][guard]") {
    Config cfg; // no ADMIN_TOKEN, no allow-list, no signing secret

    AdminRequestView view;
    auto denied = authorizeAdminRequest(cfg, view, kNow);
    CHECK_FALSE(denied.authorized);
    CHECK(denied.statusCode == 401);
    CHECK(denied.reason == AdminDenyReason::NotConfigured);

    // Presenting credentials does not help when nothing is configured.
    view.adminTokenHeader = "anything";
    CHECK_FALSE(authorizeAdminRequest(cfg, view, kNow).authorized);

    view.adminTokenHeader = "";
    view.cookieHeader     = "pp_session=whatever";
    CHECK_FALSE(authorizeAdminRequest(cfg, view, kNow).authorized);
}

TEST_CASE("a blank ADMIN_TOKEN never authorizes a request with no header", "[auth][guard]") {
    // Regression for the old fail-open guard: it returned "allowed" whenever
    // ADMIN_TOKEN was empty.
    Config cfg;
    cfg.adminToken = "";
    cfg.sessionSigningSecret = "s";
    cfg.adminGithubUsers     = parseAdminGithubUsers("operator-one");

    AdminRequestView view;
    const auto result = authorizeAdminRequest(cfg, view, kNow);
    CHECK_FALSE(result.authorized);
    CHECK(result.statusCode == 401);
}

TEST_CASE("a configured X-Admin-Token still authorizes (AC12)", "[auth][guard]") {
    Config cfg;
    cfg.adminToken = "correct-horse-battery-staple";

    AdminRequestView view;
    view.adminTokenHeader = "correct-horse-battery-staple";
    const auto allowed = authorizeAdminRequest(cfg, view, kNow);
    CHECK(allowed.authorized);
    CHECK(allowed.viaToken);
    CHECK_FALSE(allowed.viaCookie);

    // A header credential is not sent ambiently, so a POST needs no CSRF proof.
    view.isPost = true;
    CHECK(authorizeAdminRequest(cfg, view, kNow).authorized);

    for (const std::string& wrong : {"", "correct-horse-battery-stapl",
                                     "correct-horse-battery-staplex", "CORRECT-HORSE-BATTERY-STAPLE"}) {
        AdminRequestView bad;
        bad.adminTokenHeader = wrong;
        CHECK_FALSE(authorizeAdminRequest(cfg, bad, kNow).authorized);
    }
}

TEST_CASE("an allow-listed session authorizes a GET admin request", "[auth][guard]") {
    const Config cfg = signingConfig();
    const auto result =
        authorizeAdminRequest(cfg, getView(sessionCookieFor(cfg, "Operator-One", kNow + 600)), kNow);
    CHECK(result.authorized);
    CHECK(result.viaCookie);
    CHECK_FALSE(result.viaToken);
    CHECK(result.login == "Operator-One");
}

TEST_CASE("a session for a login outside the allow-list is denied", "[auth][guard]") {
    const Config cfg = signingConfig();
    const auto result =
        authorizeAdminRequest(cfg, getView(sessionCookieFor(cfg, "intruder", kNow + 600)), kNow);
    CHECK_FALSE(result.authorized);
    CHECK(result.statusCode == 401);
    CHECK(result.reason == AdminDenyReason::NotAllowListed);
}

TEST_CASE("removing a login from the allow-list takes effect immediately (AC7)", "[auth][guard]") {
    const Config cfg = signingConfig();
    const std::string cookie = sessionCookieFor(cfg, "operator-one", kNow + 6000);
    REQUIRE(authorizeAdminRequest(cfg, getView(cookie), kNow).authorized);

    Config revoked = cfg;
    revoked.adminGithubUsers = parseAdminGithubUsers("operator-two");

    // The very same unexpired cookie no longer authorizes anything.
    const auto result = authorizeAdminRequest(revoked, getView(cookie), kNow);
    CHECK_FALSE(result.authorized);
    CHECK(result.reason == AdminDenyReason::NotAllowListed);
}

TEST_CASE("expired and tampered session cookies are denied by the guard", "[auth][guard]") {
    const Config cfg = signingConfig();

    const auto expired =
        authorizeAdminRequest(cfg, getView(sessionCookieFor(cfg, "operator-one", kNow - 1)), kNow);
    CHECK_FALSE(expired.authorized);
    CHECK(expired.reason == AdminDenyReason::BadSession);

    std::string tampered = sessionCookieFor(cfg, "operator-one", kNow + 600);
    tampered.back()      = (tampered.back() == 'A') ? 'B' : 'A';
    const auto forged    = authorizeAdminRequest(cfg, getView(tampered), kNow);
    CHECK_FALSE(forged.authorized);
    CHECK(forged.reason == AdminDenyReason::BadSession);
}

TEST_CASE("an allow-list without a signing secret cannot authorize anyone", "[auth][guard]") {
    Config cfg = signingConfig();
    const std::string cookie = sessionCookieFor(cfg, "operator-one", kNow + 600);

    cfg.sessionSigningSecret.clear();
    cfg.adminToken.clear();

    const auto result = authorizeAdminRequest(cfg, getView(cookie), kNow);
    CHECK_FALSE(result.authorized);
    CHECK(result.reason == AdminDenyReason::NotConfigured);
}

TEST_CASE("a cookie-authorized POST needs same-origin proof (AC14)", "[auth][guard][csrf]") {
    const Config cfg = signingConfig();
    const std::string cookie = sessionCookieFor(cfg, "operator-one", kNow + 600);

    // Cross-site POST carrying a perfectly valid session but no proof.
    AdminRequestView crossSite = postView(cookie);
    crossSite.secFetchSite     = "cross-site";
    crossSite.origin           = "https://evil.example";
    crossSite.host             = "puzzle.example";
    const auto rejected        = authorizeAdminRequest(cfg, crossSite, kNow);
    CHECK_FALSE(rejected.authorized);
    CHECK(rejected.statusCode == 403);
    CHECK(rejected.reason == AdminDenyReason::CsrfCheckFailed);

    // A form POST from another site sends no Sec-Fetch-Site in older browsers
    // and no Origin at all — still no proof.
    AdminRequestView bare = postView(cookie);
    CHECK_FALSE(authorizeAdminRequest(cfg, bare, kNow).authorized);
    CHECK(authorizeAdminRequest(cfg, bare, kNow).reason == AdminDenyReason::CsrfCheckFailed);

    // Sensitivity: the identical request with same-origin proof is authorized,
    // so the rejections above are the CSRF check and not a session failure.
    AdminRequestView sameOrigin = postView(cookie);
    sameOrigin.secFetchSite     = "same-origin";
    CHECK(authorizeAdminRequest(cfg, sameOrigin, kNow).authorized);

    AdminRequestView originProof = postView(cookie);
    originProof.origin           = "https://puzzle.example";
    originProof.host             = "puzzle.example";
    CHECK(authorizeAdminRequest(cfg, originProof, kNow).authorized);

    // The same cross-site request as a GET is still allowed: CSRF protection
    // covers state-changing POSTs.
    AdminRequestView crossSiteGet = getView(cookie);
    crossSiteGet.secFetchSite     = "cross-site";
    CHECK(authorizeAdminRequest(cfg, crossSiteGet, kNow).authorized);
}

TEST_CASE("a valid X-Admin-Token wins even when the session cookie is bad", "[auth][guard]") {
    Config cfg      = signingConfig();
    cfg.adminToken  = "the-token";

    AdminRequestView view = postView("garbage");
    view.adminTokenHeader = "the-token";
    CHECK(authorizeAdminRequest(cfg, view, kNow).authorized);
}

TEST_CASE("a wrong X-Admin-Token falls through to the cookie mechanism", "[auth][guard]") {
    Config cfg     = signingConfig();
    cfg.adminToken = "the-token";

    AdminRequestView view = getView(sessionCookieFor(cfg, "operator-one", kNow + 600));
    view.adminTokenHeader = "not-the-token";
    const auto result     = authorizeAdminRequest(cfg, view, kNow);
    CHECK(result.authorized);
    CHECK(result.viaCookie);
}

// ── startup diagnostics (AC10, AC13) ─────────────────────────────────────────

TEST_CASE("startup diagnostics name the missing variables and no values", "[auth][diagnostics]") {
    Config cfg;
    const auto messages = startupAuthDiagnostics(cfg);
    REQUIRE(messages.size() >= 2);

    const std::string joined = [&] {
        std::string all;
        for (const auto& m : messages) all += m + "\n";
        return all;
    }();
    CHECK(joined.find("ADMIN_TOKEN") != std::string::npos);
    CHECK(joined.find("ADMIN_GITHUB_USERS") != std::string::npos);
    CHECK(joined.find("SESSION_SIGNING_SECRET") != std::string::npos);
    CHECK(joined.find("401") != std::string::npos);
    CHECK(joined.find("503") != std::string::npos);
}

TEST_CASE("a fully configured deployment emits no auth warnings", "[auth][diagnostics]") {
    Config cfg = signingConfig();
    cfg.adminToken              = "t";
    cfg.githubOauthClientId     = "id";
    cfg.githubOauthClientSecret = "secret";
    CHECK(startupAuthDiagnostics(cfg).empty());
}

TEST_CASE("diagnostics never echo a configured secret value", "[auth][diagnostics]") {
    Config cfg;
    cfg.adminToken           = "super-secret-admin-token";
    cfg.sessionSigningSecret = "";
    for (const auto& message : startupAuthDiagnostics(cfg)) {
        CHECK(message.find("super-secret-admin-token") == std::string::npos);
    }
}
