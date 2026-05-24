import type {
  AllocatorGenerationVisualization,
  AllocatorScatterPoint,
  ChunkStatus,
  VisualizationCell,
} from './types.ts';
import { formatIntegerDots, formatGapNumber } from './format.ts';

export const CHUNK_COLORS: Record<ChunkStatus, string> = {
  completed: '#0f8',
  FOUND: '#f36',
  assigned: '#0cf',
  reclaimed: '#b80',
  blocked: '#fff',
};

export const CHUNK_GLOW_COLORS: Record<ChunkStatus, string> = {
  completed: 'rgba(0, 255, 136, 0.4)',
  FOUND: 'rgba(255, 51, 102, 1.0)',
  assigned: 'rgba(0, 204, 255, 0.5)',
  reclaimed: 'rgba(255, 187, 0, 0.3)',
  blocked: 'rgba(255, 255, 255, 0.55)',
};

export const MAP_COLS = 512;
export const MAP_ROWS = 128;
export const HILBERT_N = 256;

const STATUS_ORDER_ALL: ChunkStatus[] = ['FOUND', 'assigned', 'reclaimed', 'blocked', 'completed'];
const STATUS_ORDER_NATIVE: ChunkStatus[] = ['FOUND', 'assigned', 'reclaimed', 'completed'];
const STATUS_NAMES: Record<ChunkStatus, string> = {
  completed: 'Done',
  assigned: 'In Progress',
  reclaimed: 'Reclaimed',
  FOUND: 'Found',
  blocked: 'Blocked',
};

export type CellLookup = Map<number, VisualizationCell>;

function countAt(cell: VisualizationCell, status: ChunkStatus): number {
  const offset = status === 'completed' ? 1
    : status === 'assigned' ? 2
    : status === 'reclaimed' ? 3
    : status === 'FOUND' ? 4
    : 5;
  return cell[offset] ?? 0;
}

function totalAt(cell: VisualizationCell): number {
  return cell[1] + cell[2] + cell[3] + cell[4] + cell[5];
}

function dominantStatus(cell: VisualizationCell, statuses: ChunkStatus[]): ChunkStatus | null {
  for (const status of statuses) {
    if (countAt(cell, status) > 0) return status;
  }
  return null;
}

function hash32Mix(a: number, b = 0, c = 0, d = 0): number {
  let x = (a | 0) ^ 0x9e3779b9;
  x = Math.imul(x ^ (b | 0), 0x85ebca6b);
  x = Math.imul(x ^ (c | 0), 0xc2b2ae35);
  x = Math.imul(x ^ (d | 0), 0x27d4eb2f);
  x ^= x >>> 15;
  x = Math.imul(x, 0x85ebca6b);
  x ^= x >>> 13;
  x = Math.imul(x, 0xc2b2ae35);
  x ^= x >>> 16;
  return x >>> 0;
}

function buildCellLookup(cells: VisualizationCell[]): CellLookup {
  const lookup: CellLookup = new Map();
  for (const cell of cells) lookup.set(cell[0], cell);
  return lookup;
}

export function drawHeatmap(
  canvas: HTMLCanvasElement,
  cells: VisualizationCell[],
  filter: string,
): CellLookup {
  const W = canvas.offsetWidth;
  const H = canvas.offsetHeight;
  const lookup = buildCellLookup(cells);
  if (W === 0 || H === 0) return lookup;
  canvas.width = W;
  canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  const cellW = W / MAP_COLS;
  const cellH = H / MAP_ROWS;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);

  let maxLoad = 1;
  for (const cell of cells) {
    const load = totalAt(cell);
    if (load > maxLoad) maxLoad = load;
  }

  ctx.globalCompositeOperation = 'lighter';
  const statusPasses: ChunkStatus[][] = [
    ['completed', 'blocked'],
    ['reclaimed', 'assigned', 'FOUND'],
  ];
  for (const statuses of statusPasses) {
    for (const cell of cells) {
      const index = cell[0];
      let status: ChunkStatus | null = null;
      if (filter === 'all') {
        const candidate = dominantStatus(cell, statuses);
        if (candidate && statuses.includes(candidate)) status = candidate;
      } else {
        const layerStatus = filter as ChunkStatus;
        if (statuses.includes(layerStatus) && countAt(cell, layerStatus) > 0) status = layerStatus;
      }
      if (!status) continue;

      const col = index % MAP_COLS;
      const row = Math.floor(index / MAP_COLS);
      const cx = col * cellW + cellW / 2;
      const cy = row * cellH + cellH / 2;
      const loadNorm = Math.log(totalAt(cell) + 1) / Math.log(maxLoad + 1);
      const isEmphasized = status === 'assigned' || status === 'reclaimed' || status === 'FOUND';
      const radiusBase = isEmphasized
        ? Math.max(3.2, Math.min(4.2, W / 260))
        : Math.max(0.55, Math.min(1.1, W / 900));
      const jitter = hash32Mix(index, totalAt(cell), countAt(cell, status), isEmphasized ? 1 : 0);
      const radiusJitter = isEmphasized
        ? 0.92 + ((jitter & 0xff) / 255) * 0.18
        : 0.78 + ((jitter & 0xff) / 255) * 0.24;
      const r = isEmphasized
        ? Math.min(4.4, radiusBase * (0.88 + loadNorm * 0.3) * radiusJitter)
        : Math.min(1.35, radiusBase * (0.48 + loadNorm * 0.42) * radiusJitter);
      const dx = ((((jitter >>> 8) & 0xff) / 255) - 0.5) * cellW * 0.2;
      const dy = ((((jitter >>> 16) & 0xff) / 255) - 0.5) * cellH * 0.2;
      ctx.fillStyle = CHUNK_GLOW_COLORS[status];
      ctx.beginPath();
      ctx.arc(cx + dx, cy + dy, r, 0, Math.PI * 2);
      ctx.fill();
      if (totalAt(cell) >= (isEmphasized ? 4 : 8)) {
        ctx.beginPath();
        ctx.arc(cx + dx, cy + dy, Math.max(isEmphasized ? 0.9 : 0.35, r * (isEmphasized ? 0.38 : 0.3)), 0, Math.PI * 2);
        ctx.fill();
      }
    }
  }
  ctx.globalCompositeOperation = 'source-over';
  return lookup;
}

export function getHilbertXY(n: number, d: number): [number, number] {
  let x = 0;
  let y = 0;
  let t = Math.floor(d);
  for (let s = 1; s < n; s *= 2) {
    const rx = 1 & (t >> 1);
    const ry = 1 & (t ^ rx);
    if (ry === 0) {
      if (rx === 1) {
        x = s - 1 - x;
        y = s - 1 - y;
      }
      const temp = x;
      x = y;
      y = temp;
    }
    x += s * rx;
    y += s * ry;
    t = Math.floor(t / 4);
  }
  return [x, y];
}

export function getHilbertD(n: number, x: number, y: number): number {
  let d = 0;
  let xm = x;
  let ym = y;
  for (let s = Math.floor(n / 2); s > 0; s = Math.floor(s / 2)) {
    const rx = (xm & s) > 0 ? 1 : 0;
    const ry = (ym & s) > 0 ? 1 : 0;
    d += s * s * ((3 * rx) ^ ry);
    if (ry === 0) {
      if (rx === 1) {
        xm = s - 1 - xm;
        ym = s - 1 - ym;
      }
      const temp = xm;
      xm = ym;
      ym = temp;
    }
  }
  return d;
}

export function drawHilbert(
  canvas: HTMLCanvasElement,
  cells: VisualizationCell[],
  filter: string,
): CellLookup {
  const lookup = buildCellLookup(cells);
  const size = canvas.offsetWidth;
  if (size === 0) return lookup;
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext('2d')!;
  const cellSize = size / HILBERT_N;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, size, size);
  ctx.globalCompositeOperation = 'lighter';
  const pointR = Math.max(0.55, Math.min(1.2, size / 760));
  const order: ChunkStatus[] = ['reclaimed', 'assigned', 'completed', 'FOUND', 'blocked'];
  for (const status of order) {
    if (filter === 'blocked' && status !== 'blocked') continue;
    if (filter === 'native' && status === 'blocked') continue;
    ctx.fillStyle = CHUNK_GLOW_COLORS[status];
    for (const cell of cells) {
      const chosen = filter === 'blocked'
        ? (countAt(cell, 'blocked') > 0 ? 'blocked' : null)
        : filter === 'native'
          ? dominantStatus(cell, STATUS_ORDER_NATIVE)
          : dominantStatus(cell, STATUS_ORDER_ALL);
      if (chosen !== status) continue;
      const [hx, hy] = getHilbertXY(HILBERT_N, cell[0]);
      const px = hx * cellSize + cellSize / 2;
      const py = hy * cellSize + cellSize / 2;
      ctx.beginPath();
      ctx.arc(px, py, pointR, 0, Math.PI * 2);
      ctx.fill();
    }
  }
  ctx.globalCompositeOperation = 'source-over';
  return lookup;
}

function drawHistogram(canvas: HTMLCanvasElement, bins: number[], color: string, markerXRatio?: number): void {
  const W = canvas.offsetWidth;
  const H = canvas.offsetHeight;
  if (W === 0 || H === 0) return;
  canvas.width = W;
  canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);
  if (!bins.length) return;
  const maxCount = Math.max(...bins, 1);
  const barW = W / bins.length;
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = Math.floor((H * i) / 4) + 0.5;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(W, y);
    ctx.stroke();
  }
  if (markerXRatio != null) {
    const xRef = markerXRatio * W;
    ctx.strokeStyle = 'rgba(255,176,0,0.85)';
    ctx.beginPath();
    ctx.moveTo(xRef + 0.5, 0);
    ctx.lineTo(xRef + 0.5, H);
    ctx.stroke();
  }
  ctx.fillStyle = color;
  for (let i = 0; i < bins.length; i++) {
    const h = (bins[i] / maxCount) * (H - 8);
    ctx.fillRect(i * barW, H - h, Math.max(1, barW - 1), h);
  }
}

function drawAllocatorScatter(canvas: HTMLCanvasElement, sample: AllocatorScatterPoint[]): void {
  const W = canvas.offsetWidth;
  const H = canvas.offsetHeight;
  if (W === 0 || H === 0) return;
  canvas.width = W;
  canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);
  if (!sample.length) return;
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = Math.floor((H * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
    const x = Math.floor((W * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
  }
  const pointR = Math.max(0.6, Math.min(1.35, W / 760));
  for (const [xNorm, s, statusCode] of sample) {
    const x = xNorm * (W - 1);
    const y = (1 - s) * (H - 1);
    const status = statusCode === 0 ? 'completed'
      : statusCode === 1 ? 'assigned'
      : statusCode === 2 ? 'reclaimed'
      : statusCode === 3 ? 'FOUND'
      : 'blocked';
    ctx.fillStyle = CHUNK_GLOW_COLORS[status];
    ctx.beginPath();
    ctx.arc(x, y, pointR, 0, Math.PI * 2);
    ctx.fill();
  }
}

function drawAllocatorResidueMap(canvas: HTMLCanvasElement, sample: AllocatorScatterPoint[], totalCount: number): void {
  const W = canvas.offsetWidth;
  const H = canvas.offsetHeight;
  if (W === 0 || H === 0) return;
  canvas.width = W;
  canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);
  if (!sample.length || totalCount <= 0) return;
  const MOD = 1024;
  const rows = Math.min(128, Math.max(32, Math.floor(H / 2)));
  const cellW = W / MOD;
  const cellH = H / rows;
  ctx.strokeStyle = 'rgba(255,255,255,0.04)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = Math.floor((H * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
  }
  for (const [xNorm, s, statusCode] of sample) {
    const residue = Math.floor(s * MOD) % MOD;
    const row = Math.floor(xNorm * rows);
    const status = statusCode === 0 ? 'completed'
      : statusCode === 1 ? 'assigned'
      : statusCode === 2 ? 'reclaimed'
      : statusCode === 3 ? 'FOUND'
      : 'blocked';
    ctx.fillStyle = CHUNK_GLOW_COLORS[status];
    ctx.fillRect(residue * cellW, row * cellH, Math.max(1, cellW), Math.max(1, cellH));
  }
}

export function drawAllocatorDiagnostics(
  canvases: {
    scatter: HTMLCanvasElement;
    gap: HTMLCanvasElement;
    gapnorm: HTMLCanvasElement;
    residue: HTMLCanvasElement;
  },
  gapMetricsEl: HTMLElement,
  generation: AllocatorGenerationVisualization | null,
): void {
  if (!generation) {
    gapMetricsEl.innerHTML = 'Gap metrics: <span style="color:var(--text-muted)">not loaded</span>';
    drawAllocatorScatter(canvases.scatter, []);
    drawHistogram(canvases.gap, [], 'rgba(0,255,255,0.55)');
    drawHistogram(canvases.gapnorm, [], 'rgba(0,255,136,0.60)');
    drawAllocatorResidueMap(canvases.residue, [], 0);
    return;
  }

  drawAllocatorScatter(canvases.scatter, generation.scatter);
  drawHistogram(canvases.gap, generation.gap_histogram.bins, 'rgba(0,255,255,0.55)');
  drawHistogram(
    canvases.gapnorm,
    generation.norm_gap_histogram.bins,
    'rgba(0,255,136,0.60)',
    1 / generation.norm_gap_histogram.clip,
  );
  drawAllocatorResidueMap(canvases.residue, generation.scatter, generation.total_count);

  if (!generation.metrics) {
    gapMetricsEl.innerHTML = 'Gap metrics: <span style="color:var(--text-muted)">not enough data</span>';
    return;
  }
  const m = generation.metrics;
  gapMetricsEl.innerHTML =
    `Gap metrics: ` +
    `n <span style="color:var(--accent-cyan)">${formatIntegerDots(m.n)}</span> · ` +
    `mean <span style="color:var(--accent-amber)">${formatGapNumber(m.mean)}</span> · ` +
    `median <span style="color:var(--accent-amber)">${formatGapNumber(m.median)}</span> · ` +
    `p95 <span style="color:var(--accent-green)">${formatGapNumber(m.p95)}</span> · ` +
    `max <span style="color:var(--accent-green)">${formatGapNumber(m.max)}</span> · ` +
    `cv <span style="color:var(--accent-cyan)">${m.cv != null ? formatGapNumber(m.cv) : '—'}</span> · ` +
    `max/mean <span style="color:var(--accent-cyan)">${m.max_over_mean != null ? formatGapNumber(m.max_over_mean) : '—'}</span>`;
}

export function tooltipLinesForCell(cell: VisualizationCell | undefined): string[] {
  if (!cell) return [];
  const lines: string[] = [];
  for (const status of STATUS_ORDER_ALL) {
    const count = countAt(cell, status);
    if (count <= 0) continue;
    lines.push(`<span style="color:${CHUNK_COLORS[status]}">${STATUS_NAMES[status]}</span> · ${formatIntegerDots(count)}`);
  }
  return lines;
}

export function showTooltipLines(tooltipEl: HTMLElement, e: MouseEvent, lines: string[]): void {
  if (!lines.length) {
    tooltipEl.style.display = 'none';
    return;
  }
  tooltipEl.innerHTML = lines.join('<br>');
  tooltipEl.style.display = 'block';
  let tx = e.clientX + 14;
  let ty = e.clientY - tooltipEl.offsetHeight / 2;
  if (ty < 10) ty = 10;
  if (ty + tooltipEl.offsetHeight > window.innerHeight) ty = window.innerHeight - tooltipEl.offsetHeight - 10;
  tooltipEl.style.left = tx + 'px';
  tooltipEl.style.top = ty + 'px';
}
