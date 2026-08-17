import { describe, expect, it } from 'vitest';
import {
  ACTIVATION_REFUSED_HINT,
  SIGNED_OUT,
  SIGNED_OUT_HINT,
  activationHint,
  authStateFromResponse,
  parseAuthMe,
  refusedActivationHint,
  safeAvatarUrl,
} from './auth.ts';

const signedInBody = {
  authenticated: true,
  login: 'octocat',
  avatar_url: 'https://avatars.githubusercontent.com/u/583231?v=4',
  is_admin: true,
};

describe('safeAvatarUrl', () => {
  it('accepts an absolute https avatar URL', () => {
    expect(safeAvatarUrl('https://avatars.githubusercontent.com/u/583231?v=4')).toBe(
      'https://avatars.githubusercontent.com/u/583231?v=4',
    );
  });

  it('rejects every scheme that could execute or downgrade', () => {
    expect(safeAvatarUrl('javascript:alert(1)')).toBeNull();
    expect(safeAvatarUrl('data:text/html;base64,PHNjcmlwdD4=')).toBeNull();
    expect(safeAvatarUrl('http://avatars.githubusercontent.com/u/1')).toBeNull();
    expect(safeAvatarUrl('vbscript:msgbox')).toBeNull();
  });

  it('rejects relative, empty and non-string values', () => {
    expect(safeAvatarUrl('/u/583231')).toBeNull();
    expect(safeAvatarUrl('')).toBeNull();
    expect(safeAvatarUrl(undefined)).toBeNull();
    expect(safeAvatarUrl(null)).toBeNull();
    expect(safeAvatarUrl(42)).toBeNull();
    expect(safeAvatarUrl({ href: 'https://example.test/a.png' })).toBeNull();
  });

  it('does not treat markup in the URL as markup', () => {
    // The value is only ever assigned to an <img src>, never parsed as HTML, and
    // URL normalization percent-encodes the angle brackets.
    expect(safeAvatarUrl('https://example.test/<script>.png')).toBe(
      'https://example.test/%3Cscript%3E.png',
    );
  });
});

describe('parseAuthMe', () => {
  it('reads a signed-in allow-listed identity', () => {
    expect(parseAuthMe(signedInBody)).toEqual({
      signedIn: true,
      isAdmin: true,
      login: 'octocat',
      avatarUrl: 'https://avatars.githubusercontent.com/u/583231?v=4',
    });
  });

  it('keeps a signed-in non-allow-listed identity signed in but not admin', () => {
    expect(parseAuthMe({ ...signedInBody, is_admin: false })).toMatchObject({
      signedIn: true,
      isAdmin: false,
      login: 'octocat',
    });
  });

  it('reads the signed-out body the backend returns for missing, malformed, bad-signature and expired cookies', () => {
    // The backend deliberately returns one identical body for all four cases.
    expect(parseAuthMe({ authenticated: false, is_admin: false })).toEqual(SIGNED_OUT);
  });

  it('refuses an authenticated body that cannot name the account', () => {
    expect(parseAuthMe({ authenticated: true, is_admin: true })).toEqual(SIGNED_OUT);
    expect(parseAuthMe({ authenticated: true, login: '', is_admin: true })).toEqual(SIGNED_OUT);
    expect(parseAuthMe({ authenticated: true, login: '   ', is_admin: true })).toEqual(SIGNED_OUT);
    expect(parseAuthMe({ authenticated: true, login: 7, is_admin: true })).toEqual(SIGNED_OUT);
  });

  it('does not accept a truthy non-true authenticated flag', () => {
    expect(parseAuthMe({ authenticated: 'true', login: 'octocat', is_admin: true })).toEqual(SIGNED_OUT);
    expect(parseAuthMe({ authenticated: 1, login: 'octocat', is_admin: true })).toEqual(SIGNED_OUT);
  });

  it('does not accept a truthy non-true admin flag', () => {
    expect(parseAuthMe({ ...signedInBody, is_admin: 'yes' }).isAdmin).toBe(false);
    expect(parseAuthMe({ ...signedInBody, is_admin: 1 }).isAdmin).toBe(false);
  });

  it('drops an unsafe avatar without losing the identity', () => {
    expect(parseAuthMe({ ...signedInBody, avatar_url: 'javascript:alert(1)' })).toEqual({
      signedIn: true,
      isAdmin: true,
      login: 'octocat',
      avatarUrl: null,
    });
  });

  it('survives a body that is not an object', () => {
    expect(parseAuthMe(null)).toEqual(SIGNED_OUT);
    expect(parseAuthMe(undefined)).toEqual(SIGNED_OUT);
    expect(parseAuthMe('authenticated')).toEqual(SIGNED_OUT);
    expect(parseAuthMe([signedInBody])).toEqual(SIGNED_OUT);
  });

  it('keeps a long login intact for the header to truncate visually', () => {
    const login = 'a'.repeat(39); // GitHub's maximum login length
    expect(parseAuthMe({ ...signedInBody, login }).login).toBe(login);
  });
});

describe('authStateFromResponse', () => {
  it('reports signed out for the 503 returned when session signing is unconfigured', () => {
    expect(
      authStateFromResponse(false, { error: 'auth_unavailable', reason: 'session_signing_secret_missing' }),
    ).toEqual(SIGNED_OUT);
  });

  it('reports signed out for any non-ok response, whatever the body claims', () => {
    expect(authStateFromResponse(false, signedInBody)).toEqual(SIGNED_OUT);
  });

  it('reads the identity from an ok response', () => {
    expect(authStateFromResponse(true, signedInBody).signedIn).toBe(true);
  });
});

describe('activationHint', () => {
  it('tells a signed-out visitor how to gain access instead of opening a modal', () => {
    expect(activationHint(SIGNED_OUT)).toBe(SIGNED_OUT_HINT);
  });

  it('explains why a signed-in non-allow-listed account cannot activate', () => {
    const hint = activationHint({ signedIn: true, isAdmin: false, login: 'octocat', avatarUrl: null });
    expect(hint).toContain('octocat');
    expect(hint).toContain('not on the admin allow-list');
  });

  it('permits the action for a signed-in allow-listed admin', () => {
    expect(activationHint({ signedIn: true, isAdmin: true, login: 'octocat', avatarUrl: null })).toBeNull();
  });
});

describe('refusedActivationHint', () => {
  // The states /api/v1/auth/me can report *after* an activation 401. The
  // allow-list is consulted per request (src/auth.cpp authorizeAdminRequest), so
  // a 401 does not imply the session ended.

  it('tells an expired session to sign in again', () => {
    expect(refusedActivationHint(SIGNED_OUT)).toBe(SIGNED_OUT_HINT);
  });

  it('names the account when a still-signed-in admin was removed from the allow-list', () => {
    // The backend answers this case with 401 while /auth/me still returns
    // authenticated: true, is_admin: false — so the identity stays in the
    // header and a "sign in with GitHub" hint would contradict it.
    const hint = refusedActivationHint({ signedIn: true, isAdmin: false, login: 'octocat', avatarUrl: null });
    expect(hint).toContain('octocat');
    expect(hint).toContain('not on the admin allow-list');
    expect(hint).not.toBe(SIGNED_OUT_HINT);
  });

  it('still explains the refusal when the refreshed state looks authorized', () => {
    // activationHint() returns null here, which would leave the visitor with a
    // closed overlay and no message at all. The refusal is a fact they saw.
    expect(refusedActivationHint({ signedIn: true, isAdmin: true, login: 'octocat', avatarUrl: null }))
      .toBe(ACTIVATION_REFUSED_HINT);
  });

  it('never returns null for any state, so the hint is never silent', () => {
    for (const signedIn of [true, false]) {
      for (const isAdmin of [true, false]) {
        expect(refusedActivationHint({ signedIn, isAdmin, login: 'octocat', avatarUrl: null })).not.toBe('');
      }
    }
  });
});
