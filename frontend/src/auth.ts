import type { AuthState } from './types.ts';

// Authentication state derivation, deliberately free of any DOM or fetch
// dependency so every accept and reject path is directly unit-testable.
// The DOM wiring lives in dashboard.ts and the transport in api.ts.

// The optimistic default the dashboard paints before /api/v1/auth/me answers,
// and the state every failure collapses to.
export const SIGNED_OUT: AuthState = Object.freeze({
  signedIn: false,
  isAdmin: false,
  login: '',
  avatarUrl: null,
});

export const SIGNED_OUT_HINT = 'Sign in with GitHub to activate a keyspace.';

// The avatar URL is provider-derived, so it is treated as untrusted input even
// though the backend builds it from the immutable numeric GitHub id. Only an
// absolute https URL is ever assigned to an <img src>; anything else — a
// `javascript:` URL, a relative path, a non-string — yields null and the avatar
// is simply not rendered.
export function safeAvatarUrl(value: unknown): string | null {
  if (typeof value !== 'string' || value === '') return null;
  let parsed: URL;
  try {
    parsed = new URL(value);
  } catch {
    return null;
  }
  return parsed.protocol === 'https:' ? parsed.href : null;
}

// Normalizes a decoded /api/v1/auth/me body. A body that claims authentication
// but cannot name the account is not a usable signed-in state, so it is treated
// as signed out rather than rendered as a nameless identity.
export function parseAuthMe(payload: unknown): AuthState {
  if (payload === null || typeof payload !== 'object') return SIGNED_OUT;

  const body = payload as Record<string, unknown>;
  if (body.authenticated !== true) return SIGNED_OUT;

  const login = typeof body.login === 'string' ? body.login.trim() : '';
  if (login === '') return SIGNED_OUT;

  return {
    signedIn: true,
    isAdmin: body.is_admin === true,
    login,
    avatarUrl: safeAvatarUrl(body.avatar_url),
  };
}

// A non-2xx response — including the 503 returned when SESSION_SIGNING_SECRET is
// unset — carries no identity, so it is signed out rather than an error state
// the visitor has to dismiss.
export function authStateFromResponse(ok: boolean, payload: unknown): AuthState {
  return ok ? parseAuthMe(payload) : SIGNED_OUT;
}

// The non-blocking inline hint shown when an admin action is attempted without
// the authorization it needs. `null` means the action may proceed, so a modal
// that could only fail is never opened.
export function activationHint(state: AuthState): string | null {
  if (!state.signedIn) return SIGNED_OUT_HINT;
  if (!state.isAdmin) {
    return `Signed in as ${state.login}, which is not on the admin allow-list, so keyspace activation is unavailable.`;
  }
  return null;
}
