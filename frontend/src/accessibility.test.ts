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
    expect(html).toMatch(/<th>Last seen<\/th>/);
  });

  it('styles stale score timestamps in white with a score-specific class and updates the empty-state colspan', () => {
    expect(html).toMatch(/\.td-score-time-stale\s*\{\s*color:\s*#fff;/);
    expect(dashboardTs).toMatch(/emptyRow\(5, 'No completed work yet'\)/);
  });

  it('renders score last_seen values through the recency helper', () => {
    expect(dashboardTs).toMatch(/const lastSeenClass = isRecentUtc\(s\.last_seen\) \? 'td-score-time' : 'td-score-time td-score-time-stale';/);
    expect(dashboardTs).toMatch(/<td class="\$\{lastSeenClass\}">\$\{fmtUtc\(s\.last_seen\)\}<\/td>/);
  });

  it('keeps Visible Workers and Keys Found on the shared td-time class', () => {
    expect(dashboardTs).toMatch(/<td class="td-time">\$\{fmtUtc\(w\.last_seen\)\}<\/td>/);
    expect(dashboardTs).toMatch(/<td class="td-time">\$\{fmtUtc\(f\.created_at\)\}<\/td>/);
  });

  it('renders emphasized heatmap statuses in a second paint pass above completed and blocked dots', () => {
    expect(canvasTs).toMatch(/const statusPasses:\s*ChunkStatus\[\]\[\]\s*=\s*\[\s*\['completed', 'blocked'\],\s*\['reclaimed', 'assigned', 'FOUND'\],\s*\];/);
    expect(canvasTs).toMatch(/for \(const statuses of statusPasses\)[\s\S]*for \(const cell of cells\)/);
  });
});
