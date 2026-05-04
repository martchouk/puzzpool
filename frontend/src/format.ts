import type { WorkerInfo, PuzzleInfo } from './types.ts';

// ── HTML escaping ─────────────────────────────────────────────────────────────

/** Escape untrusted strings before interpolating into innerHTML. */
export function esc(s: string | null | undefined): string {
  return (s ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

// ── Big integer formatting ────────────────────────────────────────────────────

/** Format a BigInt decimal string with dot-separated thousands groups. */
export function formatBigInt(value: string | number | bigint | null | undefined): string {
  const str = String(value ?? '').replace(/[^0-9]/g, '');
  if (!str) return '0';
  let out = '';
  for (let i = 0; i < str.length; i++) {
    if (i > 0 && (str.length - i) % 3 === 0) out += '.';
    out += str[i];
  }
  return out;
}

/** Format an integer (number, string, or null) with dot-separated thousands. */
export function formatIntegerDots(value: number | string | null | undefined): string {
  if (value == null || value === '') return '0';
  if (typeof value === 'number' && Number.isFinite(value)) {
    return formatBigInt(Math.trunc(value));
  }
  return formatBigInt(String(value));
}

// ── Hashrate ──────────────────────────────────────────────────────────────────

export function formatHashrate(h: number): string {
  if (h >= 1e9) return (h / 1e9).toFixed(2) + ' GH/s';
  if (h >= 1e6) return (h / 1e6).toFixed(2) + ' MH/s';
  if (h >= 1e3) return (h / 1e3).toFixed(2) + ' KH/s';
  return h.toFixed(0) + ' H/s';
}

// ── Time ──────────────────────────────────────────────────────────────────────

export function fmtUtc(s: string | null | undefined): string {
  if (!s) return '—';
  return new Date(s + 'Z').toLocaleString('de-DE', {
    day: '2-digit', month: '2-digit', year: 'numeric',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
    hour12: false,
  });
}

// ── Percentage formatting ─────────────────────────────────────────────────────

export function formatPrecisePercentage(pct: number): string {
  if (!Number.isFinite(pct) || pct <= 0) return '0 %';
  if (pct < 1e-9) return pct.toExponential(4) + ' %';
  let s = pct.toFixed(12);
  s = s.replace(/0+$/, '').replace(/\.$/, '');
  return s + ' %';
}

export function formatGapNumber(x: number): string {
  if (!Number.isFinite(x)) return '—';
  if (x >= 1) return x.toFixed(2);
  if (x >= 0.01) return x.toFixed(4);
  if (x >= 0.0001) return x.toFixed(6);
  return x.toExponential(3);
}

// ── Hex ranges ────────────────────────────────────────────────────────────────

export function trimHexRange(h: string | null | undefined): string {
  const s = (h ?? '').replace(/^0+/, '') || '0';
  return s.length > 16 ? s.slice(0, 8) + '-' + s.slice(-6) : s;
}

// ── vchunk run display ────────────────────────────────────────────────────────

export function formatRunShort(
  start: string | bigint | null | undefined,
  endExclusive: string | bigint | null | undefined,
): string {
  if (start == null || endExclusive == null) return '—';
  try {
    const s = BigInt(start);
    const e = BigInt(endExclusive);
    if (e <= s) return formatBigInt(String(s));
    return `${formatBigInt(String(s))} + ${formatBigInt(String(e - s - 1n))}`;
  } catch { return '—'; }
}

export function formatRunCompact(
  start: string | bigint | null | undefined,
  endExclusive: string | bigint | null | undefined,
): string {
  if (start == null || endExclusive == null) return '—';
  try {
    const s = BigInt(start);
    const e = BigInt(endExclusive);
    if (e <= s) return '—';
    return `${formatBigInt(String(s))} + ${formatBigInt(String(e - s))}`;
  } catch { return '—'; }
}

// ── ETA ───────────────────────────────────────────────────────────────────────

export function formatETA(totalKeys: string, keysCompleted: string, hashrate: number): string {
  if (hashrate <= 0) return '∞';
  const remaining = BigInt(totalKeys) - BigInt(keysCompleted) > 0n
    ? BigInt(totalKeys) - BigInt(keysCompleted)
    : 0n;
  if (remaining === 0n) return 'Complete';

  const remainingYears = remaining / BigInt(Math.round(hashrate * 365.25 * 86400));
  if (remainingYears > 999_999_999_999n) return '> 1 trillion years';
  if (remainingYears > 0n) return `~${formatBigInt(remainingYears.toString())} years`;

  const etaSec = Number(remaining) / hashrate;
  const MINUTE = 60, HOUR = 3600, DAY = 86400;
  if (etaSec >= DAY) return `~${Math.round(etaSec / DAY)} days`;
  if (etaSec >= HOUR) return `~${Math.round(etaSec / HOUR)} hours`;
  if (etaSec >= MINUTE) return `~${Math.round(etaSec / MINUTE)} min`;
  return '< 1 min';
}

// ── Worker progress ───────────────────────────────────────────────────────────

export function computeWorkerProgressPercent(worker: WorkerInfo): number | null {
  if (!worker.assigned_at || !worker.current_job_keys || !worker.hashrate) return null;
  const assignedMs = new Date(worker.assigned_at + 'Z').getTime();
  if (!Number.isFinite(assignedMs)) return null;
  const total = Number(worker.current_job_keys);
  const speed = Number(worker.hashrate);
  if (!(total > 0) || !(speed > 0)) return null;
  const elapsedSec = Math.max(0, (Date.now() - assignedMs) / 1000);
  const estimatedDone = elapsedSec * speed;
  return Math.max(0, Math.min(100, (estimatedDone / total) * 100));
}

export function renderWorkerProgress(worker: WorkerInfo): string {
  const pct = computeWorkerProgressPercent(worker);
  if (pct == null || !Number.isFinite(pct)) return '—';
  const clamped = Math.max(0, Math.min(100, pct));
  const rounded = Math.round(clamped);
  let color = 'var(--text-secondary)';
  if (worker.active) {
    color = 'var(--accent-cyan)';
  } else if (worker.assigned_here) {
    color = 'var(--accent-amber)';
  }
  return `
    <div class="progress-wrap">
      <div class="progress-bar">
        <div class="progress-bar-fill" style="width:${rounded}%;background:${color};"></div>
      </div>
      <span class="progress-label" style="color:${color};">${rounded}%</span>
    </div>`;
}

// ── Allocator ─────────────────────────────────────────────────────────────────

export function allocatorFriendlyName(strategy: string | null | undefined): string {
  if (strategy === 'virtual_random_chunks_v1') return 'Virtual random chunks';
  if (strategy === 'legacy_random_shards_v1') return 'Legacy random shards';
  return strategy ?? 'Unknown';
}

export function allocatorDiagnosticsHtml(puzzle: PuzzleInfo | null): string {
  if (!puzzle) return '';
  if (puzzle.alloc_strategy === 'virtual_random_chunks_v1') {
    const cursor = puzzle.alloc_cursor ?? '—';
    const total = puzzle.virtual_chunk_count ?? '—';
    const size = puzzle.virtual_chunk_size_keys ? formatBigInt(puzzle.virtual_chunk_size_keys) : '—';
    const stage = puzzle.bootstrap_stage ?? '—';
    return [
      `Allocator: <span style="color:var(--accent-cyan)">${allocatorFriendlyName(puzzle.alloc_strategy)}</span>`,
      `cursor: <span style="color:var(--accent-amber)">${formatIntegerDots(cursor)}</span>`,
      `virtual chunks: <span style="color:var(--accent-cyan)">${formatIntegerDots(total)}</span>`,
      `size: <span style="color:var(--accent-cyan)">${size}</span>`,
      `bootstrap stage: <span style="color:var(--accent-green)">${formatIntegerDots(stage)}</span>`,
    ].join(' · ');
  }
  return `Allocator: <span style="color:var(--accent-cyan)">${allocatorFriendlyName(puzzle.alloc_strategy)}</span>`;
}

// ── Table helpers ─────────────────────────────────────────────────────────────

export function emptyRow(colspan: number, msg: string): string {
  return `<tr><td colspan="${colspan}" class="td-empty">${msg}</td></tr>`;
}

// ── Statistics helpers ────────────────────────────────────────────────────────

export function percentileSorted(arr: number[], p: number): number | null {
  if (!arr || arr.length === 0) return null;
  if (p <= 0) return arr[0];
  if (p >= 1) return arr[arr.length - 1];
  const idx = (arr.length - 1) * p;
  const lo = Math.floor(idx);
  const hi = Math.ceil(idx);
  if (lo === hi) return arr[lo];
  const t = idx - lo;
  return arr[lo] * (1 - t) + arr[hi] * t;
}

export function quantileSorted(arr: number[], q: number): number | null {
  if (!arr.length) return null;
  const pos = (arr.length - 1) * q;
  const lo = Math.floor(pos);
  const hi = Math.ceil(pos);
  if (lo === hi) return arr[lo];
  const t = pos - lo;
  return arr[lo] * (1 - t) + arr[hi] * t;
}
