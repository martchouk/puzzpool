/// <reference types="node" />

import { describe, expect, it } from 'vitest';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const thisDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(thisDir, '..');
const dashboardTs = fs.readFileSync(path.join(repoRoot, 'src', 'dashboard.ts'), 'utf8');
const canvasTs = fs.readFileSync(path.join(repoRoot, 'src', 'canvas.ts'), 'utf8');

describe('frontend performance regressions', () => {
  it('throttles Hilbert tooltip hit-testing with requestAnimationFrame', () => {
    expect(dashboardTs).toMatch(/let hilbertTooltipFrame = 0;/);
    expect(dashboardTs).toMatch(/if \(hilbertTooltipFrame\) return;/);
    expect(dashboardTs).toMatch(/hilbertTooltipFrame = requestAnimationFrame\(/);
  });

  it('shares allocator diagnostics work instead of recomputing normalized gap metrics separately', () => {
    expect(dashboardTs).toMatch(/const filteredChunks = getFilteredChunks\(\);/);
    expect(dashboardTs).toMatch(/const ngm = drawAllocatorDiagnostics\(allocCanvases, gapMetricsEl, filteredChunks\);/);
    expect(dashboardTs).not.toMatch(/exportNormalizedGapMetrics\(getFilteredChunks\(\)\)/);
    expect(canvasTs).toMatch(/export function drawAllocatorDiagnostics\([\s\S]*\)\s*:\s*ReturnType<typeof computeNormalizedGapMetricsFromSorted>/);
    expect(canvasTs).toMatch(/return computeNormalizedGapMetricsFromSorted\(sortedByS\);/);
  });

  it('avoids sorting allocator chunks by id before immediately sorting by start', () => {
    expect(canvasTs).not.toMatch(/function getAllocatorSortedStarts\(/);
    expect(canvasTs).toMatch(/const sortedByS\s*=\s*chunks[\s\S]*?\.sort\(\(a, b\) => a\.s - b\.s\)/);
  });
});
