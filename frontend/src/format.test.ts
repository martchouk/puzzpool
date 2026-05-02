import { describe, it, expect } from 'vitest';
import { esc } from './format.ts';

describe('esc()', () => {
  it('returns empty string for null', () => expect(esc(null)).toBe(''));
  it('returns empty string for undefined', () => expect(esc(undefined)).toBe(''));
  it('returns empty string for empty string', () => expect(esc('')).toBe(''));
  it('passes through plain text unchanged', () => expect(esc('hello world')).toBe('hello world'));

  it('escapes &', () => expect(esc('a&b')).toBe('a&amp;b'));
  it('escapes <', () => expect(esc('<tag>')).toBe('&lt;tag&gt;'));
  it('escapes >', () => expect(esc('a>b')).toBe('a&gt;b'));
  it('escapes double quote', () => expect(esc('"quoted"')).toBe('&quot;quoted&quot;'));
  it("escapes single quote", () => expect(esc("it's")).toBe('it&#39;s'));

  it('escapes a typical XSS script payload', () =>
    expect(esc('<script>alert(1)</script>')).toBe(
      '&lt;script&gt;alert(1)&lt;/script&gt;'
    ));

  it('escapes a worker name containing HTML', () =>
    expect(esc('<worker"name>')).toBe('&lt;worker&quot;name&gt;'));

  it('escapes found_address with all special characters', () =>
    expect(esc(`<img src=x onerror="alert('xss')">`)).toBe(
      `&lt;img src=x onerror=&quot;alert(&#39;xss&#39;)&quot;&gt;`
    ));

  it('escapes multiple & in sequence', () =>
    expect(esc('a&b&c')).toBe('a&amp;b&amp;c'));
});
