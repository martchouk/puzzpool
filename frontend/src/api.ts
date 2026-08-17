import type {
  AllocatorVisualizationResponse,
  AuthState,
  HeatmapVisualizationResponse,
  HilbertVisualizationResponse,
  StatsResponse,
} from './types.ts';
import { SIGNED_OUT, authStateFromResponse } from './auth.ts';

export async function fetchStats(puzzleId: number | null): Promise<StatsResponse> {
  const url = puzzleId !== null
    ? `/api/v1/stats?puzzle_id=${puzzleId}`
    : '/api/v1/stats';
  const res = await fetch(url);
  if (!res.ok) throw new Error(`Stats fetch failed: ${res.status}`);
  return res.json() as Promise<StatsResponse>;
}

async function fetchVisualization<T>(path: string, puzzleId: number | null): Promise<T> {
  const url = puzzleId !== null ? `${path}?puzzle_id=${puzzleId}` : path;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`Visualization fetch failed: ${res.status}`);
  return res.json() as Promise<T>;
}

export function fetchHeatmapVisualization(puzzleId: number | null): Promise<HeatmapVisualizationResponse> {
  return fetchVisualization<HeatmapVisualizationResponse>('/api/v1/visualization/heatmap', puzzleId);
}

export function fetchHilbertVisualization(puzzleId: number | null): Promise<HilbertVisualizationResponse> {
  return fetchVisualization<HilbertVisualizationResponse>('/api/v1/visualization/hilbert', puzzleId);
}

export function fetchAllocatorVisualization(puzzleId: number | null): Promise<AllocatorVisualizationResponse> {
  return fetchVisualization<AllocatorVisualizationResponse>('/api/v1/visualization/allocator', puzzleId);
}

// ── Authentication ────────────────────────────────────────────────────────────

// Never throws: a 503, an unparsable body and a dropped connection are all
// simply "signed out", so a public visitor is never shown a failure they cannot
// act on. `credentials: 'same-origin'` sends the HttpOnly pp_session cookie,
// which JavaScript can never read itself.
export async function fetchAuthMe(): Promise<AuthState> {
  try {
    const res = await fetch('/api/v1/auth/me', { credentials: 'same-origin' });
    const payload: unknown = res.ok ? await res.json() : null;
    return authStateFromResponse(res.ok, payload);
  } catch (_) {
    return SIGNED_OUT;
  }
}

// The same-origin POST satisfies the Slice-A CSRF contract: the browser adds
// `Sec-Fetch-Site: same-origin`, and `Origin` as the fallback proof.
export async function logout(): Promise<boolean> {
  try {
    const res = await fetch('/api/v1/auth/logout', {
      method: 'POST',
      credentials: 'same-origin',
    });
    return res.ok;
  } catch (_) {
    return false;
  }
}

// ── Admin ─────────────────────────────────────────────────────────────────────

export const ACTIVATION_NETWORK_ERROR = 'Network error: the activation request did not reach the server.';

// Authorized by the session cookie alone. The browser holds no admin secret, so
// there is nothing for this call to read out of storage and attach.
//
// Never throws, like the two transports above. A dropped connection is reported
// as a failed activation the confirmation dialog can show and dismiss, not an
// unhandled rejection that leaves the overlay open with no message at all. It is
// deliberately not reported as `unauthorized`: the session is not known to be
// gone, so the signed-in UI must not be discarded over a network blip.
export async function activatePuzzle(id: number): Promise<{ ok: boolean; error?: string; unauthorized?: boolean }> {
  let res: Response;
  try {
    res = await fetch('/api/v1/admin/activate-puzzle', {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id }),
    });
  } catch (_) {
    return { ok: false, error: ACTIVATION_NETWORK_ERROR };
  }
  if (res.ok) return { ok: true };
  let msg = `Error ${res.status}`;
  try { const err = await res.json() as { error?: string }; msg = err.error ?? msg; } catch (_) { /* ignore */ }
  return { ok: false, error: msg, unauthorized: res.status === 401 };
}
