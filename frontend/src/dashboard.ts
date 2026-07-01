import type {
  AllocatorVisualizationResponse,
  HeatmapVisualizationResponse,
  HilbertVisualizationResponse,
  PuzzleListEntry,
  PuzzleStatusInfo,
} from './types.ts';
import {
  activatePuzzle,
  fetchAllocatorVisualization,
  fetchHeatmapVisualization,
  fetchHilbertVisualization,
  fetchStats,
} from './api.ts';
import {
  formatBigInt, formatIntegerDots, formatHashrate, fmtUtc, isRecentUtc,
  formatPrecisePercentage, trimHexRange, formatETA,
  renderWorkerProgress, allocatorDiagnosticsHtml, emptyRow, esc,
} from './format.ts';
import {
  MAP_COLS, MAP_ROWS, HILBERT_N,
  drawAllocatorDiagnostics,
  drawHeatmap,
  drawHilbert,
  type CellLookup,
  getHilbertD,
  showTooltipLines,
  tooltipLinesForCell,
} from './canvas.ts';

type BackendStatus = 'online' | 'offline';

let allocGenerationFilter = 'feistel';
let hmLayerFilter = 'completed';
let hilLayerFilter = 'native';
let heatmapLookup: CellLookup = new Map();
let hilbertLookup: CellLookup = new Map();
let heatmapVis: HeatmapVisualizationResponse | null = null;
let hilbertVis: HilbertVisualizationResponse | null = null;
let allocatorVis: AllocatorVisualizationResponse | null = null;
let pendingActivateId: number | null = null;
let lastPuzzles: (PuzzleListEntry & { active: boolean | number })[] = [];
let selectedId: number | null = null;
let stageSet = false;
let loadedVisualizationPuzzleId: number | null = null;
let currentVisRevision = 0;
let heatmapLoadedRevision = 0;
let allocatorLoadedRevision = 0;
let hilbertLoadedRevision = 0;
let heatmapLoading = false;
let allocatorLoading = false;
let hilbertLoading = false;

const tooltip = document.getElementById('ks-tooltip')!;
const hmCanvas = document.getElementById('heatmap-canvas') as HTMLCanvasElement;
const hilCanvas = document.getElementById('hilbert-canvas') as HTMLCanvasElement;
const allocCanvases = {
  scatter: document.getElementById('alloc-scatter-canvas') as HTMLCanvasElement,
  gap: document.getElementById('alloc-gap-canvas') as HTMLCanvasElement,
  gapnorm: document.getElementById('alloc-gapnorm-canvas') as HTMLCanvasElement,
  residue: document.getElementById('alloc-residue-canvas') as HTMLCanvasElement,
};
const gapMetricsEl = document.getElementById('alloc-gap-metrics')!;
const backendStatusEl = document.getElementById('backend-status')!;
const backendStatusIconEl = document.getElementById('backend-status-icon')!;
const backendStatusLabelEl = document.getElementById('backend-status-label')!;
const heatmapRefreshBtn = document.getElementById('heatmap-refresh-btn') as HTMLButtonElement;
const allocatorRefreshBtn = document.getElementById('allocator-refresh-btn') as HTMLButtonElement;
const hilbertRefreshBtn = document.getElementById('hilbert-refresh-btn') as HTMLButtonElement;

function svgEl(tag: string): SVGElement {
  return document.createElementNS('http://www.w3.org/2000/svg', tag);
}

function buildBackendStatusIcon(state: BackendStatus): SVGSVGElement {
  const svg = svgEl('svg') as SVGSVGElement;
  svg.setAttribute('class', 'status-icon');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('fill', 'none');
  svg.setAttribute('aria-hidden', 'true');

  const makePath = (d: string, cls: string): SVGPathElement => {
    const path = svgEl('path') as SVGPathElement;
    path.setAttribute('d', d);
    path.setAttribute('class', cls);
    return path;
  };

  const makeCircle = (cx: string, cy: string, r: string, cls: string): SVGCircleElement => {
    const circle = svgEl('circle') as SVGCircleElement;
    circle.setAttribute('cx', cx);
    circle.setAttribute('cy', cy);
    circle.setAttribute('r', r);
    circle.setAttribute('class', cls);
    return circle;
  };

  svg.appendChild(makePath('M2 8.82a15 15 0 0 1 20 0', 'status-icon-wave wave-1'));
  svg.appendChild(makePath('M5 12.859a10 10 0 0 1 14 0', 'status-icon-wave wave-2'));
  svg.appendChild(makePath('M8.5 16.429a5 5 0 0 1 7 0', 'status-icon-wave wave-3'));
  svg.appendChild(makeCircle('12', '20', '1.25', 'status-icon-core'));
  if (state === 'offline') svg.appendChild(makePath('M2 2l20 20', 'status-icon-slash'));
  return svg;
}

function setBackendStatus(state: BackendStatus): void {
  backendStatusEl.classList.toggle('is-online', state === 'online');
  backendStatusEl.classList.toggle('is-offline', state === 'offline');
  backendStatusLabelEl.textContent = state === 'online' ? 'ONLINE' : 'OFFLINE';
  backendStatusIconEl.replaceChildren(buildBackendStatusIcon(state));
}

function renderHeatmapPanel(): void {
  heatmapLookup = drawHeatmap(hmCanvas, heatmapVis?.cells ?? [], hmLayerFilter);
}

function renderHilbertPanel(): void {
  hilbertLookup = drawHilbert(hilCanvas, hilbertVis?.cells ?? [], hilLayerFilter);
}

function currentAllocatorGeneration() {
  return allocatorVis?.generations[allocGenerationFilter as 'all' | 'legacy' | 'affine' | 'feistel'] ?? null;
}

function renderAllocatorPanel(): void {
  drawAllocatorDiagnostics(allocCanvases, gapMetricsEl, currentAllocatorGeneration());
}

function redrawAll(): void {
  renderHeatmapPanel();
  renderAllocatorPanel();
  renderHilbertPanel();
}

function setRefreshButtonState(
  button: HTMLButtonElement,
  loading: boolean,
  loadedAt: string | null,
  stale: boolean,
): void {
  button.disabled = loading;
  button.classList.toggle('is-loading', loading);
  button.classList.toggle('is-stale', stale);
  const parts: string[] = [button.dataset.panelTitle ?? 'Refresh'];
  if (loadedAt) parts.push(`Loaded ${fmtUtc(loadedAt)}`);
  if (stale) parts.push('Visualization is stale');
  button.title = parts.join(' · ');
}

function syncRefreshButtons(): void {
  setRefreshButtonState(
    heatmapRefreshBtn,
    heatmapLoading,
    heatmapVis?.loaded_at ?? null,
    !!heatmapVis && heatmapLoadedRevision < currentVisRevision,
  );
  setRefreshButtonState(
    allocatorRefreshBtn,
    allocatorLoading,
    allocatorVis?.loaded_at ?? null,
    !!allocatorVis && allocatorLoadedRevision < currentVisRevision,
  );
  setRefreshButtonState(
    hilbertRefreshBtn,
    hilbertLoading,
    hilbertVis?.loaded_at ?? null,
    !!hilbertVis && hilbertLoadedRevision < currentVisRevision,
  );
}

function renderPuzzleStatus(status: PuzzleStatusInfo | null): void {
  const host = document.getElementById('puzzle-status-host')!;
  host.replaceChildren();
  if (!status) return;

  const badge = document.createElement('span');
  badge.className = `puzzle-status-chip is-${status.state}`;
  badge.textContent = (status.label || status.state).toLowerCase();

  const titleParts: string[] = [];
  if (status.checked_at) titleParts.push(`Checked ${fmtUtc(status.checked_at)}`);
  if (status.note) titleParts.push(status.note);
  if (titleParts.length > 0) badge.title = titleParts.join(' · ');

  if (status.link) {
    const link = document.createElement('a');
    link.className = 'puzzle-status-link';
    link.href = status.link;
    link.target = '_blank';
    link.rel = 'noreferrer noopener';
    link.appendChild(badge);
    host.appendChild(link);
    return;
  }

  host.appendChild(badge);
}

function applyStage(stage: string): void {
  if (stageSet) return;
  stageSet = true;
  if (stage !== 'TEST') return;
  const link = document.querySelector<HTMLLinkElement>('link[rel="icon"]');
  if (link) link.href = link.href.replace('%23F7931A', '%23ff3366');
  const label = document.getElementById('stage-label');
  if (label) label.style.display = 'inline';
}

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
  const token = (document.getElementById('modal-token-input') as HTMLInputElement).value.trim();
  const errEl = document.getElementById('modal-error')!;
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

function renderKeyspaceTabs(puzzles: (PuzzleListEntry & { active: boolean | number })[]): void {
  const normalized = (puzzles ?? []).map(p => ({
    ...p,
    active: p.active === true || p.active === 1,
  }));

  const poolActive = normalized.find(p => p.active) ?? normalized[0];
  const poolActiveId = poolActive ? poolActive.id : null;
  lastPuzzles = normalized;

  if (selectedId === null) selectedId = poolActiveId;
  if (selectedId !== null && !normalized.find(p => p.id === selectedId)) selectedId = poolActiveId;

  const nav = document.getElementById('ks-nav')!;
  const tabStrip = document.getElementById('ks-tab-strip')!;
  const togStrip = document.getElementById('ks-toggle-strip')!;
  const header = document.querySelector('.header')!;

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
    `<div class="ks-tab${p.id === selectedId ? ' active' : ''}" data-id="${p.id}">${esc(p.name)}</div>`,
  ).join('');

  const selIdx = normalized.findIndex(p => p.id === selectedId);
  const isOn = selectedId === poolActiveId;
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

function updateAllocatorGenerationFilterCounts(gens?: { legacy: number; affine: number; feistel: number }): void {
  const root = document.getElementById('alloc-generation-filter');
  if (!root) return;
  const counts = {
    all: (gens?.legacy ?? 0) + (gens?.affine ?? 0) + (gens?.feistel ?? 0),
    legacy: gens?.legacy ?? 0,
    affine: gens?.affine ?? 0,
    feistel: gens?.feistel ?? 0,
  };
  root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(btn => {
    const gen = btn.dataset.gen ?? 'all';
    const base = gen === 'all' ? 'All' : gen === 'legacy' ? 'Legacy' : gen === 'affine' ? 'Affine' : 'Feistel';
    btn.textContent = `${base} (${counts[gen as keyof typeof counts] ?? 0})`;
  });
}

function initAllocatorGenerationFilter(): void {
  const root = document.getElementById('alloc-generation-filter');
  if (!root) return;
  const syncButtons = (): void => {
    root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(b => {
      const isActive = b.dataset.gen === allocGenerationFilter;
      b.classList.toggle('active', isActive);
      b.setAttribute('aria-pressed', String(isActive));
    });
  };
  syncButtons();
  root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const next = btn.dataset.gen ?? 'all';
      if (next === allocGenerationFilter) return;
      allocGenerationFilter = next;
      syncButtons();
      renderAllocatorPanel();
    });
  });
}

function initHmLayerFilter(): void {
  const root = document.getElementById('hm-layer-filter');
  if (!root) return;
  const sync = (): void => root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(b => {
    const isActive = b.dataset.layer === hmLayerFilter;
    b.classList.toggle('active', isActive);
    b.setAttribute('aria-pressed', String(isActive));
  });
  sync();
  root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const next = btn.dataset.layer ?? 'completed';
      if (next === hmLayerFilter) return;
      hmLayerFilter = next;
      sync();
      renderHeatmapPanel();
    });
  });
}

function initHilLayerFilter(): void {
  const root = document.getElementById('hil-layer-filter');
  if (!root) return;
  const sync = (): void => root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(b => {
    const isActive = b.dataset.layer === hilLayerFilter;
    b.classList.toggle('active', isActive);
    b.setAttribute('aria-pressed', String(isActive));
  });
  sync();
  root.querySelectorAll<HTMLElement>('.alloc-filter-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const next = btn.dataset.layer ?? 'native';
      if (next === hilLayerFilter) return;
      hilLayerFilter = next;
      sync();
      renderHilbertPanel();
    });
  });
}

async function loadHeatmapVisualizationPanel(): Promise<void> {
  if (selectedId === null) return;
  heatmapLoading = true;
  syncRefreshButtons();
  try {
    heatmapVis = await fetchHeatmapVisualization(selectedId);
    heatmapLoadedRevision = currentVisRevision;
    renderHeatmapPanel();
  } finally {
    heatmapLoading = false;
    syncRefreshButtons();
  }
}

async function loadAllocatorVisualizationPanel(): Promise<void> {
  if (selectedId === null) return;
  allocatorLoading = true;
  syncRefreshButtons();
  try {
    allocatorVis = await fetchAllocatorVisualization(selectedId);
    allocatorLoadedRevision = currentVisRevision;
    renderAllocatorPanel();
  } finally {
    allocatorLoading = false;
    syncRefreshButtons();
  }
}

async function loadHilbertVisualizationPanel(): Promise<void> {
  if (selectedId === null) return;
  hilbertLoading = true;
  syncRefreshButtons();
  try {
    hilbertVis = await fetchHilbertVisualization(selectedId);
    hilbertLoadedRevision = currentVisRevision;
    renderHilbertPanel();
  } finally {
    hilbertLoading = false;
    syncRefreshButtons();
  }
}

async function loadAllVisualizationsForCurrentPuzzle(): Promise<void> {
  await Promise.all([
    loadHeatmapVisualizationPanel(),
    loadAllocatorVisualizationPanel(),
    loadHilbertVisualizationPanel(),
  ]);
}

async function updateDashboard(): Promise<void> {
  try {
    const data = await fetchStats(selectedId);
    setBackendStatus('online');
    currentVisRevision = data.vis_revision ?? 0;

    applyStage(data.stage);
    renderKeyspaceTabs(data.puzzles ?? []);

    document.getElementById('total-hashrate')!.textContent = formatHashrate(data.total_hashrate);
    document.getElementById('pool-id')!.textContent = data.puzzle ? 'Pool ID: ' + data.puzzle.id : '—';
    document.getElementById('active-workers')!.textContent = formatIntegerDots(data.active_workers_count);
    document.getElementById('inactive-workers')!.textContent = data.puzzle
      ? 'Inactive: ' + formatIntegerDots(data.inactive_workers_count)
      : '—';
    document.getElementById('completed-chunks')!.textContent = formatIntegerDots(data.completed_chunks);
    document.getElementById('reclaimed-chunks')!.textContent = data.puzzle
      ? 'Reclaimed: ' + formatIntegerDots(data.reclaimed_chunks)
      : '—';
    document.getElementById('completed-keys')!.textContent = formatBigInt(data.total_keys_completed);
    document.getElementById('found-keys')!.textContent = formatIntegerDots(data.finders.length);

    if (data.puzzle?.total_keys) {
      document.getElementById('puzzle-name')!.textContent = data.puzzle.name;
      renderPuzzleStatus(data.puzzle.status ?? null);
      document.getElementById('frontier')!.textContent =
        `0x${trimHexRange(data.puzzle.start_hex)} - 0x${trimHexRange(data.puzzle.end_hex)}`;

      const totalP = BigInt(data.puzzle.total_keys);
      const comp = BigInt(data.total_keys_completed);
      const vchunks = data.virtual_chunks ?? data.shards ?? { total: 0, started_vchunks: 0, completed_vchunks: 0, virtual_chunk_size_keys: null, blocked_vchunk_count: 0 };
      if (totalP > 0n && comp >= 0n) {
        const pctBig = (comp * 10n ** 18n) / totalP;
        let pctDisplay = formatPrecisePercentage(Number(pctBig) / 10 ** 16);
        const blockedVchunks = String(vchunks.blocked_vchunk_count ?? 0);
        const vchunkSize = vchunks.virtual_chunk_size_keys;
        if (blockedVchunks !== '0' && vchunkSize) {
          try {
            const blockedKeys = BigInt(blockedVchunks) * BigInt(vchunkSize);
            if (blockedKeys > 0n) {
              const pct2Big = ((comp + blockedKeys) * 10n ** 18n) / totalP;
              pctDisplay += ' / ' + formatPrecisePercentage(Number(pct2Big) / 10 ** 16);
            }
          } catch { /* ignore */ }
        }
        document.getElementById('completed-keys-pct')!.textContent = pctDisplay;
      }

      const eta = formatETA(data.puzzle.total_keys, data.total_keys_completed, data.total_hashrate);
      const a = (s: string) => `<span style="color:var(--accent-amber)">${s}</span>`;
      const c = (s: string) => `<span style="color:var(--accent-cyan)">${s}</span>`;
      const g = (s: string) => `<span style="color:var(--accent-green)">${s}</span>`;
      const w = (s: string) => `<span style="color:#fff">${s}</span>`;

      document.getElementById('puzzle-total-keys')!.innerHTML =
        `Keys total: ${a(formatBigInt(data.puzzle.total_keys))}`;
      if (vchunks.total !== 0 && vchunks.total !== '0') {
        const blockedCount = vchunks.blocked_vchunk_count ?? 0;
        document.getElementById('puzzle-vchunks')!.innerHTML =
          `Virtual chunks total: ${a(formatBigInt(String(vchunks.total)))} · started: ${c(formatBigInt(String(vchunks.started_vchunks)))} · completed: ${g(formatBigInt(String(vchunks.completed_vchunks)))} · Blocked: ${w(formatBigInt(String(blockedCount)))}`;
      } else {
        document.getElementById('puzzle-vchunks')!.innerHTML = '';
      }

      document.getElementById('puzzle-alloc')!.innerHTML = allocatorDiagnosticsHtml(data.puzzle);

      let etaLine = `ETA: ${a(eta)}`;
      try {
        const bvCount = String(vchunks.blocked_vchunk_count ?? 0);
        const bvSize = vchunks.virtual_chunk_size_keys;
        if (bvCount !== '0' && bvSize) {
          const blockedKeys = BigInt(bvCount) * BigInt(bvSize);
          if (blockedKeys > 0n) {
            const effComp = (BigInt(data.total_keys_completed) + blockedKeys).toString();
            etaLine += ` / ${w(formatETA(data.puzzle.total_keys, effComp, data.total_hashrate))}`;
          }
        }
      } catch { /* ignore */ }
      document.getElementById('puzzle-eta')!.innerHTML = etaLine;
    } else {
      renderPuzzleStatus(null);
      document.getElementById('puzzle-vchunks')!.textContent = '';
      document.getElementById('puzzle-alloc')!.textContent = '';
      document.getElementById('puzzle-eta')!.textContent = '';
    }

    const gens = data.alloc_generations ?? { legacy: 0, affine: 0, feistel: 0 };
    updateAllocatorGenerationFilterCounts(gens);
    document.getElementById('alloc-generation-summary')!.innerHTML =
      `Generation counts: ` +
      `legacy <span style="color:var(--accent-amber)">${formatIntegerDots(gens.legacy)}</span> · ` +
      `affine <span style="color:var(--accent-cyan)">${formatIntegerDots(gens.affine)}</span> · ` +
      `feistel <span style="color:var(--accent-green)">${formatIntegerDots(gens.feistel)}</span>`;

    document.getElementById('workers-section-title')!.textContent =
      `Visible Workers · Active: ${data.active_workers_count} · Inactive: ${data.inactive_workers_count}`;

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
          try {
            const s = BigInt(w.current_vchunk_run_start);
            const e = BigInt(w.current_vchunk_run_end);
            if (e > s) {
              runStart = formatBigInt(w.current_vchunk_run_start);
              runCount = formatBigInt(String(e - s));
            }
          } catch { /* leave as — */ }
        } else if (typeof w.current_vchunk_run === 'string') {
          const m = w.current_vchunk_run.match(/^(\d+)\.\.(\d+)$/);
          if (m) {
            try {
              runStart = formatBigInt(m[1]);
              runCount = formatBigInt(String(BigInt(m[2]) - BigInt(m[1]) + 1n));
            } catch { /* leave as — */ }
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

    const stbody = document.getElementById('score-list')!;
    if (!data.scores?.length) {
      stbody.innerHTML = emptyRow(5, 'No completed work yet');
    } else {
      stbody.innerHTML = data.scores.map((s, i) => {
        const lastSeenClass = isRecentUtc(s.last_seen) ? 'td-score-time' : 'td-score-time td-score-time-stale';
        return `<tr>
        <td class="td-rank">#${formatIntegerDots(i + 1)}</td>
        <td class="td-name">${esc(s.worker_name)}</td>
        <td class="td-chunks">${formatBigInt(s.total_keys)}</td>
        <td class="td-chunks">${formatIntegerDots(s.completed_chunks)}</td>
        <td class="${lastSeenClass}">${fmtUtc(s.last_seen)}</td>
      </tr>`;
      }).join('');
    }

    const ftbody = document.getElementById('finder-list')!;
    if (!data.finders?.length) {
      ftbody.innerHTML = emptyRow(6, 'No keys found yet');
    } else {
      ftbody.innerHTML = data.finders.map(f => `<tr>
        <td class="td-finder">${esc(f.worker_name)}</td>
        <td class="td-addr">${esc(f.found_address ?? 'Unknown')}</td>
        <td class="td-key">${f.vchunk_start != null ? formatBigInt(f.vchunk_start) : '—'}</td>
        <td class="td-key">${f.vchunk_end != null ? formatBigInt(String(BigInt(f.vchunk_end) - 1n)) : '—'}</td>
        <td class="td-key">${f.chunk_global != null ? '#' + formatIntegerDots(f.chunk_global) : '—'}</td>
        <td class="td-time">${fmtUtc(f.created_at)}</td>
      </tr>`).join('');
    }

    const currentPuzzleId = data.puzzle?.id ?? null;
    if (currentPuzzleId === null) {
      loadedVisualizationPuzzleId = null;
      heatmapVis = null;
      hilbertVis = null;
      allocatorVis = null;
      redrawAll();
      syncRefreshButtons();
    } else if (loadedVisualizationPuzzleId !== currentPuzzleId) {
      loadedVisualizationPuzzleId = currentPuzzleId;
      heatmapVis = null;
      hilbertVis = null;
      allocatorVis = null;
      heatmapLoadedRevision = 0;
      allocatorLoadedRevision = 0;
      hilbertLoadedRevision = 0;
      syncRefreshButtons();
      await loadAllVisualizationsForCurrentPuzzle();
    } else {
      syncRefreshButtons();
    }
  } catch (e) {
    setBackendStatus('offline');
    console.error(e);
  }
}

hmCanvas.addEventListener('mousemove', (e: MouseEvent) => {
  const rect = hmCanvas.getBoundingClientRect();
  const col = Math.floor(((e.clientX - rect.left) / rect.width) * MAP_COLS);
  const row = Math.floor(((e.clientY - rect.top) / rect.height) * MAP_ROWS);
  if (col < 0 || col >= MAP_COLS || row < 0 || row >= MAP_ROWS) {
    tooltip.style.display = 'none';
    return;
  }
  showTooltipLines(tooltip, e, tooltipLinesForCell(heatmapLookup.get(row * MAP_COLS + col)));
});
hmCanvas.addEventListener('mouseleave', () => { tooltip.style.display = 'none'; });

hilCanvas.addEventListener('mousemove', (e: MouseEvent) => {
  const rect = hilCanvas.getBoundingClientRect();
  const hx = Math.floor(((e.clientX - rect.left) / rect.width) * HILBERT_N);
  const hy = Math.floor(((e.clientY - rect.top) / rect.height) * HILBERT_N);
  if (hx < 0 || hx >= HILBERT_N || hy < 0 || hy >= HILBERT_N) {
    tooltip.style.display = 'none';
    return;
  }
  const index = getHilbertD(HILBERT_N, hx, hy);
  showTooltipLines(tooltip, e, tooltipLinesForCell(hilbertLookup.get(index)));
});
hilCanvas.addEventListener('mouseleave', () => { tooltip.style.display = 'none'; });

new ResizeObserver(() => {
  requestAnimationFrame(redrawAll);
}).observe(document.querySelector('.container')!);

heatmapRefreshBtn.addEventListener('click', () => { void loadHeatmapVisualizationPanel(); });
allocatorRefreshBtn.addEventListener('click', () => { void loadAllocatorVisualizationPanel(); });
hilbertRefreshBtn.addEventListener('click', () => { void loadHilbertVisualizationPanel(); });

requestAnimationFrame(() => {
  setBackendStatus('offline');
  initAllocatorGenerationFilter();
  initHmLayerFilter();
  initHilLayerFilter();
  syncRefreshButtons();
  void updateDashboard();
  setInterval(() => { void updateDashboard(); }, 5000);
});
