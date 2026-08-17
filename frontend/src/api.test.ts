import { afterEach, describe, expect, it } from 'vitest';
import { ACTIVATION_NETWORK_ERROR, activatePuzzle, fetchAuthMe, logout } from './api.ts';
import { SIGNED_OUT } from './auth.ts';

// api.ts touches no DOM — only the global `fetch` — so the real transport code
// runs here against a stubbed network instead of a stand-in wrapper. Every
// assertion below awaits the returned promise, so a transport that rejects
// instead of resolving fails the test rather than passing quietly: removing a
// try/catch from api.ts turns the corresponding case red.

type FetchCall = [input: string, init: RequestInit | undefined];

const realFetch = globalThis.fetch;

function stubFetch(reply: (input: string, init: RequestInit | undefined) => Promise<Response>): FetchCall[] {
  const calls: FetchCall[] = [];
  globalThis.fetch = ((input: string, init?: RequestInit) => {
    calls.push([input, init]);
    return reply(input, init);
  }) as unknown as typeof fetch;
  return calls;
}

const jsonReply = (status: number, body: unknown): Promise<Response> =>
  Promise.resolve({
    ok: status >= 200 && status < 300,
    status,
    json: () => Promise.resolve(body),
  } as unknown as Response);

const unparsableReply = (status: number): Promise<Response> =>
  Promise.resolve({
    ok: status >= 200 && status < 300,
    status,
    json: () => Promise.reject(new SyntaxError('Unexpected end of JSON input')),
  } as unknown as Response);

const droppedConnection = (): Promise<Response> => Promise.reject(new TypeError('Failed to fetch'));

afterEach(() => {
  globalThis.fetch = realFetch;
});

describe('activatePuzzle', () => {
  it('posts the puzzle id with same-origin credentials and no admin secret', async () => {
    const calls = stubFetch(() => jsonReply(200, { ok: true }));

    await activatePuzzle(66);

    expect(calls).toHaveLength(1);
    const [url, init] = calls[0];
    expect(url).toBe('/api/v1/admin/activate-puzzle');
    expect(init?.method).toBe('POST');
    expect(init?.credentials).toBe('same-origin');
    expect(init?.body).toBe(JSON.stringify({ id: 66 }));
    expect(JSON.stringify(init?.headers)).not.toMatch(/admin-token/i);
  });

  it('reports success for a 2xx response', async () => {
    stubFetch(() => jsonReply(200, { ok: true }));
    expect(await activatePuzzle(66)).toEqual({ ok: true });
  });

  it('surfaces the server error message for a rejected activation', async () => {
    stubFetch(() => jsonReply(409, { error: 'puzzle already active' }));
    expect(await activatePuzzle(66)).toEqual({ ok: false, error: 'puzzle already active', unauthorized: false });
  });

  it('falls back to the status code when the error body cannot be read', async () => {
    stubFetch(() => unparsableReply(500));
    expect(await activatePuzzle(66)).toEqual({ ok: false, error: 'Error 500', unauthorized: false });
  });

  it('flags a 401 so the dashboard can discard the authenticated state', async () => {
    stubFetch(() => jsonReply(401, { error: 'unauthorized' }));
    expect(await activatePuzzle(66)).toMatchObject({ ok: false, unauthorized: true });
  });

  it('does not flag a 403 as unauthorized', async () => {
    stubFetch(() => jsonReply(403, { error: 'csrf_check_failed' }));
    expect(await activatePuzzle(66)).toMatchObject({ ok: false, unauthorized: false });
  });

  // AC4: a network error must leave a safe visible state with a message, never an
  // unhandled rejection that strands the confirmation overlay with no feedback.
  it('resolves to a showable error instead of rejecting when the connection drops', async () => {
    stubFetch(droppedConnection);

    const result = await activatePuzzle(66);

    expect(result.ok).toBe(false);
    expect(result.error).toBe(ACTIVATION_NETWORK_ERROR);
    expect(result.error).not.toBe('');
  });

  it('does not treat a dropped connection as an expired session', async () => {
    stubFetch(droppedConnection);
    // `unauthorized` drives discardAuthenticatedState(), so a network blip must
    // not sign a live admin session out of the UI.
    expect((await activatePuzzle(66)).unauthorized).toBeFalsy();
  });
});

describe('fetchAuthMe', () => {
  it('reads the identity from an authenticated response', async () => {
    const calls = stubFetch(() =>
      jsonReply(200, {
        authenticated: true,
        is_admin: true,
        login: 'octocat',
        avatar_url: 'https://avatars.githubusercontent.com/u/583231?v=4',
      }),
    );

    expect(await fetchAuthMe()).toEqual({
      signedIn: true,
      isAdmin: true,
      login: 'octocat',
      avatarUrl: 'https://avatars.githubusercontent.com/u/583231?v=4',
    });
    expect(calls[0][0]).toBe('/api/v1/auth/me');
    expect(calls[0][1]?.credentials).toBe('same-origin');
  });

  it('reports signed out for the 503 returned when session signing is unconfigured', async () => {
    stubFetch(() => jsonReply(503, { error: 'auth_unavailable' }));
    expect(await fetchAuthMe()).toEqual(SIGNED_OUT);
  });

  it('reports signed out for an unparsable body', async () => {
    stubFetch(() => unparsableReply(200));
    expect(await fetchAuthMe()).toEqual(SIGNED_OUT);
  });

  it('reports signed out instead of rejecting when the connection drops', async () => {
    stubFetch(droppedConnection);
    expect(await fetchAuthMe()).toEqual(SIGNED_OUT);
  });
});

describe('logout', () => {
  it('signs out through a same-origin POST', async () => {
    const calls = stubFetch(() => jsonReply(204, null));

    expect(await logout()).toBe(true);

    expect(calls[0][0]).toBe('/api/v1/auth/logout');
    expect(calls[0][1]?.method).toBe('POST');
    expect(calls[0][1]?.credentials).toBe('same-origin');
  });

  it('reports failure for a rejected logout so the caller re-reads the real state', async () => {
    stubFetch(() => jsonReply(403, { error: 'csrf_check_failed' }));
    expect(await logout()).toBe(false);
  });

  it('reports failure instead of rejecting when the connection drops', async () => {
    stubFetch(droppedConnection);
    expect(await logout()).toBe(false);
  });
});
