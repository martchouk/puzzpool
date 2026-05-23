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

  it('styles puzzle status chips as lower-case compact pills with solved green and unsolved red', () => {
    expect(html).toMatch(/\.puzzle-status-chip\s*\{[\s\S]*border-radius:\s*6px;/);
    expect(html).toMatch(/\.puzzle-status-chip\.is-solved\s*\{[\s\S]*color:\s*var\(--accent-green\);/);
    expect(html).toMatch(/\.puzzle-status-chip\.is-unsolved\s*\{[\s\S]*color:\s*var\(--accent-red\);/);
    expect(dashboardTs).toMatch(/badge\.textContent = \(status\.label \|\| status\.state\)\.toLowerCase\(\);/);
  });

  it('prioritizes found and in-progress statuses over blocked/completed in mixed heatmap buckets', () => {
    expect(canvasTs).toMatch(/if \(bucket\.FOUND > 0\) return 'FOUND';[\s\S]*if \(bucket\.assigned > 0\) return 'assigned';[\s\S]*if \(bucket\.reclaimed > 0\) return 'reclaimed';[\s\S]*if \(bucket\.blocked > 0\) return 'blocked';/);
  });
});
