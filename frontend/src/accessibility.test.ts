/// <reference types="node" />

import { describe, expect, it } from 'vitest';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const thisDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(thisDir, '..');
const html = fs.readFileSync(path.join(repoRoot, 'index.html'), 'utf8');
const dashboardTs = fs.readFileSync(path.join(repoRoot, 'src', 'dashboard.ts'), 'utf8');
const canvasTs = fs.readFileSync(path.join(repoRoot, 'src', 'canvas.ts'), 'utf8');
const apiTs = fs.readFileSync(path.join(repoRoot, 'src', 'api.ts'), 'utf8');
const authTs = fs.readFileSync(path.join(repoRoot, 'src', 'auth.ts'), 'utf8');

// Returns the opening tag of the element carrying `id`, so an attribute
// assertion cannot be satisfied by a coincidental match elsewhere in the file.
function openingTag(source: string, id: string): string {
  const match = source.match(new RegExp(`<[a-z]+[^>]*\\sid="${id}"[^>]*>`));
  if (!match) throw new Error(`no element with id="${id}"`);
  return match[0];
}

// Returns just the `if (result.unauthorized)` block of the modal-confirm
// handler. `refreshAuthState()` is also called at startup, so a negative
// assertion about how it is called there must not read the whole file.
function unauthorizedBranch(source: string): string {
  const match = source.match(/if \(result\.unauthorized\) \{[\s\S]*?\n  \}/);
  if (!match) throw new Error('no if (result.unauthorized) branch');
  return match[0];
}

describe('frontend accessibility regressions', () => {
  it('defines a prefers-reduced-motion override', () => {
    expect(html).toMatch(/@media\s*\(prefers-reduced-motion:\s*reduce\)/);
    expect(html).toMatch(/transition-duration:\s*0\.01ms\s*!important/);
    expect(html).toMatch(/animation-duration:\s*0\.01ms\s*!important/);
    expect(html).toMatch(/\.puzzle-status-link:hover\s+\.puzzle-status-chip\s*\{\s*transform:\s*none;/s);
  });

  it('marks filter buttons as toggle buttons with aria-pressed defaults', () => {
    expect(html).toMatch(/data-layer="completed"\s+aria-pressed="true"/);
    expect(html).toMatch(/data-layer="all"\s+aria-pressed="false"/);
    expect(html).toMatch(/data-gen="feistel"\s+aria-pressed="true"/);
    expect(html).toMatch(/data-layer="native"\s+aria-pressed="true"/);
  });

  it('exposes each visualization filter set as a named group in the accessibility tree', () => {
    expect(html).toMatch(
      /<div class="alloc-filter-group" id="hm-layer-filter" role="group" aria-label="Night Sky Heatmap layer filter">/,
    );
    expect(html).toMatch(
      /<div class="alloc-filter-group" id="alloc-generation-filter" role="group" aria-label="Allocator Diagnostics generation filter">/,
    );
    expect(html).toMatch(
      /<div class="alloc-filter-group" id="hil-layer-filter" role="group" aria-label="Hilbert Curve Mapping layer filter">/,
    );
  });

  it('gives each filter group a unique aria-label identifying its visualization and dimension', () => {
    const labels = [...html.matchAll(/<div class="alloc-filter-group"[^>]*aria-label="([^"]+)"/g)].map(
      (m) => m[1],
    );
    expect(labels).toEqual([
      'Night Sky Heatmap layer filter',
      'Allocator Diagnostics generation filter',
      'Hilbert Curve Mapping layer filter',
    ]);
    expect(new Set(labels).size).toBe(labels.length);
  });

  it('marks every filter group container with role="group"', () => {
    const groups = html.match(/<div class="alloc-filter-group"[^>]*>/g) ?? [];
    expect(groups).toHaveLength(3);
    for (const group of groups) {
      expect(group).toContain('role="group"');
    }
  });

  it('synchronizes filter button aria-pressed state in dashboard logic', () => {
    expect(dashboardTs).toMatch(/setAttribute\('aria-pressed', String\(isActive\)\)/);
  });

  it('uses native details disclosures for API reference blocks', () => {
    expect(html).toMatch(/<details class="api-block">/);
    expect(html).toMatch(/<summary class="api-header">/);
    expect(dashboardTs).not.toMatch(/initApiReferencePanels/);
  });

  it('applies content-visibility to heavy visualization sections', () => {
    expect(html).toMatch(/content-visibility:\s*auto;/);
    expect(html).toMatch(/contain-intrinsic-size:\s*auto none auto 400px;/);
  });

  it('styles puzzle status chips as lower-case compact pills with solved green and unsolved red', () => {
    expect(html).toMatch(/\.puzzle-status-chip\s*\{[\s\S]*border-radius:\s*6px;/);
    expect(html).toMatch(/\.puzzle-status-chip\.is-solved\s*\{[\s\S]*color:\s*var\(--accent-green\);/);
    expect(html).toMatch(/\.puzzle-status-chip\.is-unsolved\s*\{[\s\S]*color:\s*var\(--accent-red\);/);
    expect(dashboardTs).toMatch(/badge\.textContent = \(status\.label \|\| status\.state\)\.toLowerCase\(\);/);
  });

  it('prioritizes found and in-progress statuses over blocked/completed in mixed heatmap buckets', () => {
    expect(canvasTs).toMatch(/const STATUS_ORDER_ALL: ChunkStatus\[\] = \['FOUND', 'assigned', 'reclaimed', 'blocked', 'completed'\];/);
  });

  it('updates the backend connection badge on stats success and failure with wifi icons', () => {
    expect(html).toMatch(/id="backend-status"/);
    expect(html).toMatch(/id="backend-status-icon"/);
    expect(html).toMatch(/id="backend-status-label"/);
    expect(html).toMatch(/status-badge is-offline/);
    expect(html).toMatch(/\.status-badge\s*\{[\s\S]*background:\s*transparent;[\s\S]*border:\s*none;/);
    expect(html).toMatch(/\.status-icon\s*\{[\s\S]*width:\s*1rem;[\s\S]*height:\s*1rem;/);
    expect(html).toMatch(/@keyframes wifiWavePulse/);
    expect(html).toMatch(/status-badge\.is-online[\s\S]*status-icon-wave[\s\S]*animation:/);
    expect(html).toMatch(/wave-3\s*\{[\s\S]*animation-delay:\s*0s;/);
    expect(html).toMatch(/wave-2\s*\{[\s\S]*animation-delay:\s*0\.12s;/);
    expect(html).toMatch(/wave-1\s*\{[\s\S]*animation-delay:\s*0\.24s;/);
    expect(dashboardTs).toMatch(/setBackendStatus\('online'\);/);
    expect(dashboardTs).toMatch(/setBackendStatus\('offline'\);/);
  });

  it('adds a Last seen column to the all-time scores table', () => {
    expect(html).toMatch(/<th class="sticky-col-right">Last seen<\/th>/);
  });

  it('styles stale score timestamps in white with a score-specific class and updates the empty-state colspan', () => {
    expect(html).toMatch(/\.td-score-time-stale\s*\{\s*color:\s*#fff;/);
    expect(dashboardTs).toMatch(/emptyRow\(5, 'No completed work yet'\)/);
  });

  it('renders score last_seen values through the recency helper', () => {
    expect(dashboardTs).toMatch(/const lastSeenClass = isRecentUtc\(s\.last_seen\) \? 'td-score-time' : 'td-score-time td-score-time-stale';/);
    expect(dashboardTs).toMatch(/<td class="sticky-col-right \$\{lastSeenClass\}">\$\{fmtUtc\(s\.last_seen\)\}<\/td>/);
  });

  it('keeps Visible Workers and Keys Found on the shared td-time class', () => {
    expect(dashboardTs).toMatch(/<td class="sticky-col-right td-time">\$\{fmtUtc\(w\.last_seen\)\}<\/td>/);
    expect(dashboardTs).toMatch(/<td class="td-time">\$\{fmtUtc\(f\.created_at\)\}<\/td>/);
  });

  it('scopes keyboard-scrollable sticky table wrappers to Visible Workers and Scores only', () => {
    expect(html.match(/class="table-wrap table-wrap-scroll-x"/g) ?? []).toHaveLength(2);
    expect(html).toMatch(/<div class="table-wrap table-wrap-scroll-x" style="margin-bottom: 2rem;" tabindex="0" aria-label="Visible Workers table, scroll horizontally">/);
    expect(html).toMatch(/<div class="table-wrap table-wrap-scroll-x" style="margin-bottom: 2rem;" tabindex="0" aria-label="Scores table, scroll horizontally">/);
    expect(html).not.toMatch(/aria-label="Keys Found table, scroll horizontally"/);
  });

  it('defines scoped sticky-column table styles with opaque per-side backgrounds, tinted sticky headers, and no separators', () => {
    expect(html).toMatch(/\.table-wrap-scroll-x\s*\{[\s\S]*overflow-x:\s*auto;[\s\S]*overflow-y:\s*hidden;/);
    expect(html).toMatch(/\.table-wrap-scroll-x:focus-visible\s*\{[\s\S]*outline:\s*2px solid rgba\(0,255,255,0\.45\);/);
    expect(html).toMatch(/\.table-wrap-scroll-x table\s*\{[\s\S]*width:\s*max-content;[\s\S]*min-width:\s*100%;/);

    // The grouped base rule keeps sticky positioning but no longer paints a
    // single shared flat fill — each column supplies its own opaque per-side base.
    const groupedBase = html.match(/\.sticky-col-left-1,\s*\.sticky-col-left-2,\s*\.sticky-col-right\s*\{([^}]*)\}/)?.[1] ?? '';
    expect(groupedBase).toContain('position: sticky');
    expect(groupedBase).toContain('background-clip: padding-box');
    expect(groupedBase).not.toMatch(/background:\s*var\(--bg-secondary\)/);

    // Per-side opaque fill matches the local gradient tone: left edge -> --bg-tertiary,
    // right edge -> --bg-secondary. Content must not bleed through pinned cells.
    const left1Block = html.match(/\.sticky-col-left-1\s*\{([^}]*)\}/)?.[1] ?? '';
    const left2Block = html.match(/\.sticky-col-left-2\s*\{([^}]*)\}/)?.[1] ?? '';
    const rightBlock = html.match(/\.sticky-col-right\s*\{([^}]*right:\s*0;[^}]*)\}/)?.[1] ?? '';
    expect(left1Block).toContain('background-color: var(--bg-tertiary)');
    expect(left2Block).toContain('background-color: var(--bg-tertiary)');
    expect(rightBlock).toContain('background-color: var(--bg-secondary)');

    // Separator treatment (vertical line + scroll-edge shadow) is fully removed.
    expect(left2Block).not.toContain('border-right');
    expect(left2Block).not.toContain('box-shadow');
    expect(rightBlock).not.toContain('border-left');
    expect(rightBlock).not.toContain('box-shadow');

    // Generic row hover must exclude sticky cells so it does not reset their
    // per-side opaque background-color to a translucent shorthand fill.
    expect(html).toMatch(/tbody tr:hover td:not\(\.sticky-col-left-1\):not\(\.sticky-col-left-2\):not\(\.sticky-col-right\)\s*\{\s*background:\s*rgba\(255,255,255,0\.02\);/);

    // Sticky-cell hover then adds only the translucent white overlay on top of
    // that preserved opaque base instead of using a flat #1a1a1a fill.
    const hoverBlock = html.match(/tbody tr:hover td\.sticky-col-left-1,[\s\S]*?\{([^}]*)\}/)?.[1] ?? '';
    expect(hoverBlock).toMatch(/background-image:\s*linear-gradient\(rgba\(255,255,255,0\.02\),\s*rgba\(255,255,255,0\.02\)\)/);
    expect(hoverBlock).not.toContain('#1a1a1a');
    expect(hoverBlock).not.toMatch(/\bbackground:\s*rgba\(255,255,255,0\.02\)/);

    // Dimmed/offline rows no longer override sticky cells with a distinct #151515 fill.
    expect(html).not.toContain('#151515');

    // Sticky header cells composite the cyan header tint over their opaque base so
    // pinned headers match the tinted non-sticky header cells at rest.
    const headerBlock = html.match(/\.table-wrap-scroll-x thead \.sticky-col-left-1,[\s\S]*?\{([^}]*)\}/)?.[1] ?? '';
    expect(headerBlock).toContain('z-index: 3');
    expect(headerBlock).toMatch(/background-image:\s*linear-gradient\(rgba\(0,255,255,0\.04\),\s*rgba\(0,255,255,0\.04\)\)/);

    // The table card gradient must stay horizontal: the flat per-side sticky
    // fills can only match a gradient whose color does not vary with row
    // height. A diagonal axis (e.g. 145deg) makes the pinned "Last Seen"
    // column render as a visibly darker band near the top of the table.
    const tableWrapBlock = html.match(/\.table-wrap\s*\{([^}]*)\}/)?.[1] ?? '';
    expect(tableWrapBlock).toMatch(/background:\s*linear-gradient\(90deg,\s*var\(--bg-tertiary\)\s*0%,\s*var\(--bg-secondary\)\s*100%\)/);
  });

  it('marks the sticky headers and cells for worker and score identity plus recency columns', () => {
    expect(html).toMatch(/<th class="sticky-col-left-1">#?<\/th>/);
    expect(html).toMatch(/<th class="sticky-col-left-2">Worker<\/th>/);
    expect(html).toMatch(/<th class="sticky-col-right">Last Seen<\/th>/);
    expect(html).toMatch(/<th class="sticky-col-right">Last seen<\/th>/);
    expect(dashboardTs).toMatch(/<td class="sticky-col-left-1">\$\{dot\}<\/td>/);
    expect(dashboardTs).toMatch(/<td class="sticky-col-left-2 td-name">\$\{esc\(w\.name\)\}<\/td>/);
    expect(dashboardTs).toMatch(/<td class="sticky-col-right td-time">\$\{fmtUtc\(w\.last_seen\)\}<\/td>/);
    expect(dashboardTs).toMatch(/<td class="sticky-col-left-1 td-rank">#\$\{formatIntegerDots\(i \+ 1\)\}<\/td>/);
    expect(dashboardTs).toMatch(/<td class="sticky-col-left-2 td-name">\$\{esc\(s\.worker_name\)\}<\/td>/);
    expect(dashboardTs).toMatch(/<td class="sticky-col-right \$\{lastSeenClass\}">\$\{fmtUtc\(s\.last_seen\)\}<\/td>/);
  });

  it('renders emphasized heatmap statuses in a second paint pass above completed and blocked dots', () => {
    expect(canvasTs).toMatch(/const statusPasses:\s*ChunkStatus\[\]\[\]\s*=\s*\[\s*\['completed', 'blocked'\],\s*\['reclaimed', 'assigned', 'FOUND'\],\s*\];/);
    expect(canvasTs).toMatch(/for \(const statuses of statusPasses\)[\s\S]*for \(const cell of cells\)/);
  });
});

// ── GitHub sign-in, identity and sign-out (story #149 AC15-AC19, AC27) ────────
//
// Every assertion that an attribute, field or storage path is *absent* is paired
// with a sensitivity check: the same probe is run over a copy of the real source
// that reintroduces the prohibited thing, and must report it. An absence
// assertion whose probe cannot detect the behaviour proves nothing.

describe('dashboard authentication controls', () => {
  const carriesAriaPressed = (tag: string): boolean => /\saria-pressed=/.test(tag);
  const readsAdminTokenStorage = (source: string): boolean =>
    /sessionStorage|localStorage|adminToken/.test(source);
  const sendsAdminTokenHeader = (source: string): boolean => /X-Admin-Token/.test(source);
  const declaresTokenField = (source: string): boolean =>
    /modal-token-input|class="modal-field"|<input/.test(source);
  const injectsIdentityAsMarkup = (source: string): boolean => /innerHTML[^\n]*authState/.test(source);
  // A live region that is populated while out of the accessibility tree and only
  // then revealed is the case assistive technology is least likely to announce.
  // These three probes cover every way the hint could be taken out of that tree.
  const hintTagCarriesHidden = (source: string): boolean =>
    /\shidden\b/.test(openingTag(source, 'ks-auth-hint'));
  const hintCssRemovesTheHint = (source: string): boolean =>
    /[#.]ks-auth-hint[^{}]*\{[^}]*display:\s*none/.test(source);
  const togglesHintVisibility = (source: string): boolean => /authHintEl\.(hidden|style)\b/.test(source);

  const headerHtml = html.match(/<div class="header">[\s\S]*?\n        <\/div>/)?.[0] ?? '';
  const identityGroupHtml = html.match(/<div id="auth-identity"[\s\S]*?<\/div>/)?.[0] ?? '';
  const activateModalHtml =
    html.match(/<div class="modal-overlay" id="activate-modal"[\s\S]*?\n    <\/div>/)?.[0] ?? '';
  const narrowViewportCss = html.match(/@media \(max-width: 768px\) \{([\s\S]*?)\n        \}/)?.[1] ?? '';

  it('groups the auth controls with the backend status inside the header', () => {
    expect(headerHtml).toContain('class="header-actions"');
    expect(headerHtml).toContain('id="backend-status"');
    expect(headerHtml).toContain('id="auth-signin-btn"');
    expect(headerHtml).toContain('id="auth-identity"');
  });

  it('gives the sign-in control a visible accessible name', () => {
    expect(html).toMatch(
      /<button id="auth-signin-btn"[^>]*type="button"[^>]*>Sign in with GitHub<\/button>/,
    );
  });

  it('exposes the signed-in identity as a named group holding the avatar, login and sign-out control', () => {
    const group = openingTag(html, 'auth-identity');
    expect(group).toContain('role="group"');
    expect(group).toMatch(/aria-label="[^"]+"/);

    expect(identityGroupHtml).toContain('id="auth-avatar"');
    expect(identityGroupHtml).toContain('id="auth-login"');
    expect(identityGroupHtml).toContain('id="auth-signout-btn"');
    expect(html).toMatch(
      /<button id="auth-signout-btn"[^>]*aria-label="Sign out of admin session"[^>]*>Sign out<\/button>/,
    );
  });

  it('paints signed out by default, with the identity group hidden', () => {
    expect(openingTag(html, 'auth-identity')).toMatch(/\shidden\b/);
    expect(openingTag(html, 'auth-signin-btn')).not.toMatch(/\shidden\b/);
    expect(dashboardTs).toMatch(/let authState: AuthState = SIGNED_OUT;/);
  });

  it('keeps the sign-in and sign-out controls mutually exclusive', () => {
    expect(dashboardTs).toMatch(/authSigninBtn\.hidden = authState\.signedIn;/);
    expect(dashboardTs).toMatch(/authIdentityEl\.hidden = !authState\.signedIn;/);
    // `hidden` has to beat the inline-flex display these elements carry.
    expect(html).toMatch(/#auth-signin-btn\[hidden\],[\s\S]*?#auth-identity\[hidden\],[\s\S]*?\{\s*display: none;/);
  });

  it('never marks the sign-in or sign-out control as a toggle button (D15/AC19)', () => {
    for (const id of ['auth-signin-btn', 'auth-signout-btn']) {
      expect(carriesAriaPressed(openingTag(html, id))).toBe(false);
    }

    for (const id of ['auth-signin-btn', 'auth-signout-btn']) {
      const mutated = html.replace(`<button id="${id}"`, `<button aria-pressed="false" id="${id}"`);
      expect(carriesAriaPressed(openingTag(mutated, id))).toBe(true);
    }
  });

  it('keeps aria-pressed reserved for the visualization filter toggle groups', () => {
    const tags = html.match(/<[a-z]+[^>]*\saria-pressed="[^"]*"[^>]*>/g) ?? [];
    expect(tags.length).toBeGreaterThan(0);
    for (const tag of tags) expect(tag).toContain('class="alloc-filter-btn');
  });

  it('removes the shared-token field from the activation modal but keeps the confirmation', () => {
    expect(declaresTokenField(activateModalHtml)).toBe(false);
    expect(html).not.toMatch(/\.modal-field\s*\{/);
    expect(activateModalHtml).toContain('id="modal-puzzle-name"');
    expect(activateModalHtml).toContain('id="modal-error"');
    expect(activateModalHtml).toContain('id="modal-cancel"');
    expect(activateModalHtml).toContain('id="modal-confirm"');

    const restored = activateModalHtml.replace(
      '<div class="modal-error"',
      '<div class="modal-field"><input type="password" id="modal-token-input"></div><div class="modal-error"',
    );
    expect(declaresTokenField(restored)).toBe(true);
  });

  it('removes every browser-storage admin-token path', () => {
    for (const source of [html, dashboardTs, apiTs, authTs]) {
      expect(readsAdminTokenStorage(source)).toBe(false);
    }
    expect(readsAdminTokenStorage(`${dashboardTs}\nsessionStorage.setItem('adminToken', token);\n`)).toBe(true);
  });

  it('stops sending an admin token header from the browser', () => {
    expect(sendsAdminTokenHeader(apiTs)).toBe(false);
    expect(sendsAdminTokenHeader(`${apiTs}\nheaders['X-Admin-Token'] = token;\n`)).toBe(true);
  });

  it('sends same-origin credentials on the auth and cookie-authorized admin calls', () => {
    const calls = [
      apiTs.match(/fetch\('\/api\/v1\/auth\/me'[\s\S]*?\);/)?.[0] ?? '',
      apiTs.match(/fetch\('\/api\/v1\/auth\/logout'[\s\S]*?\}\);/)?.[0] ?? '',
      apiTs.match(/fetch\('\/api\/v1\/admin\/activate-puzzle'[\s\S]*?\}\);/)?.[0] ?? '',
    ];
    for (const call of calls) {
      expect(call).not.toBe('');
      expect(call).toContain("credentials: 'same-origin'");
    }
    expect(apiTs).toMatch(/fetch\('\/api\/v1\/auth\/logout', \{\s*method: 'POST',/);
  });

  it('navigates sign-in through the backend OAuth entry point', () => {
    expect(dashboardTs).toMatch(/window\.location\.assign\('\/api\/v1\/auth\/github\/login'\);/);
  });

  it('renders the GitHub login as text, never as markup', () => {
    expect(dashboardTs).toMatch(/authLoginEl\.textContent = authState\.signedIn \? authState\.login : '';/);
    expect(injectsIdentityAsMarkup(dashboardTs)).toBe(false);
    expect(injectsIdentityAsMarkup(`${dashboardTs}\nauthLoginEl.innerHTML = authState.login;\n`)).toBe(true);
  });

  it('accepts only an https avatar and keeps the readable login when it fails to load', () => {
    expect(authTs).toMatch(/parsed\.protocol === 'https:'/);
    expect(dashboardTs).toMatch(
      /authAvatarEl\.addEventListener\('error', \(\) => \{ authAvatarEl\.hidden = true; \}\);/,
    );
    // Decorative: the accessible identity is the login text beside the image.
    const avatar = openingTag(html, 'auth-avatar');
    expect(avatar).toContain('alt=""');
    expect(avatar).toContain('aria-hidden="true"');
  });

  it('answers an unauthorized activation attempt with a non-blocking inline hint', () => {
    expect(openingTag(html, 'ks-auth-hint')).toContain('role="status"');
    // The hint replaces the modal instead of disabling the control.
    expect(dashboardTs).toMatch(
      /function requestActivate[\s\S]*?showAuthHint\(hint\);[\s\S]*?return;[\s\S]*?openActivateModal\(id, name\);/,
    );
    expect(dashboardTs).toMatch(/cell\.addEventListener\('click', \(\) => requestActivate\(/);
    expect(html).not.toMatch(/ks-toggle-cell[^>]*\sdisabled/);
  });

  it('keeps the activation hint an always-rendered live region so its text change is announced', () => {
    // The same shape as #backend-status: in the accessibility tree from the
    // start, with only the text swapped — never revealed already populated.
    const hint = openingTag(html, 'ks-auth-hint');
    expect(hint).toContain('role="status"');
    expect(hint).toContain('aria-live="polite"');
    expect(hintTagCarriesHidden(html)).toBe(false);
    expect(hintCssRemovesTheHint(html)).toBe(false);
    expect(togglesHintVisibility(dashboardTs)).toBe(false);
    expect(dashboardTs).toMatch(
      /function showAuthHint\(message: string\): void \{\s*authHintEl\.textContent = message;\s*\}/,
    );
    expect(dashboardTs).toMatch(/function clearAuthHint\(\): void \{\s*authHintEl\.textContent = '';\s*\}/);
    // Always rendered must not mean always taking up room in the strip.
    expect(html).toMatch(/\.ks-auth-hint:empty\s*\{\s*padding-top:\s*0;/);

    // Sensitivity: every probe above must fire on a source that puts the
    // populate-then-reveal pattern back.
    const reHidden = html.replace('id="ks-auth-hint"', 'id="ks-auth-hint" hidden');
    expect(hintTagCarriesHidden(reHidden)).toBe(true);
    const reDisplayNone = html.replace('#auth-avatar[hidden] {', '#auth-avatar[hidden],\n        #ks-auth-hint[hidden] {');
    expect(hintCssRemovesTheHint(reDisplayNone)).toBe(true);
    expect(togglesHintVisibility(`${dashboardTs}\n  authHintEl.hidden = false;\n`)).toBe(true);
    expect(togglesHintVisibility(`${dashboardTs}\n  authHintEl.style.display = 'none';\n`)).toBe(true);
  });

  it('closes the overlay and discards authenticated state on an admin 401', () => {
    expect(apiTs).toMatch(/unauthorized: res\.status === 401/);
    expect(dashboardTs).toMatch(
      /function discardAuthenticatedState\(\): void \{\s*closeActivateModal\(\);\s*authState = SIGNED_OUT;\s*renderAuthState\(\);\s*\}/,
    );
    expect(dashboardTs).toMatch(/if \(result\.unauthorized\) \{[\s\S]*?discardAuthenticatedState\(\);/);
  });

  it('words the admin-401 hint from the refreshed state, not the state it just reset', () => {
    // A 401 also answers an allow-list removal, whose session is still valid, so
    // refreshAuthState() puts the identity back in the header. Computing the
    // hint before that resolves renders "sign in" next to a signed-in account.
    // refusedActivationHint() is unit-tested against every resulting state in
    // auth.test.ts; this pins only the ordering, which is not visible there.
    expect(unauthorizedBranch(dashboardTs)).toMatch(
      /discardAuthenticatedState\(\);[\s\S]*?await refreshAuthState\(\);[\s\S]*?showAuthHint\(refusedActivationHint\(authState\)\)/,
    );
    // Fire-and-forget is the exact defect; it must not come back in any form.
    expect(unauthorizedBranch(dashboardTs)).not.toMatch(/void refreshAuthState\(\)/);

    // Sensitivity: both probes fire on a source with the old ordering restored.
    const reordered = dashboardTs.replace(
      /await refreshAuthState\(\);\s*showAuthHint\(refusedActivationHint\(authState\)\);/,
      'showAuthHint(refusedActivationHint(authState));\n    void refreshAuthState();',
    );
    expect(reordered).not.toBe(dashboardTs);
    expect(unauthorizedBranch(reordered)).not.toMatch(
      /discardAuthenticatedState\(\);[\s\S]*?await refreshAuthState\(\);[\s\S]*?showAuthHint\(refusedActivationHint\(authState\)\)/,
    );
    expect(unauthorizedBranch(reordered)).toMatch(/void refreshAuthState\(\)/);
  });

  it('signs out through a same-origin POST and re-reads the resulting state', () => {
    expect(dashboardTs).toMatch(
      /async function signOut\(\): Promise<void> \{[\s\S]*?discardAuthenticatedState\(\);[\s\S]*?await logout\(\);[\s\S]*?await refreshAuthState\(\);/,
    );
  });

  it('wraps the header and stacks the auth controls at the 768px breakpoint (D17/AC27)', () => {
    const headerBlock = html.match(/\.header\s*\{([^}]*)\}/)?.[1] ?? '';
    expect(headerBlock).toContain('flex-wrap: wrap');

    expect(narrowViewportCss).toMatch(/\.header-actions\s*\{[^}]*flex-basis:\s*100%/);
    expect(narrowViewportCss).toMatch(/\.auth-login\s*\{[^}]*max-width/);

    // The stage label is centered with `position: absolute` at desktop width and
    // would otherwise sit on top of h1 once the header stacks, so it rejoins the
    // flow at this breakpoint. Its styles must live in the stylesheet for that
    // override to be possible at all — an inline style attribute would win.
    expect(html).toMatch(/#stage-label\s*\{[^}]*position:\s*absolute/);
    expect(openingTag(html, 'stage-label')).not.toMatch(/position:\s*absolute/);
    expect(narrowViewportCss).toMatch(/#stage-label\s*\{[^}]*position:\s*static/);
  });

  it('bounds the identity strip so a long GitHub login cannot clip the header', () => {
    const loginBlock = html.match(/\.auth-login\s*\{([^}]*)\}/)?.[1] ?? '';
    expect(loginBlock).toContain('overflow: hidden');
    expect(loginBlock).toContain('text-overflow: ellipsis');
    expect(loginBlock).toMatch(/max-width:/);
    expect(html).toMatch(/\.header-actions\s*\{[^}]*flex-wrap:\s*wrap/);
  });
});
