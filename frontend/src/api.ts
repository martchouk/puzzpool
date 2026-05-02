import type { StatsResponse } from './types.ts';

export async function fetchStats(puzzleId: number | null): Promise<StatsResponse> {
  const url = puzzleId !== null
    ? `/api/v1/stats?puzzle_id=${puzzleId}`
    : '/api/v1/stats';
  const res = await fetch(url);
  if (!res.ok) throw new Error(`Stats fetch failed: ${res.status}`);
  return res.json() as Promise<StatsResponse>;
}

export async function activatePuzzle(id: number, token: string): Promise<{ ok: boolean; error?: string; unauthorized?: boolean }> {
  const headers: Record<string, string> = { 'Content-Type': 'application/json' };
  if (token) headers['X-Admin-Token'] = token;
  const res = await fetch('/api/v1/admin/activate-puzzle', {
    method: 'POST',
    headers,
    body: JSON.stringify({ id }),
  });
  if (res.ok) return { ok: true };
  let msg = `Error ${res.status}`;
  try { const err = await res.json() as { error?: string }; msg = err.error ?? msg; } catch (_) { /* ignore */ }
  return { ok: false, error: msg, unauthorized: res.status === 401 };
}
