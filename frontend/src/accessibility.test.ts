/// <reference types="node" />

import { describe, expect, it } from 'vitest';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const thisDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(thisDir, '..');
const html = fs.readFileSync(path.join(repoRoot, 'index.html'), 'utf8');
const dashboardTs = fs.readFileSync(path.join(repoRoot, 'src', 'dashboard.ts'), 'utf8');

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
});
