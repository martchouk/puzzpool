# puzzpool

[![CI](https://github.com/YOUR_USERNAME/puzzpool/actions/workflows/ci.yml/badge.svg)](https://github.com/YOUR_USERNAME/puzzpool/actions/workflows/ci.yml)
[![Node.js](https://img.shields.io/badge/node-%3E%3D18-brightgreen)](https://nodejs.org)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Distributed Bitcoin keyspace search pool. Workers request chunks of a puzzle's keyspace,
scan them for matching private keys, and report results. A live dashboard visualises
progress in real time using three canvas-based views: a 1D progress bar, a 2D heatmap,
and a Hilbert curve projection.

**Live instance:** https://puzzle.b58.de

---

## Quick Start (local)

```bash
# Prerequisites: Node.js >= 18
git clone https://github.com/YOUR_USERNAME/puzzpool.git
cd puzzpool
npm install
node server.js
# → server running on http://127.0.0.1:8888
# → open http://127.0.0.1:8888 in your browser
```

---

## Architecture

```
  Workers               puzzpool server              Browser
  ───────               ──────────────              ───────
  scanner  ──/work──▶   Express.js         ◀──/stats──  Dashboard
           ◀──job_id──  (port 8888)                      (3 s poll)
  scanning
           ──/submit──▶      │
           ──/heartbeat──▶   ▼
                         SQLite (pool.db)

  Internet: HTTPS → Nginx → 127.0.0.1:8888
```

| Layer | Technology |
|-------|-----------|
| Runtime | Node.js ≥ 18 |
| HTTP | Express 4 |
| Database | SQLite 3 (`better-sqlite3`, WAL mode) |
| Frontend | Vanilla HTML/CSS/JS (no build step) |
| Reverse proxy | Nginx (TLS + admin IP restriction) |
| Process manager | systemd |

See [docs/architecture.md](docs/architecture.md) for the full component diagram and
design decisions.

---

## Database Schema

Four tables — all created automatically on first run.

| Table | Purpose |
|-------|---------|
| `puzzles` | Puzzle definitions (keyspace range, active flag, test chunk) |
| `workers` | Registered workers (name, hashrate, last seen) |
| `chunks` | Work units (assigned / completed / reclaimed / FOUND) |
| `findings` | Audit log of discovered private keys |

See [docs/database.md](docs/database.md) for full schema, ER diagram, and migration notes.

---

## API Reference

See [docs/api.md](docs/api.md) for full request/response schemas.

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/api/v1/work` | Request next keyspace chunk |
| `POST` | `/api/v1/submit` | Report chunk done or key found |
| `POST` | `/api/v1/heartbeat` | Reset reclaim timer for long jobs |
| `GET`  | `/api/v1/stats` | Dashboard data |
| `POST` | `/api/v1/admin/set-puzzle` | Create / activate a puzzle |
| `POST` | `/api/v1/admin/set-test-chunk` | Set verification chunk for new workers |
| `GET`  | `/api/v1/admin/puzzles` | List all puzzles |

---

## Configuration

All tunables are read from environment variables. Copy `.env.example` to configure:

```bash
cp .env.example .env
```

| Variable | Default | Description |
|----------|---------|-------------|
| `PORT` | `8888` | Bind port (localhost only) |
| `DB_PATH` | `pool.db` | SQLite database file path |
| `TARGET_MINUTES` | `5` | Expected time to complete one chunk |
| `TIMEOUT_MINUTES` | `15` | Minutes before an inactive chunk is reclaimed |
| `ADMIN_TOKEN` | *(unset)* | If set, admin routes require `X-Admin-Token` header |

---

## Deployment

### Prerequisites

- Ubuntu/Debian server with Node.js 18+, Nginx, Certbot, systemd
- DNS A record pointing your domain to the server

### Steps

```bash
# 1. Clone
git clone https://github.com/YOUR_USERNAME/puzzpool.git ~/git/puzzpool
cd ~/git/puzzpool && npm ci --production

# 2. Configure
cp .env.example .env
# Edit .env — set ADMIN_TOKEN at minimum

# 3. Systemd service
sudo cp deploy/puzzpool.service /etc/systemd/system/
# Edit the User= and WorkingDirectory= lines in the service file
sudo systemctl daemon-reload
sudo systemctl enable --now puzzpool

# 4. Nginx
sudo cp deploy/nginx.conf /etc/nginx/sites-available/puzzpool
# Edit server_name and SSL certificate paths
sudo ln -s /etc/nginx/sites-available/puzzpool /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx

# 5. TLS (Let's Encrypt)
sudo certbot --nginx -d your.domain.com
```

See [deploy/nginx.conf](deploy/nginx.conf) and [deploy/puzzpool.service](deploy/puzzpool.service)
for annotated configuration files.

---

## Security

- Admin routes are **IP-restricted** at the Nginx level by default (see `deploy/nginx.conf`)
- Set `ADMIN_TOKEN` for token-based admin authentication
- Workers are identified by name only — no passwords (by design for an open public puzzle)
- All SQL uses parameterised queries (no injection risk)
- Frontend uses only `textContent` (no XSS risk)

See [docs/security.md](docs/security.md) for the full threat model and recommendations.

---

## Testing

```bash
npm test          # Jest integration tests (in-memory SQLite, no server needed)
npm run lint      # ESLint
bash test.sh      # Manual curl tests against live or local server
```

See [docs/testing.md](docs/testing.md) for test scenarios and manual verification steps.

---

## Contributing

### Branch naming

| Prefix | Use |
|--------|-----|
| `feat/` | New features |
| `fix/` | Bug fixes |
| `docs/` | Documentation only |

### Workflow

1. Fork the repo and create a branch from `main`
2. Make your changes — add tests for new behaviour
3. Run `npm test` and `npm run lint` — both must pass
4. If you changed API routes, update `docs/api.md`
5. Open a Pull Request — fill in the PR template checklist
6. CI runs automatically; a maintainer will review within a few days
7. Approved PRs are squash-merged into `main` and auto-deployed

### Review process

- `main` is protected — requires 1 approval and passing CI
- The `/review-pr` Claude skill can generate a structured review draft
- All review conversations must be resolved before merge
- Security issues: use GitHub's **private vulnerability reporting** (Security tab)

---

## License

MIT — see [LICENSE](LICENSE)
