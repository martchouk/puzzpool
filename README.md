# puzzpool

[![CI](https://github.com/martchouk/puzzpool/actions/workflows/ci.yml/badge.svg)](https://github.com/martchouk/puzzpool/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Distributed Bitcoin keyspace search pool. Workers request chunks of a puzzle's keyspace,
scan them for matching private keys, and report results. A live dashboard visualises
progress in real time: stat cards (hashrate, ETA, keys completed), three canvas-based views
(1D progress bar, 2D heatmap, Hilbert curve projection), and live worker/score tables.

**Live instance:** https://puzzle.b58.de

---

## Quick Start (local)

**Prerequisites:** GCC 12+ or Clang 15+, CMake 3.20+, `libboost-dev`, `nlohmann-json3-dev`, Node.js 20+ (frontend build only)

```bash
# Ubuntu / Debian
sudo apt-get install -y cmake build-essential libboost-dev nlohmann-json3-dev libssl-dev

git clone --recurse-submodules https://github.com/martchouk/puzzpool.git
cd puzzpool
./update-deps.sh     # build C++ server + frontend dashboard
cp .env.example .env # configure (set ADMIN_TOKEN at minimum)
./build/bin/puzzpool # → server on http://127.0.0.1:8888
```

---

## Architecture

```
  Workers               puzzpool server              Browser
  ───────               ──────────────              ───────
  scanner  ──/work──▶   C++ / Crow         ◀──/stats──  Dashboard
           ◀──job_id──  (port 8888)                      (5 s poll)
  scanning
           ──/submit──▶      │
           ──/heartbeat──▶   ▼
                         SQLite (pool.db)

  Internet: HTTPS → Nginx → 127.0.0.1:8888
```

| Layer | Technology |
|-------|-----------|
| HTTP server | C++20, [Crow](https://crowcpp.org/) |
| Database | SQLite 3 via [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) |
| Frontend | TypeScript + Vite (built to `public/index.html`, single self-contained file) |
| Reverse proxy | Nginx (TLS + admin IP restriction) |
| Process manager | systemd |

See [docs/architecture.md](docs/architecture.md) for the full component diagram and
design decisions.

---

## Database Schema

Four tables — all created automatically on first run.

| Table | Purpose |
|-------|---------|
| `puzzles` | Puzzle definitions (keyspace range, allocator config, test chunk) |
| `workers` | Registered workers (name, hashrate, version, last seen) |
| `chunks` | Work units (assigned / completed / reclaimed / FOUND) |
| `findings` | Audit log of discovered private keys |

A legacy `sectors` table is retained for databases created with the old `legacy_random_shards_v1` allocator.

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
| `POST` | `/api/v1/admin/activate-puzzle` | Switch the active puzzle by ID |
| `POST` | `/api/v1/admin/set-test-chunk` | Set verification chunk for new workers |
| `GET`  | `/api/v1/admin/puzzles` | List all puzzles |
| `POST` | `/api/v1/admin/reclaim` | Force-reclaim timed-out chunks immediately |

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
| `ACTIVE_MINUTES` | `1.167` | Minutes a worker stays green after its last heartbeat. Capped at half of `TIMEOUT_MINUTES`. |
| `STAGE` | `PROD` | Deployment stage shown in the dashboard (`PROD` or `TEST`) |
| `ADMIN_TOKEN` | *(unset)* | If set, admin routes require `X-Admin-Token` header |
| `PERMUTATION_MODE` | `feistel` | Virtual chunk permutation algorithm: `feistel` (cycle-walking Feistel cipher) or `affine` (linear congruential). Recorded per-chunk as `alloc_generation`. |
| `KEYSPACE_<NAME>` | *(unset)* | Seed a keyspace on startup: `KEYSPACE_ALL_BTC=<start_hex>:<end_hex>`. Underscores in the variable name become spaces in the puzzle name. Multiple variables are supported. |

---

## Deployment

### Prerequisites

- Ubuntu/Debian server with GCC 12+, CMake 3.20+, `libboost-dev`, `nlohmann-json3-dev`, `libssl-dev`
- Node.js 20+ (only needed at build time to compile the TypeScript dashboard)
- Nginx, Certbot, systemd
- DNS A record pointing your domain to the server

### Steps

```bash
# 1. Install build dependencies
sudo apt-get install -y cmake build-essential libboost-dev nlohmann-json3-dev libssl-dev nodejs npm

# 2. Clone with submodules
git clone --recurse-submodules https://github.com/martchouk/puzzpool.git ~/git/puzzpool
cd ~/git/puzzpool

# 3. Configure
cp .env.example .env
# Edit .env — set ADMIN_TOKEN at minimum

# 4. Build C++ server + TypeScript dashboard
./update-deps.sh

# 5. Systemd service
sudo cp deploy/puzzpool.service /etc/systemd/system/
# Edit User= and WorkingDirectory= in the service file
sudo systemctl daemon-reload
sudo systemctl enable --now puzzpool

# 6. Nginx
sudo cp deploy/nginx.conf /etc/nginx/sites-available/puzzpool
# Edit server_name and SSL certificate paths
sudo ln -s /etc/nginx/sites-available/puzzpool /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx

# 7. TLS (Let's Encrypt)
sudo certbot --nginx -d your.domain.com
```

### Routine update (after `git pull`)

```bash
git pull origin main
./update.sh                      # incremental C++ rebuild + frontend rebuild
sudo systemctl restart puzzpool
```

See [deploy/nginx.conf](deploy/nginx.conf) and [deploy/puzzpool.service](deploy/puzzpool.service)
for annotated configuration files.

### Test instance (feature branch testing)

A separate test instance can run on the same server at a different subdomain and port.
See [deploy/nginx-test.conf](deploy/nginx-test.conf) and [deploy/puzzpool-test.service](deploy/puzzpool-test.service).
It uses port `8889` and `~/git/puzzpool.test/` as its working directory, so production is never affected.

---

## Security

- Admin routes are **IP-restricted** at the Nginx level by default (see `deploy/nginx.conf`)
- Set `ADMIN_TOKEN` for token-based admin authentication
- Workers are identified by name only — no passwords (by design for an open public puzzle)
- All SQL uses parameterised queries (no injection risk)
- Dashboard renders all user-supplied data via `textContent` (no XSS risk)

See [docs/security.md](docs/security.md) for the full threat model and recommendations.

---

## Testing

```bash
# Build and run unit tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# TypeScript type check + frontend build
npm run build --prefix frontend
```

See [docs/testing.md](docs/testing.md) for test scenarios and manual verification steps.

---

## Branching strategy

| Branch | Purpose |
|--------|---------|
| `main` | Production-ready code. Always matches what runs on prod. |
| `dev` | Active development. Test server always tracks this branch. |
| `feat/<topic>` | Optional isolation for large or risky changes — branch off `dev`, PR back into `dev`. |

```
feat/xyz  ●──●──●
                ↓ PR → dev
dev       ●─────●──●──●──●
                          ↓ PR → main (after tester approval)
main      ●───────────────●  ← tag v1.x
```

### Day-to-day workflow

- **Small changes** — commit directly to `dev`; test server picks them up on next deploy
- **Larger/risky changes** — open a `feat/` branch off `dev`, get it reviewed, merge to `dev`
- **Ship to prod** — open a PR from `dev → main`; merge after tester confirms; tag the release

### Tagging releases

```bash
git tag v1.x && git push origin v1.x
```

### Deployment

| Environment | Tracks | Update command |
|-------------|--------|----------------|
| Test server | `dev` | `git pull origin dev && ./update.sh` |
| Production  | `main` | `git pull origin main && ./update.sh` |

### Review process

- `main` is protected — requires 1 approval and passing CI
- The `/review-pr` Claude skill can generate a structured review draft
- If you changed API routes, update `docs/api.md` in the same commit
- Security issues: use GitHub's **private vulnerability reporting** (Security tab)

---

## License

MIT — see [LICENSE](LICENSE)
