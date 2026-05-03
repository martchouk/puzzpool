import type { ChunkVis, PuzzleListEntry } from './types.ts';
import { fetchStats, activatePuzzle } from './api.ts';
import {
  formatBigInt, formatIntegerDots, formatHashrate, fmtUtc,
  formatPrecisePercentage, trimHexRange, formatETA,
  renderWorkerProgress, allocatorDiagnosticsHtml, emptyRow, esc,
} from './format.ts';
import {
  MAP_COLS, MAP_ROWS, HILBERT_N,
  draw1DBar, drawHeatmap, drawHilbert,
  drawAllocatorDiagnostics,
  getHilbertD,
  showTooltip, exportNormalizedGapMetrics,
} from './canvas.ts';

// ── Module-level state ────────────────────────────────────────────────────────

let chunksVis: ChunkVis[] = [];
let allocGenerationFilter = 'feistel';
let heatmapBuckets: ReturnType<typeof drawHeatmap> = [];
let pendingActivateId: number | null = null;
let lastPuzzles: (PuzzleListEntry & { active: boolean | number })[] = [];
let selectedId: number | null = null;
let stageSet = false;

// ── Stable DOM references ─────────────────────────────────────────────────────

const tooltip       = document.getElementById('ks-tooltip')!;
const ksCanvas      = document.getElementById('keyspace-canvas') as HTMLCanvasElement;
const hmCanvas      = document.getElementById('heatmap-canvas')  as HTMLCanvasElement;
const hilCanvas     = document.getElementById('hilbert-canvas')  as HTMLCanvasElement;
const allocCanvases = {
  scatter: document.getElementById('alloc-scatter-canvas') as HTMLCanvasElement,
  gap:     document.getElementById('alloc-gap-canvas')     as HTMLCanvasElement,
  gapnorm: document.getElementById('alloc-gapnorm-canvas') as HTMLCanvasElement,
  residue: document.getElementById('alloc-residue-canvas') as HTMLCanvasElement,
};
const gapMetricsEl = document.getElementById('alloc-gap-metrics')!;

// ── Helpers ───────────────────────────────────────────────────────────────────

function getFilteredChunks(): ChunkVis[] {
  if (allocGenerationFilter === 'all') return chunksVis;
  return chunksVis.filter(c => (c.g ?? 'legacy') === allocGenerationFilter);
}

function redrawAll(): void {
  draw1DBar(ksCanvas, chunksVis);
  heatmapBuckets = drawHeatmap(hmCanvas, chunksVis);
  drawAllocatorDiagnostics(allocCanvases, gapMetricsEl, getFilteredChunks());
  drawHilbert(hilCanvas, chunksVis);
}

// ── Stage indicator ───────────────────────────────────────────────────────────

function applyStage(stage: string): void {
  if (stageSet) return;
  stageSet = true;
  if (stage !== 'TEST') return;
  const link = document.querySelector<HTMLLinkElement>('link[rel="icon"]');
  if (link) link.href = link.href.replace('%23F7931A', '%23ff3366');
  const label = document.getElementById('stage-label');
  if (label) label.style.display = 'inline';
}

// ── Activate-puzzle modal ─────────────────────────────────────────────────────

function openActivateModal(id: number, name: string): void {
  pendingActivateId = id;
  document.getElementById('modal-puzzle-name')!.textContent = name;
  (document.getElementById('modal-token-input') as HTMLInputElement).value =
    sessionStorage.getItem('adminToken') ?? '';
  document.getElementById('modal-error')!.style.display = 'none';
  document.getElementById('activate-modal')!.style.display = 'flex';
}

document.getElementById('modal-cancel')!.addEventListener('click', () => {
  document.getElementById('activate-modal')!.style.display = 'none';
  pendingActivateId = null;
});

document.getElementById('modal-confirm')!.addEventListener('click', async () => {
  const token  = (document.getElementById('modal-token-input') as HTMLInputElement).value.trim();
  const errEl  = document.getElementById('modal-error')!;
  errEl.style.display = 'none';
  if (pendingActivateId === null) return;

  const result = await activatePuzzle(pendingActivateId, token);
  if (result.ok) {
    if (token) sessionStorage.setItem('adminToken', token);
    document.getElementById('activate-modal')!.style.display = 'none';
    selectedId = pendingActivateId;
    renderKeyspaceTabs(lastPuzzles.map(p => ({ ...p, active: p.id === pendingActivateId ? 1 : 0 })));
    pendingActivateId = null;
    void updateDashboard();
  } else {
    errEl.textContent = result.error ?? 'Error';
    errEl.style.display = 'block';
    if (result.unauthorized) sessionStorage.removeItem('adminToken');
  }
});

// ── Keyspace tabs ─────────────────────────────────────────────────────────────

function renderKeyspaceTabs(puzzles: (PuzzleListEntry & { active: boolean | number })[]): void {
  const normalized = (puzzles ?? []).map(p => ({
    ...p,
    active: p.active === true || p.active === 1,
  }));

  const poolActive   = normalized.find(p => p.active) ?? normalized[0];
  const poolActiveId = poolActive ? poolActive.id : null;
  lastPuzzles        = normalized;

  if (selectedId === null) selectedId = poolActiveId;
  if (selectedId !== null && !normalized.find(p => p.id === selectedId)) {
    selectedId = poolActiveId;
  }

  const nav      = document.getElementById('ks-nav')!;
  const tabStrip = document.getElementById('ks-tab-strip')!;
  const togStrip = document.getElementById('ks-toggle-strip')!;
  const header   = document.querySelector('.header')!;

  if (normalized.length <= 1) {
    nav.style.display = 'none';
    header.classList.remove('has-ks-tabs');
    tabStrip.innerHTML = '';
    togStrip.innerHTML = '';
    return;
  }

  nav.style.display = 'block';
  header.classList.add('has-ks-tabs');

  const gridCols = `repeat(${normalized.length}, 1fr)`;
  (tabStrip as HTMLElement).style.gridTemplateColumns = gridCols;
  (togStrip as HTMLElement).style.gridTemplateColumns = gridCols;

  tabStrip.innerHTML = normalized.map(p =>
    `<div class="ks-tab${p.id === selectedId ? ' active' : ''}" data-id="${p.id}">${esc(p.name)}</div>`
  ).join('');

  const selIdx  = normalized.findIndex(p => p.id === selectedId);
  const isOn    = selectedId === poolActiveId;
  const selName = normalized[selIdx]?.name ?? '';

  togStrip.innerHTML = `<div class="ks-toggle-cell${isOn ? '' : ' ks-toggle-inactive'}"
    style="grid-column:${selIdx + 1}" data-id="${selectedId}">
    <label class="ks-toggle">
      <input type="checkbox"${isOn ? ' checked' : ''} disabled>
      <span class="ks-toggle-slider"></span>
    </label>
  </div>`;

  tabStrip.querySelectorAll<HTMLElement>('.ks-tab:not(.active)').forEach(tab => {
    tab.addEventListener('click', () => {
      selectedId = Number(tab.dataset.id);
      renderKeyspaceTabs(lastPuzzles);
      void updateDashboard();
    });
  });

  togStrip.querySelectorAll<HTMLElement>('.ks-toggle-inactive').forEach(cell => {
    cell.addEventListener('click', () => openActivateModal(Number(cell.dataset.id), selName));
  });
}

// ── Allocator generation filter ───────────────────────────────────────────────

function updateAllocatorGenerationFilterCounts(): void {
  const root = document.getElementById('alloc-generation-filter');
  if (!root) return;
  const counts: Record<string, number> = { all: 0, legacy: 0, affine: 0, feistel: 0 };
  for (const c of chunksVis) {
    counts['all']++;
    const g = c.g ?? 'legacy';
    if (g in counts) counts[g]++;
  }
  root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(btn => {
    const gen  = btn.dataset.gen ?? 'all';
    const base = gen === 'all' ? 'All' : gen === 'legacy' ? 'Legacy' :
                 gen === 'affine' ? 'Affine' : gen === 'feistel' ? 'Feistel' : gen;
    btn.textContent = `${base} (${counts[gen] ?? 0})`;
  });
}

function initAllocatorGenerationFilter(): void {
  const root = document.getElementById('alloc-generation-filter');
  if (!root) return;

  const syncButtons = (): void => {
    root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(b => {
      b.classList.toggle('active', b.dataset.gen === allocGenerationFilter);
    });
  };

  syncButtons();

  root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const next = btn.dataset.gen ?? 'all';
      if (next === allocGenerationFilter) return;
      allocGenerationFilter = next;
      syncButtons();
      drawAllocatorDiagnostics(allocCanvases, gapMetricsEl, getFilteredChunks());
    });
  });
}

// ── Main dashboard update ─────────────────────────────────────────────────────

async function updateDashboard(): Promise<void> {
  try {
    const data = await fetchStats(selectedId);

    applyStage(data.stage);
    renderKeyspaceTabs(data.puzzles ?? []);

    document.getElementById('total-hashrate')!.textContent  = formatHashrate(data.total_hashrate);
    document.getElementById('pool-id')!.textContent         = data.puzzle ? 'Pool ID: ' + data.puzzle.id : '—';
    document.getElementById('active-workers')!.textContent  = formatIntegerDots(data.active_workers_count);
    document.getElementById('inactive-workers')!.textContent = data.puzzle
      ? 'Inactive: ' + formatIntegerDots(data.inactive_workers_count)
      : '—';
    document.getElementById('completed-chunks')!.textContent = formatIntegerDots(data.completed_chunks);
    document.getElementById('reclaimed-chunks')!.textContent = data.puzzle
      ? 'Reclaimed: ' + formatIntegerDots(data.reclaimed_chunks)
      : '—';
    document.getElementById('completed-keys')!.textContent = formatBigInt(data.total_keys_completed);
    document.getElementById('found-keys')!.textContent     = formatIntegerDots(data.finders.length);

    if (data.puzzle?.total_keys) {
      document.getElementById('puzzle-name')!.textContent = data.puzzle.name;
      document.getElementById('frontier')!.textContent =
        `0x${trimHexRange(data.puzzle.start_hex)} - 0x${trimHexRange(data.puzzle.end_hex)}`;

      const totalP = BigInt(data.puzzle.total_keys);
      const comp   = BigInt(data.total_keys_completed);
      if (totalP > 0n && comp >= 0n) {
        const pctBig = (comp * 10n ** 18n) / totalP;
        document.getElementById('completed-keys-pct')!.textContent =
          formatPrecisePercentage(Number(pctBig) / 10 ** 16);
      }

      const eta = formatETA(data.puzzle.total_keys, data.total_keys_completed, data.total_hashrate);
      const a = (s: string) => `<span style="color:var(--accent-amber)">${s}</span>`;
      const c = (s: string) => `<span style="color:var(--accent-cyan)">${s}</span>`;
      const g = (s: string) => `<span style="color:var(--accent-green)">${s}</span>`;

      document.getElementById('puzzle-total-keys')!.innerHTML =
        `Keys total: ${a(formatBigInt(data.puzzle.total_keys))}`;

      const vchunks = data.virtual_chunks ?? data.shards ?? { total: 0, started: 0, completed: 0 };
      document.getElementById('puzzle-vchunks')!.innerHTML = vchunks.total > 0
        ? `Virtual chunks total: ${a(formatIntegerDots(vchunks.total))} · started: ${c(formatIntegerDots(vchunks.started))} · completed: ${g(formatIntegerDots(vchunks.completed))}`
        : '';

      document.getElementById('puzzle-alloc')!.innerHTML = allocatorDiagnosticsHtml(data.puzzle);
      document.getElementById('puzzle-eta')!.innerHTML   = `ETA: ${a(eta)}`;
    } else {
      document.getElementById('puzzle-vchunks')!.textContent = '';
      document.getElementById('puzzle-alloc')!.textContent   = '';
      document.getElementById('puzzle-eta')!.textContent     = '';
    }

    chunksVis = data.chunks_vis ?? [];
    updateAllocatorGenerationFilterCounts();
    redrawAll();

    const ngm = exportNormalizedGapMetrics(getFilteredChunks());
    if (ngm) {
      console.log(
        `[Alloc norm-gap ${allocGenerationFilter}] ` +
        `n ${formatIntegerDots(ngm.n)} · ` +
        `mean ${ngm.mean.toFixed(3)} · median ${ngm.median.toFixed(3)} · ` +
        `p95 ${ngm.p95.toFixed(3)} · max ${ngm.max.toFixed(3)} · ` +
        `cv ${ngm.cv != null ? ngm.cv.toFixed(4) : '—'}`,
      );
    }

    const gens = data.alloc_generations ?? { legacy: 0, affine: 0, feistel: 0 };
    document.getElementById('alloc-generation-summary')!.innerHTML =
      `Generation counts: ` +
      `legacy <span style="color:var(--accent-amber)">${formatIntegerDots(gens.legacy)}</span> · ` +
      `affine <span style="color:var(--accent-cyan)">${formatIntegerDots(gens.affine)}</span> · ` +
      `feistel <span style="color:var(--accent-green)">${formatIntegerDots(gens.feistel)}</span>`;

    // Workers section title
    document.getElementById('workers-section-title')!.textContent =
      `Visible Workers · Active: ${data.active_workers_count} · Inactive: ${data.inactive_workers_count}`;

    // Workers table
    const tbody = document.getElementById('worker-list')!;
    if (!data.workers?.length) {
      tbody.innerHTML = emptyRow(9, 'No visible workers');
    } else {
      tbody.innerHTML = data.workers.map(w => {
        const dim = w.active ? '' : ' class="worker-dim"';
        const dot = `<span class="worker-dot ${w.active ? 'worker-dot-on' : 'worker-dot-off'}"></span>`;

        let runStart = '—';
        let runCount = '—';
        if (w.current_vchunk_run_start != null && w.current_vchunk_run_end != null) {
          const s = Number(w.current_vchunk_run_start);
          const e = Number(w.current_vchunk_run_end);
          if (Number.isFinite(s) && Number.isFinite(e) && e > s) {
            runStart = formatIntegerDots(s);
            runCount = formatIntegerDots(e - s);
          }
        } else if (typeof w.current_vchunk_run === 'string') {
          const m = w.current_vchunk_run.match(/^(\d+)\.\.(\d+)$/);
          if (m) {
            runStart = formatIntegerDots(Number(m[1]));
            runCount = formatIntegerDots(Number(m[2]) - Number(m[1]) + 1);
          }
        }

        return `<tr${dim}>
          <td>${dot}</td>
          <td class="td-name">${esc(w.name)}</td>
          <td class="td-mono">${esc(w.version ?? 'unknown')}</td>
          <td class="td-speed">${formatHashrate(w.hashrate)}</td>
          <td class="td-chunks">${w.current_chunk != null ? '#' + formatIntegerDots(w.current_chunk) : '—'}</td>
          <td class="td-chunks">${runStart}</td>
          <td class="td-chunks">${runCount}</td>
          <td class="td-progress">${renderWorkerProgress(w)}</td>
          <td class="td-time">${fmtUtc(w.last_seen)}</td>
        </tr>`;
      }).join('');
    }

    // Scores table
    const stbody = document.getElementById('score-list')!;
    if (!data.scores?.length) {
      stbody.innerHTML = emptyRow(4, 'No completed work yet');
    } else {
      stbody.innerHTML = data.scores.map((s, i) => `<tr>
        <td class="td-rank">#${formatIntegerDots(i + 1)}</td>
        <td class="td-name">${esc(s.worker_name)}</td>
        <td class="td-chunks">${formatBigInt(s.total_keys)}</td>
        <td class="td-chunks">${formatIntegerDots(s.completed_chunks)}</td>
      </tr>`).join('');
    }

    // Finders table
    const ftbody = document.getElementById('finder-list')!;
    if (!data.finders?.length) {
      ftbody.innerHTML = emptyRow(6, 'No keys found yet');
    } else {
      ftbody.innerHTML = data.finders.map(f => `<tr>
        <td class="td-finder">${esc(f.worker_name)}</td>
        <td class="td-addr">${esc(f.found_address ?? 'Unknown')}</td>
        <td class="td-key">${f.vchunk_start != null ? formatIntegerDots(f.vchunk_start) : '—'}</td>
        <td class="td-key">${f.vchunk_end  != null ? formatIntegerDots(Number(f.vchunk_end) - 1) : '—'}</td>
        <td class="td-key">${f.chunk_global != null ? '#' + formatIntegerDots(f.chunk_global) : '—'}</td>
        <td class="td-time">${fmtUtc(f.created_at)}</td>
      </tr>`).join('');
    }
  } catch (e) {
    console.error(e);
  }
}

// ── Canvas tooltip wiring ─────────────────────────────────────────────────────

ksCanvas.addEventListener('mousemove', (e: MouseEvent) => {
  const W  = ksCanvas.offsetWidth;
  const px = e.clientX - ksCanvas.getBoundingClientRect().left;
  const hits = chunksVis.filter(c => {
    const x = c.s * W;
    const w = Math.max(1, (c.e - c.s) * W);
    return px >= x - 1 && px <= x + w + 1;
  });
  showTooltip(tooltip, e, hits);
});
ksCanvas.addEventListener('mouseleave', () => { tooltip.style.display = 'none'; });

hmCanvas.addEventListener('mousemove', (e: MouseEvent) => {
  const rect = hmCanvas.getBoundingClientRect();
  const col = Math.floor(((e.clientX - rect.left) / rect.width)  * MAP_COLS);
  const row = Math.floor(((e.clientY - rect.top)  / rect.height) * MAP_ROWS);
  if (col < 0 || col >= MAP_COLS || row < 0 || row >= MAP_ROWS) {
    tooltip.style.display = 'none';
    return;
  }
  const bucket = heatmapBuckets[row * MAP_COLS + col];
  showTooltip(tooltip, e, bucket?.hits ?? []);
});
hmCanvas.addEventListener('mouseleave', () => { tooltip.style.display = 'none'; });

hilCanvas.addEventListener('mousemove', (e: MouseEvent) => {
  const rect = hilCanvas.getBoundingClientRect();
  const hx = Math.floor(((e.clientX - rect.left) / rect.width)  * HILBERT_N);
  const hy = Math.floor(((e.clientY - rect.top)  / rect.height) * HILBERT_N);
  if (hx < 0 || hx >= HILBERT_N || hy < 0 || hy >= HILBERT_N) return;
  const index      = getHilbertD(HILBERT_N, hx, hy);
  const totalCells = HILBERT_N * HILBERT_N;
  const cell_s     = index / totalCells;
  const cell_e     = (index + 1) / totalCells;
  showTooltip(tooltip, e, chunksVis.filter(c => c.s <= cell_e && c.e >= cell_s));
});
hilCanvas.addEventListener('mouseleave', () => { tooltip.style.display = 'none'; });

// ── Resize observer ───────────────────────────────────────────────────────────

new ResizeObserver(() => {
  requestAnimationFrame(redrawAll);
}).observe(document.querySelector('.container')!);

// ── Entry point ───────────────────────────────────────────────────────────────

requestAnimationFrame(() => {
  initAllocatorGenerationFilter();
  void updateDashboard();
  setInterval(() => { void updateDashboard(); }, 5000);
});
