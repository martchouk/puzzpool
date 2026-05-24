/// <reference types="node" />

import { describe, expect, it } from 'vitest';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const thisDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(thisDir, '..');
const dashboardTs = fs.readFileSync(path.join(repoRoot, 'src', 'dashboard.ts'), 'utf8');
const apiTs = fs.readFileSync(path.join(repoRoot, 'src', 'api.ts'), 'utf8');
const typesTs = fs.readFileSync(path.join(repoRoot, 'src', 'types.ts'), 'utf8');

describe('frontend visualization architecture regressions', () => {
  it('fetches visualization data through dedicated endpoints instead of stats payload chunks', () => {
    expect(apiTs).toMatch(/fetchHeatmapVisualization/);
    expect(apiTs).toMatch(/fetchHilbertVisualization/);
    expect(apiTs).toMatch(/fetchAllocatorVisualization/);
    expect(typesTs).not.toMatch(/chunks_vis:/);
  });

  it('loads visualization automatically only for initial puzzle context and puzzle switches', () => {
    expect(dashboardTs).toMatch(/if \(currentPuzzleId === null\)/);
    expect(dashboardTs).toMatch(/else if \(loadedVisualizationPuzzleId !== currentPuzzleId\)/);
    expect(dashboardTs).toMatch(/await loadAllVisualizationsForCurrentPuzzle\(\);/);
    expect(dashboardTs).not.toMatch(/setInterval\(\(\) => \{ void loadAllVisualizationsForCurrentPuzzle\(\); \}/);
  });

  it('supports per-panel manual refresh handlers for heavy visualizations', () => {
    expect(dashboardTs).toMatch(/heatmapRefreshBtn\.addEventListener\('click', \(\) => \{ void loadHeatmapVisualizationPanel\(\); \}\);/);
    expect(dashboardTs).toMatch(/allocatorRefreshBtn\.addEventListener\('click', \(\) => \{ void loadAllocatorVisualizationPanel\(\); \}\);/);
    expect(dashboardTs).toMatch(/hilbertRefreshBtn\.addEventListener\('click', \(\) => \{ void loadHilbertVisualizationPanel\(\); \}\);/);
  });
});
