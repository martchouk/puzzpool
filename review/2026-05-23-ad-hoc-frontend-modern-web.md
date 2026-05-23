Author: mud-rev

## Supplemental Frontend Review — Modern Web Standards

**Scope:** Frontend changes on `dev` (2026-05-23) — puzzle status badges, collapsible API panels, heatmap filter refactor
**Branch:** `dev`
**Reviewer:** mud-rev
**Date:** 2026-05-23
**Guidance used:** `accessibility`, `search-hidden-content`, `html`, `animate-to-intrinsic-sizes`, `efficient-background-processing` (via `modern-web-guidance@2026_05_16-c5e7870`)

---

### Verdict

**CHANGES REQUESTED** (2 blocking, 1 strongly recommended, 2 suggestions)

---

### Finding 1 — MUST FIX: No `prefers-reduced-motion` override anywhere in the stylesheet

**Files:** `frontend/index.html` (CSS block, lines 28 and throughout)

The stylesheet defines `--transition: 0.3s cubic-bezier(0.4, 0, 0.2, 1)` and applies it across ~11 rules. The two new animation surfaces added by this PR — the badge hover lift and the API toggle SVG rotation — both use this variable:

```css
/* puzzle-status-chip */
transition: var(--transition);                /* badge color/border */

/* badge hover */
.puzzle-status-link:hover .puzzle-status-chip {
    transform: translateY(-1px);              /* motion */
}

/* api-toggle svg */
.api-toggle svg {
    transition: transform var(--transition);  /* motion */
}
.api-block.is-expanded .api-toggle svg {
    transform: rotate(180deg);                /* motion */
}
```

**There is no `@media (prefers-reduced-motion: reduce)` block anywhere in the file.** Users who have enabled "Reduce Motion" in their OS settings (a significant population including vestibular disorder patients) will receive the full animation experience with no accommodation.

**Required fix** — add a single block at the end of the `<style>` section:

```css
@media (prefers-reduced-motion: reduce) {
    *, *::before, *::after {
        transition-duration: 0.01ms !important;
        animation-duration: 0.01ms !important;
    }
    .puzzle-status-link:hover .puzzle-status-chip {
        transform: none;
    }
}
```

The global `transition-duration: 0.01ms` pattern is the idiomatic zero-cost override that preserves `transitionend` event firing (needed for any JS that listens for `transitionend`) while making all motion imperceptibly short.

> **Guidance:** Accessibility guide §10 (Motions and Preferences) — DO: "Support `@media (prefers-reduced-motion: reduce)` media queries." The `animate-to-intrinsic-sizes` guide marks this as MANDATORY for any animated component.

---

### Finding 2 — MUST FIX: Filter buttons have no `aria-pressed` state — selected filter is invisible to assistive technology

**Files:** `frontend/src/dashboard.ts:308-310`, `frontend/index.html:916-921`

The heatmap layer filter buttons (and the allocator/Hilbert equivalents) manage a visual `.active` CSS class via `sync()` but never set `aria-pressed`. A screen reader user cannot determine which filter is currently active:

```typescript
// Current — only visual state:
const sync = (): void => root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(b => {
    b.classList.toggle('active', b.dataset.layer === hmLayerFilter);
});
```

**Required fix** — include `aria-pressed` in the sync:

```typescript
const sync = (): void => root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(b => {
    const isActive = b.dataset.layer === hmLayerFilter;
    b.classList.toggle('active', isActive);
    b.setAttribute('aria-pressed', String(isActive));
});
```

Apply the same fix to `initAllocatorGenerationFilter` (`syncButtons`) and `initHilLayerFilter`.

The HTML buttons in the template should also carry `aria-pressed="false"` as a default to hint to AT that these are toggle buttons, except for the one with `active` which should have `aria-pressed="true"`:

```html
<!-- Before JS initializes, default state -->
<button class="alloc-filter-btn" data-layer="all" aria-pressed="false">All</button>
<button class="alloc-filter-btn active" data-layer="completed" aria-pressed="true">Done</button>
```

> **Guidance:** Accessibility guide §5 (Keyboard and Focus Management) — DO: "Utilize `aria-expanded` and `aria-pressed` to communicate toggle states for custom controls."

---

### Finding 3 — SHOULD FIX: Collapsible API panels use custom ARIA toggle; prefer `<details>` or `hidden="until-found"` for Ctrl+F findability

**Files:** `frontend/src/dashboard.ts:63-123` (`initApiReferencePanels`), `frontend/index.html` (API block HTML)

The `initApiReferencePanels()` function dynamically converts API reference blocks into custom ARIA disclosure widgets using `table.hidden = true/false`. This pattern has two consequences:

1. **Ctrl+F cannot find text inside collapsed panels.** API field names, types, and descriptions are invisible to browser Find-in-Page when a panel is collapsed. This is a significant usability regression for a reference document — users looking for a specific field name won't find it.

2. **The custom toggle adds JS complexity that `<details>` handles natively for free** (keyboard, mouse, animation via `::details-content`, no ARIA management needed).

**Preferred fix — replace the HTML structure with native `<details>`/`<summary>`:**

Each `.api-block` should become:

```html
<details class="api-block">
  <summary class="api-header">
    <span class="method">GET</span>
    <span class="api-path">/api/v1/stats</span>
    <span class="api-desc">Dashboard statistics</span>
  </summary>
  <table class="api-table">
    <!-- field rows -->
  </table>
</details>
```

This eliminates `initApiReferencePanels()` entirely, makes all content Ctrl+F-findable, and is keyboard-accessible with zero ARIA code.

**Lighter-weight alternative — keep the current structure but switch to `hidden="until-found"`:**

```typescript
// Replace:
table.hidden = isExpanded;

// With:
if (isExpanded) {
    table.setAttribute('hidden', 'until-found');
} else {
    table.removeAttribute('hidden');
}

// Also handle the beforematch event to sync aria-expanded when browser auto-expands:
table.addEventListener('beforematch', () => {
    toggle.setAttribute('aria-expanded', 'true');
    toggle.setAttribute('aria-label', `Collapse ${label}`);
    block.classList.add('is-expanded');
    block.classList.remove('is-collapsed');
});
```

With `hidden="until-found"`, browsers that support it (Chrome, Edge; Firefox and Safari need a feature-detect fallback per the guide) will reveal the panel automatically when a user's Ctrl+F search matches text inside it.

> **Guidance:** `search-hidden-content` guide — "The `<details>` element is generally recommended as it's simpler to implement and maintain." HTML guide §5 — "DO use `<details>` and `<summary>` for native accordions or revealable content without JS."

---

### Finding 4 — NIT: `aria-label` encodes action verb that duplicates `aria-expanded` state

**Files:** `frontend/src/dashboard.ts:92-93, 98-99`

```typescript
toggle.setAttribute('aria-label', `Expand ${label}`);
// …on toggle:
toggle.setAttribute('aria-label', `${isExpanded ? 'Expand' : 'Collapse'} ${label}`);
```

When `aria-expanded="true"` is also present on the button, some AT implementations will announce both the label verb and the expanded state, producing reads like *"Collapse /api/v1/stats details, expanded, button"* — slightly redundant. The accessibility guide §3 says: "Don't repeat state already exposed via ARIA (`aria-expanded`) inside the accessible name."

**Preferred fix** — keep the label constant and let `aria-expanded` carry the state:

```typescript
toggle.setAttribute('aria-label', `Toggle ${label}`);
// Remove the aria-label update inside togglePanel()
```

---

### Finding 5 — SUGGESTION: Add `content-visibility: auto` to canvas sections to conserve CPU/battery

**Files:** `frontend/index.html` (canvas wrapper elements)

The dashboard has three heavy canvas sections (heatmap, Hilbert curve, allocator diagnostics) that run `requestAnimationFrame`-driven redraws. If a user scrolls one off-screen, the browser still lays out and paints it. The `efficient-background-processing` guide (using `content-visibility: auto`) allows the browser to skip rendering for off-screen canvases and fire `contentvisibilityautostatechange` to pause/resume the draw loops.

```css
.heatmap-wrap,
.hilbert-wrap,
.alloc-diagnostics-wrap {
    content-visibility: auto;
    contain-intrinsic-size: auto none auto 400px; /* estimated height */
}
```

This is a progressive enhancement — browsers that don't support it render normally. No JS change is required unless the draw loops are expensive enough to warrant pausing them explicitly via the `contentvisibilityautostatechange` event.

> **Guidance:** `efficient-background-processing` guide — "Pause heavy background tasks when a component is not being rendered by the browser to conserve system resources and battery life."

---

### Summary Table

| # | Severity | Area | Finding |
|---|----------|------|---------|
| 1 | **MUST FIX** | CSS / Accessibility | No `prefers-reduced-motion` override — all transitions play for reduced-motion users |
| 2 | **MUST FIX** | ARIA / Accessibility | Filter buttons have no `aria-pressed` — active state invisible to screen readers |
| 3 | SHOULD FIX | HTML Semantics | Custom toggle pattern instead of `<details>` — API panel content not Ctrl+F-findable |
| 4 | NIT | ARIA | `aria-label` verb redundant with `aria-expanded` state |
| 5 | SUGGESTION | Performance | `content-visibility: auto` opportunity for off-screen canvas sections |

---

[📄 Full report: review/2026-05-23-ad-hoc-frontend-modern-web.md](https://github.com/martchouk/puzzpool/blob/dev/review/2026-05-23-ad-hoc-frontend-modern-web.md)
