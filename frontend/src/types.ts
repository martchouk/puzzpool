// ── Numeric representation policy ────────────────────────────────────────────
//
// Values from the backend that may exceed Number.MAX_SAFE_INTEGER (2^53-1)
// are kept as `string` at the API boundary and formatted without conversion.
//
// | Backend field               | Frontend type | Notes                            |
// |-----------------------------|---------------|----------------------------------|
// | total_keys_completed        | string        | BigInt decimal — display only    |
// | total_keys (puzzle)         | string        | BigInt decimal — display only    |
// | virtual_chunk_size_keys     | string | null  | BigInt decimal — display only   |
// | total_keys (score entry)    | string        | BigInt decimal — display only    |
// | current_job_keys            | string | null  | BigInt decimal — display only   |
// | start_hex / end_hex         | string        | 256-bit hex — display only       |
// | Counts (completed_chunks…)  | number        | Safe — expected < 2^53           |
// | vchunk_start / _end         | string | null  | BigInt decimal — may exceed 2^53 |
// | vchunk_run_start / _end     | string | null  | BigInt decimal — may exceed 2^53 |
// | alloc_cursor                | string        | BigInt decimal — may exceed 2^53 |
// | virtual_chunk_count         | string | null  | BigInt decimal — may exceed 2^53 |
// | Canvas positions (s, e)     | number        | Normalised float [0, 1]          |
// | Timestamps                  | string | null  | ISO 8601 UTC string              |
// | hashrate                    | number        | keys/second — safe as float      |

// ── Chunk visualisation entry (chunks_vis array) ──────────────────────────────

export type ChunkStatus = 'completed' | 'FOUND' | 'assigned' | 'reclaimed';
export type AllocGeneration = 'legacy' | 'affine' | 'feistel' | 'test' | null;

export interface ChunkVis {
  id: number;
  st: ChunkStatus;
  w: string | null;     // worker name
  g: AllocGeneration;   // alloc_generation
  s: number;            // normalised start position [0, 1]
  e: number;            // normalised end position   [0, 1]
}

// ── Worker ────────────────────────────────────────────────────────────────────

export interface WorkerInfo {
  name: string;
  hashrate: number;
  last_seen: string | null;
  version: string | null;
  min_chunk_keys: string | null;
  chunk_quantum_keys: string | null;
  fresh: boolean;
  assigned_here: boolean;
  active: boolean;
  // Present only when the worker has an active chunk assignment
  current_chunk: number | null;
  current_vchunk_run: string | null;       // "startIndex..endIndex" string
  current_vchunk_run_start: string | null;
  current_vchunk_run_end: string | null;
  assigned_at: string | null;
  heartbeat_at: string | null;
  current_job_start_hex: string | null;
  current_job_end_hex: string | null;
  current_job_keys: string | null;         // BigInt decimal string
}

// ── Score / finder ────────────────────────────────────────────────────────────

export interface ScoreEntry {
  worker_name: string;
  completed_chunks: number;
  total_keys: string;   // BigInt decimal string
}

export interface FinderEntry {
  worker_name: string;
  found_key: string;
  found_address: string | null;
  created_at: string | null;
  chunk_global: number | null;
  vchunk_start: string | null;
  vchunk_end: string | null;
}

// ── Puzzle ────────────────────────────────────────────────────────────────────

export interface PuzzleInfo {
  id: number;
  name: string;
  start_hex: string;
  end_hex: string;
  active: boolean;
  test_chunk: { start_hex: string; end_hex: string } | null;
  alloc_strategy: string;
  alloc_cursor: string;                    // BigInt decimal string
  virtual_chunk_size_keys: string | null;  // BigInt decimal string
  virtual_chunk_count: string | null;      // BigInt decimal string
  bootstrap_stage: number;
  total_keys: string;                       // BigInt decimal string
}

// active field from listPuzzles() returns 0/1 integer, not boolean
export interface PuzzleListEntry {
  id: number;
  name: string;
  active: boolean | number;
}

// ── Virtual chunk / shard counts ─────────────────────────────────────────────

export interface VirtualChunks {
  total: string | number;            // BigInt decimal string (large domains) or 0
  started_vchunks: string | number;  // virtual chunk spans covered
  completed_vchunks: string | number;
  virtual_chunk_size_keys: string | null;
}

// ── Alloc generation counts ───────────────────────────────────────────────────

export interface AllocGenerations {
  legacy: number;
  affine: number;
  feistel: number;
}

// ── Top-level stats response (/api/v1/stats) ──────────────────────────────────

export interface StatsResponse {
  stage: string;
  target_minutes: number;
  timeout_minutes: number;
  active_minutes: number;
  puzzles: PuzzleListEntry[];
  puzzle: PuzzleInfo | null;
  active_workers_count: number;
  inactive_workers_count: number;
  total_hashrate: number;
  completed_chunks: number;
  reclaimed_chunks: number;
  total_keys_completed: string;   // BigInt decimal string
  virtual_chunks: VirtualChunks;
  shards: VirtualChunks;          // legacy alias for virtual_chunks
  workers: WorkerInfo[];
  scores: ScoreEntry[];
  finders: FinderEntry[];
  chunks_vis: ChunkVis[];
  alloc_generations: AllocGenerations;
}
