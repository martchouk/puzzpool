import type { ChunkVis, ChunkStatus } from './types.ts';
import { formatIntegerDots, formatGapNumber, percentileSorted, quantileSorted, esc } from './format.ts';

// ── Chunk colour maps ─────────────────────────────────────────────────────────

export const CHUNK_COLORS: Record<ChunkStatus, string> = {
  'completed': '#0f8',
  'FOUND':     '#f36',
  'assigned':  '#0cf',
  'reclaimed': '#b80',
  'blocked':   '#fff',
};

export const CHUNK_GLOW_COLORS: Record<ChunkStatus, string> = {
  'completed': 'rgba(0, 255, 136, 0.4)',
  'FOUND':     'rgba(255, 51, 102, 1.0)',
  'assigned':  'rgba(0, 204, 255, 0.5)',
  'reclaimed': 'rgba(255, 187, 0, 0.3)',
  'blocked':   'rgba(255, 255, 255, 0.55)',
};

// ── Constants ─────────────────────────────────────────────────────────────────

export const MAP_COLS = 512;
export const MAP_ROWS = 128;
export const HILBERT_N = 256;

// ── 1D bar ────────────────────────────────────────────────────────────────────

export function draw1DBar(canvas: HTMLCanvasElement, chunks: ChunkVis[]): void {
  const W = canvas.offsetWidth, H = canvas.offsetHeight;
  if (W === 0 || H === 0) return;
  canvas.width = W; canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);
  if (chunks.length === 0) return;

  // Difference arrays (prefix-sum trick): O(chunks + W) regardless of chunk span.
  // Each array has W+1 slots so arr[e+1] is always in bounds.
  const dCompleted = new Int32Array(W + 1);
  const dAssigned  = new Int32Array(W + 1);
  const dReclaimed = new Int32Array(W + 1);
  const dFound     = new Int32Array(W + 1);
  const dBlocked   = new Int32Array(W + 1);

  for (const c of chunks) {
    const s = Math.max(0, Math.floor(c.s * W));
    const e = Math.min(W - 1, Math.floor(c.e * W));
    const arr = c.st === 'completed' ? dCompleted
              : c.st === 'assigned'  ? dAssigned
              : c.st === 'reclaimed' ? dReclaimed
              : c.st === 'FOUND'     ? dFound
              :                        dBlocked;
    arr[s]++;
    arr[e + 1]--;
  }

  // Scan prefix sums, compute colour per column, write direct to ImageData.
  const imgData = ctx.createImageData(W, H);
  const px = imgData.data;
  let completed = 0, assigned = 0, reclaimed = 0, found = 0, blocked = 0;

  for (let x = 0; x < W; x++) {
    completed += dCompleted[x];
    assigned  += dAssigned[x];
    reclaimed += dReclaimed[x];
    found     += dFound[x];
    blocked   += dBlocked[x];

    const total = completed + assigned + reclaimed + found + blocked;
    if (total === 0) continue;

    let r = 10, g = 10, b = 10;
    if (found > 0) {
      r = 255; g = 51; b = 102;                          // FOUND → red
    } else {
      const cf = completed / total;
      const af = assigned  / total;
      const bf = blocked   / total;
      const rf = reclaimed / total;
      // completed → green #0f8 (0, 255, 136)
      g += 245 * cf; b += 126 * cf;
      // assigned  → cyan  #0cf (0, 204, 255)
      g += 194 * af; b += 245 * af;
      // blocked   → violet #a070ff (160, 112, 255)
      r += 160 * bf; g += 112 * bf; b += 245 * bf;
      // reclaimed → amber #b80 (187, 136, 0)
      r += 177 * rf; g += 126 * rf;
    }

    const ri = Math.min(255, r) | 0;
    const gi = Math.min(255, g) | 0;
    const bi = Math.min(255, b) | 0;
    for (let y = 0; y < H; y++) {
      const i = (y * W + x) << 2;
      px[i] = ri; px[i + 1] = gi; px[i + 2] = bi; px[i + 3] = 255;
    }
  }
  ctx.putImageData(imgData, 0, 0);
}

// ── Heatmap ───────────────────────────────────────────────────────────────────

interface Bucket {
  hits: ChunkVis[];
  total: number;
  completed: number;
  assigned: number;
  reclaimed: number;
  FOUND: number;
  blocked: number;
  sizeJitter: number;
  offsetJitter: number;
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

function chunkHashPoint(c: ChunkVis, totalCells: number) {
  const id = Number.isFinite(c.id) ? c.id : 0;
  const sScaled = Number.isFinite(c.s) ? Math.floor(c.s * 0x7fffffff) : 0;
  const eScaled = Number.isFinite(c.e) ? Math.floor(c.e * 0x7fffffff) : 0;
  const h1 = hash32Mix(id, sScaled, eScaled, 0x1234abcd);
  const h2 = hash32Mix(id, eScaled, sScaled, 0x9e3779b9);
  const h3 = hash32Mix(sScaled, id, eScaled, 0x6d2b79f5);
  return { index: h1 % totalCells, sizeJitter: h2, offsetJitter: h3 };
}

export function buildHeatmapBuckets(chunks: ChunkVis[], totalCells: number): (Bucket | undefined)[] {
  const buckets: (Bucket | undefined)[] = new Array(totalCells);
  for (const c of chunks) {
    const hp = chunkHashPoint(c, totalCells);
    let bucket = buckets[hp.index];
    if (!bucket) {
      bucket = { hits: [], total: 0, completed: 0, assigned: 0, reclaimed: 0, FOUND: 0, blocked: 0,
                 sizeJitter: hp.sizeJitter, offsetJitter: hp.offsetJitter };
      buckets[hp.index] = bucket;
    }
    bucket.hits.push(c);
    bucket.total++;
    if (c.st === 'completed') bucket.completed++;
    else if (c.st === 'assigned') bucket.assigned++;
    else if (c.st === 'reclaimed') bucket.reclaimed++;
    else if (c.st === 'FOUND') bucket.FOUND++;
    else if (c.st === 'blocked') bucket.blocked++;
  }
  return buckets;
}

function bucketDominantStatus(bucket: Bucket | undefined): ChunkStatus | null {
  if (!bucket) return null;
  if (bucket.blocked > 0) return 'blocked';
  if (bucket.FOUND > 0) return 'FOUND';
  if (bucket.assigned > 0) return 'assigned';
  if (bucket.completed > 0) return 'completed';
  if (bucket.reclaimed > 0) return 'reclaimed';
  return null;
}

export function drawHeatmap(
  canvas: HTMLCanvasElement,
  chunks: ChunkVis[],
): (Bucket | undefined)[] {
  const W = canvas.offsetWidth, H = canvas.offsetHeight;
  if (W === 0 || H === 0) return [];
  canvas.width = W; canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  const totalCells = MAP_COLS * MAP_ROWS;
  const cellW = W / MAP_COLS, cellH = H / MAP_ROWS;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);

  const buckets = buildHeatmapBuckets(chunks, totalCells);

  let maxBucketLoad = 0;
  for (const bucket of buckets) {
    if (bucket && bucket.total > maxBucketLoad) maxBucketLoad = bucket.total;
  }
  if (maxBucketLoad < 1) maxBucketLoad = 1;

  ctx.globalCompositeOperation = 'lighter';
  for (let index = 0; index < totalCells; index++) {
    const bucket = buckets[index];
    const status = bucketDominantStatus(bucket);
    if (!status || !bucket) continue;
    const baseColor = CHUNK_GLOW_COLORS[status];
    const col = index % MAP_COLS;
    const row = Math.floor(index / MAP_COLS);
    const cx = col * cellW + cellW / 2;
    const cy = row * cellH + cellH / 2;
    const loadNorm = Math.log(bucket.total + 1) / Math.log(maxBucketLoad + 1);
    const radiusBase = Math.max(1, Math.min(2.5, W / 500));
    const radiusJitter = 0.85 + ((bucket.sizeJitter & 0xff) / 255) * 0.45;
    const r = Math.min(2.5, radiusBase * (0.65 + loadNorm * 0.9) * radiusJitter);
    const dx = ((((bucket.offsetJitter >>> 0) & 0xff) / 255) - 0.5) * cellW * 0.22;
    const dy = ((((bucket.offsetJitter >>> 8) & 0xff) / 255) - 0.5) * cellH * 0.22;
    ctx.fillStyle = baseColor;
    ctx.beginPath();
    ctx.arc(cx + dx, cy + dy, r, 0, Math.PI * 2);
    ctx.fill();
    if (bucket.total >= 3) {
      ctx.fillStyle = baseColor;
      ctx.beginPath();
      ctx.arc(cx + dx, cy + dy, r * 0.45, 0, Math.PI * 2);
      ctx.fill();
    }
  }
  ctx.globalCompositeOperation = 'source-over';
  return buckets;
}

// ── Hilbert curve ─────────────────────────────────────────────────────────────

export function getHilbertXY(n: number, d: number): [number, number] {
  let x = 0, y = 0, t = Math.floor(d);
  for (let s = 1; s < n; s *= 2) {
    const rx = 1 & (t >> 1);
    const ry = 1 & (t ^ rx);
    if (ry === 0) {
      if (rx === 1) { x = s - 1 - x; y = s - 1 - y; }
      const temp = x; x = y; y = temp;
    }
    x += s * rx;
    y += s * ry;
    t = Math.floor(t / 4);
  }
  return [x, y];
}

export function getHilbertD(n: number, x: number, y: number): number {
  let d = 0;
  let xm = x, ym = y;
  for (let s = Math.floor(n / 2); s > 0; s = Math.floor(s / 2)) {
    const rx = (xm & s) > 0 ? 1 : 0;
    const ry = (ym & s) > 0 ? 1 : 0;
    d += s * s * ((3 * rx) ^ ry);
    if (ry === 0) {
      if (rx === 1) { xm = s - 1 - xm; ym = s - 1 - ym; }
      const temp = xm; xm = ym; ym = temp;
    }
  }
  return d;
}

export function drawHilbert(canvas: HTMLCanvasElement, chunks: ChunkVis[]): void {
  const size = canvas.offsetWidth;
  if (size === 0) return;
  canvas.width = size; canvas.height = size;
  const ctx = canvas.getContext('2d')!;
  const totalCells = HILBERT_N * HILBERT_N;
  const cellSize = size / HILBERT_N;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, size, size);
  ctx.globalCompositeOperation = 'lighter';
  const order: ChunkStatus[] = ['reclaimed', 'assigned', 'completed', 'FOUND', 'blocked'];
  for (const status of order) {
    ctx.fillStyle = CHUNK_GLOW_COLORS[status];
    for (const c of chunks) {
      if (c.st !== status) continue;
      const distance = Math.floor(c.s * totalCells);
      const [hx, hy] = getHilbertXY(HILBERT_N, distance);
      if (hx < 0 || hx >= HILBERT_N || hy < 0 || hy >= HILBERT_N) continue;
      ctx.beginPath();
      ctx.arc(hx * cellSize + cellSize / 2, hy * cellSize + cellSize / 2,
              Math.max(1, Math.min(2.5, size / 500)), 0, Math.PI * 2);
      ctx.fill();
    }
  }
  ctx.globalCompositeOperation = 'source-over';
}

// ── Allocator diagnostics ─────────────────────────────────────────────────────

function getAllocatorChunks(chunks: ChunkVis[]): ChunkVis[] {
  return chunks
    .filter(c => Number.isFinite(c.id) && Number.isFinite(c.s) && Number.isFinite(c.e))
    .slice()
    .sort((a, b) => a.id - b.id);
}

function getAllocatorSortedStarts(chunks: ChunkVis[]): ChunkVis[] {
  return getAllocatorChunks(chunks).slice().sort((a, b) => a.s - b.s);
}

function getAllocatorNormalizedGaps(chunks: ChunkVis[]): number[] {
  const sorted = getAllocatorSortedStarts(chunks);
  if (sorted.length < 2) return [];
  const gaps: number[] = [];
  for (let i = 1; i < sorted.length; i++) {
    const gap = sorted[i].s - sorted[i - 1].s;
    if (Number.isFinite(gap) && gap >= 0) gaps.push(gap);
  }
  if (!gaps.length) return [];
  const mean = gaps.reduce((sum, g) => sum + g, 0) / gaps.length;
  if (!(mean > 0)) return [];
  return gaps.map(g => g / mean).filter(v => Number.isFinite(v) && v >= 0);
}

function computeNormalizedGapMetrics(chunks: ChunkVis[]) {
  const values = getAllocatorNormalizedGaps(chunks).slice().sort((a, b) => a - b);
  if (!values.length) return null;
  const n = values.length;
  const mean = values.reduce((s, v) => s + v, 0) / n;
  const median = quantileSorted(values, 0.5);
  const p95 = quantileSorted(values, 0.95);
  const max = values[n - 1];
  const variance = values.reduce((s, v) => s + (v - mean) * (v - mean), 0) / n;
  const sd = Math.sqrt(variance);
  const cv = mean > 0 ? sd / mean : null;
  return { n, mean, median: median ?? 0, p95: p95 ?? 0, max, cv };
}

export function drawAllocatorScatter(canvas: HTMLCanvasElement, chunks: ChunkVis[]): void {
  const W = canvas.offsetWidth, H = canvas.offsetHeight;
  if (W === 0 || H === 0) return;
  canvas.width = W; canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);
  const sorted = getAllocatorChunks(chunks);
  if (!sorted.length) return;
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = Math.floor((H * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
    const x = Math.floor((W * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
  }
  const n = sorted.length;
  const pointR = Math.max(1, Math.min(2.5, W / 500));
  for (let i = 0; i < n; i++) {
    const c = sorted[i];
    const x = n > 1 ? (i / (n - 1)) * (W - 1) : W / 2;
    const y = (1 - c.s) * (H - 1);
    ctx.fillStyle = CHUNK_GLOW_COLORS[c.st] ?? 'rgba(255,255,255,0.4)';
    ctx.beginPath();
    ctx.arc(x, y, pointR, 0, Math.PI * 2);
    ctx.fill();
  }
}

export function updateAllocatorGapMetrics(el: HTMLElement, chunks: ChunkVis[]): void {
  const sorted = getAllocatorChunks(chunks).slice().sort((a, b) => a.s - b.s);
  if (sorted.length < 2) {
    el.innerHTML = 'Gap metrics: <span style="color:var(--text-muted)">not enough data</span>';
    return;
  }
  const gaps: number[] = [];
  for (let i = 1; i < sorted.length; i++) {
    const gap = sorted[i].s - sorted[i - 1].s;
    if (Number.isFinite(gap) && gap >= 0) gaps.push(gap);
  }
  if (!gaps.length) {
    el.innerHTML = 'Gap metrics: <span style="color:var(--text-muted)">no valid gaps</span>';
    return;
  }
  gaps.sort((a, b) => a - b);
  const n = gaps.length;
  const mean = gaps.reduce((acc, v) => acc + v, 0) / n;
  const median = percentileSorted(gaps, 0.5);
  const p95 = percentileSorted(gaps, 0.95);
  const max = gaps[n - 1];
  let variance = 0;
  for (const g of gaps) { const d = g - mean; variance += d * d; }
  variance /= n;
  const cv = mean > 0 ? Math.sqrt(variance) / mean : null;
  const maxOverMean = mean > 0 ? max / mean : null;
  el.innerHTML =
    `Gap metrics: ` +
    `n <span style="color:var(--accent-cyan)">${formatIntegerDots(n)}</span> · ` +
    `mean <span style="color:var(--accent-amber)">${formatGapNumber(mean)}</span> · ` +
    `median <span style="color:var(--accent-amber)">${formatGapNumber(median ?? 0)}</span> · ` +
    `p95 <span style="color:var(--accent-green)">${formatGapNumber(p95 ?? 0)}</span> · ` +
    `max <span style="color:var(--accent-green)">${formatGapNumber(max)}</span> · ` +
    `cv <span style="color:var(--accent-cyan)">${cv != null ? formatGapNumber(cv) : '—'}</span> · ` +
    `max/mean <span style="color:var(--accent-cyan)">${maxOverMean != null ? formatGapNumber(maxOverMean) : '—'}</span>`;
}

export function drawAllocatorGapHistogram(canvas: HTMLCanvasElement, chunks: ChunkVis[]): void {
  const W = canvas.offsetWidth, H = canvas.offsetHeight;
  if (W === 0 || H === 0) return;
  canvas.width = W; canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);
  const sorted = getAllocatorChunks(chunks).slice().sort((a, b) => a.s - b.s);
  if (sorted.length < 2) return;
  const gaps: number[] = [];
  for (let i = 1; i < sorted.length; i++) {
    const gap = sorted[i].s - sorted[i - 1].s;
    if (Number.isFinite(gap) && gap >= 0) gaps.push(gap);
  }
  if (!gaps.length) return;
  const bins = Math.min(128, Math.max(32, Math.floor(W / 8)));
  const hist = new Array<number>(bins).fill(0);
  let maxGap = 0;
  for (const g of gaps) if (g > maxGap) maxGap = g;
  if (!(maxGap > 0)) maxGap = 1;
  for (const g of gaps) {
    let idx = Math.floor((g / maxGap) * (bins - 1));
    if (idx < 0) idx = 0;
    if (idx >= bins) idx = bins - 1;
    hist[idx]++;
  }
  const maxCount = Math.max(...hist, 1);
  const barW = W / bins;
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = Math.floor((H * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
  }
  ctx.fillStyle = 'rgba(0,255,255,0.55)';
  for (let i = 0; i < bins; i++) {
    const h = (hist[i] / maxCount) * (H - 8);
    ctx.fillRect(i * barW, H - h, Math.max(1, barW - 1), h);
  }
}

export function drawAllocatorNormalizedGapHistogram(canvas: HTMLCanvasElement, chunks: ChunkVis[]): void {
  const W = canvas.offsetWidth, H = canvas.offsetHeight;
  if (W === 0 || H === 0) return;
  canvas.width = W; canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);
  const values = getAllocatorNormalizedGaps(chunks);
  if (!values.length) return;
  const CLIP = 6;
  const bins = Math.min(96, Math.max(36, Math.floor(W / 10)));
  const hist = new Array<number>(bins).fill(0);
  for (const v of values) {
    const clipped = Math.min(v, CLIP);
    let idx = Math.floor((clipped / CLIP) * (bins - 1));
    if (idx < 0) idx = 0;
    if (idx >= bins) idx = bins - 1;
    hist[idx]++;
  }
  const maxCount = Math.max(...hist, 1);
  const barW = W / bins;
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = Math.floor((H * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
  }
  const xRef = (1 / CLIP) * W;
  ctx.strokeStyle = 'rgba(255,176,0,0.85)';
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(xRef + 0.5, 0); ctx.lineTo(xRef + 0.5, H); ctx.stroke();
  ctx.fillStyle = 'rgba(0,255,136,0.60)';
  for (let i = 0; i < bins; i++) {
    const h = (hist[i] / maxCount) * (H - 8);
    ctx.fillRect(i * barW, H - h, Math.max(1, barW - 1), h);
  }
  ctx.fillStyle = 'rgba(255,255,255,0.55)';
  ctx.font = '12px JetBrains Mono, monospace';
  ctx.fillText('1× mean', Math.min(W - 60, xRef + 6), 14);
  ctx.fillText(`clip ${CLIP}×`, W - 70, 14);
}

export function drawAllocatorResidueMap(canvas: HTMLCanvasElement, chunks: ChunkVis[]): void {
  const W = canvas.offsetWidth, H = canvas.offsetHeight;
  if (W === 0 || H === 0) return;
  canvas.width = W; canvas.height = H;
  const ctx = canvas.getContext('2d')!;
  ctx.fillStyle = '#0a0a0a';
  ctx.fillRect(0, 0, W, H);
  const sorted = getAllocatorChunks(chunks);
  if (!sorted.length) return;
  const MOD = 1024;
  const rows = Math.min(128, Math.max(32, Math.floor(H / 2)));
  const cellW = W / MOD, cellH = H / rows;
  ctx.strokeStyle = 'rgba(255,255,255,0.04)';
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = Math.floor((H * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
  }
  const n = sorted.length;
  for (let i = 0; i < n; i++) {
    const c = sorted[i];
    const residue = Math.floor(c.s * MOD) % MOD;
    const row = Math.floor((i / Math.max(1, n)) * rows);
    ctx.fillStyle = CHUNK_GLOW_COLORS[c.st] ?? 'rgba(255,255,255,0.5)';
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
  chunks: ChunkVis[],
): void {
  drawAllocatorScatter(canvases.scatter, chunks);
  drawAllocatorGapHistogram(canvases.gap, chunks);
  updateAllocatorGapMetrics(gapMetricsEl, chunks);
  drawAllocatorNormalizedGapHistogram(canvases.gapnorm, chunks);
  drawAllocatorResidueMap(canvases.residue, chunks);
}

export function exportNormalizedGapMetrics(chunks: ChunkVis[]) {
  return computeNormalizedGapMetrics(chunks);
}

// ── Tooltip ───────────────────────────────────────────────────────────────────

export function formatTooltipLine(c: ChunkVis): string {
  const status = c.st === 'FOUND'      ? '🔑 FOUND' :
                 c.st === 'assigned'   ? '⚡ In Progress' :
                 c.st === 'completed'  ? '✓ Done' :
                 c.st === 'blocked'    ? '⊘ Blocked' : '↩ Reclaimed';
  const idPart = c.st === 'blocked' ? '' : ` &nbsp;Chunk #${c.id}`;
  const wPart  = c.st === 'blocked' ? '' : ` &nbsp;<span style="color:#e0e0e0">${esc(c.w)}</span>`;
  return `<span style="color:${CHUNK_COLORS[c.st]}">${status}</span>${idPart}${wPart}`;
}

export function showTooltip(tooltipEl: HTMLElement, e: MouseEvent, hits: ChunkVis[]): void {
  if (!hits.length) { tooltipEl.style.display = 'none'; return; }
  const displayHits = hits.slice(0, 15);
  let html = displayHits.map(formatTooltipLine).join('<br>');
  if (hits.length > 15) {
    html += `<br><span style="color:#888;margin-top:5px;display:inline-block;">+ ${hits.length - 15} more chunks here</span>`;
  }
  tooltipEl.innerHTML = html;
  tooltipEl.style.display = 'block';
  let tx = e.clientX + 14;
  let ty = e.clientY - tooltipEl.offsetHeight / 2;
  if (ty < 10) ty = 10;
  if (ty + tooltipEl.offsetHeight > window.innerHeight) ty = window.innerHeight - tooltipEl.offsetHeight - 10;
  tooltipEl.style.left = tx + 'px';
  tooltipEl.style.top = ty + 'px';
}
