# `.cflo` scan archive — DRAFT v0.1 (scan-analyzer's answers folded in; not built, not final)

One file per **scan**, solo or fleet, for what the web app downloads and uploads. It replaces "a scan is several
`.jsonl` parts + maybe a Fleet file" with one object the user sees and moves. The scanners do not change what they
write: they keep plain append-only NDJSON (crash-safe, tail-fetchable, cached on the phone by `file_id`). **The web app
builds the archive** from files it already fetched, at Download / Upload time.

Extension: **`.cflo`** (`.flo` is taken: RFFlow / iGrafx FlowCharter flowcharts, the Automate Android app, and the
Middlebury optical-flow format). Media type: `application/vnd.cointelflo+zip` (unregistered; readers also accept
`application/zip`).

> A `.cflo` holds real captures (bystander identifiers + GPS + time). Never commit one. Test fixtures are synthetic.

## 1. Container

- A standard **ZIP** (PKZIP 2.0: no ZIP64, no encryption, no spanning). Size caps: **64 MB compressed**, **256 MB
  uncompressed per entry**, **1 GB uncompressed per archive** (readers enforce the inflated caps while inflating, not
  from the manifest). Rename to `.zip` and any tool opens it.
- Compression methods: STORE (0) and DEFLATE (8) only. Data descriptors (flag bit 3, as streaming writers emit) are
  allowed.
- **Refused** (a reader rejects the whole archive): ZIP64, encryption, any other compression method, entry names with
  `..`, a leading `/`, a backslash, or a duplicate name.
- **First entry `manifest.json`, STORED** (uncompressed), so a reader can sniff the format from the first bytes.
- Log entries **DEFLATE** (web: `CompressionStream("deflate-raw")`; NDJSON compresses ~5-10x). STORE is also valid.
- Entry names: UTF-8, `/` separators. Readers ignore entries they don't know.
- A reader recognizes the format by the `PK\x03\x04` magic plus a first entry named `manifest.json`, whatever media
  type or file name it arrived with.
- Layout:

```
manifest.json
logs/<scanner_id>/<file_id>.jsonl       one per log file, byte-exact (see 2.3 for the torn-tail rule)
```

`<scanner_id>` = the 12-hex id of the unit that WROTE the file (for a fleet file: the leader). `<file_id>` = the 8-hex
`file_id` from the file's line-1 `meta`; a file without one (fw < 0.25.0) uses `nofid-<n>` (n = 1, 2, … within that
unit).

## 2. `manifest.json`

```json
{
  "format": "cflo",
  "version": 1,
  "created_utc": "2026-11-11T22:40:05Z",
  "created_by": { "app": "cyd-scanner-webapp", "build": "<#uiver string>" },
  "scan": {
    "id": "0a1b2c3d4e5f-f00dfeed",
    "kind": "fleet",
    "leader": "0a1b2c3d4e5f",
    "fleet_id": "0011223344556677",
    "start_epoch": 1700000000,
    "end_epoch": 1700003600,
    "tz": "America/New_York",
    "tz_offset_min": -300
  },
  "units": [
    { "scanner_id": "0a1b2c3d4e5f", "scanner_name": "CFLO-MINI-EXAMPLE", "fw": "0.36.0", "db_gen": 5, "role": "leader" },
    { "scanner_id": "6a7b8c9d0e1f", "scanner_name": "CFLO-CYD-EXAMPLE",  "fw": "0.36.0", "db_gen": 5, "role": "client" }
  ],
  "files": [
    {
      "path": "logs/0a1b2c3d4e5f/f00dfeed.jsonl",
      "scanner_id": "0a1b2c3d4e5f",
      "file_id": "f00dfeed",
      "stream": "fg",
      "device_path": "/logs/20261111-171630.jsonl",
      "bytes": 812345,
      "lines": 3120,
      "sha256": "<64 hex of the stored bytes>",
      "first_epoch": 1700000000,
      "last_epoch": 1700003600,
      "trimmed_tail_bytes": 0
    },
    {
      "path": "logs/0a1b2c3d4e5f/deadc0de.jsonl",
      "scanner_id": "0a1b2c3d4e5f",
      "file_id": "deadc0de",
      "stream": "fleet",
      "device_path": "/logs/fleet-20261111-171630.jsonl",
      "bytes": 1203311,
      "lines": 4410,
      "sha256": "…",
      "first_epoch": 1700000006,
      "last_epoch": 1700003596,
      "trimmed_tail_bytes": 0
    }
  ]
}
```

(Ids above are synthetic.)

### 2.1 Fields

| Key | Meaning |
|---|---|
| `format`, `version` | `"cflo"`, integer. A reader rejects a `version` above what it knows; within a version, unknown keys are ignored (additive growth only). |
| `created_utc`, `created_by` | When / by what build the archive was made (phone clock). Informational. |
| `scan.id` | Stable scan identity: `<owner scanner_id>-<file_id of the scan's first own file>`. The same scan re-exported later (more rows, more parts) keeps its id, so the server can treat it as an update of one scan. |
| `scan.kind` | `"solo"` (one unit's own files) or `"fleet"` (includes a fleet file). |
| `scan.leader`, `scan.fleet_id` | Fleet only: the leader's id and the `fleet_id` from its fleet file's meta (newest if several). Omitted for solo. |
| `scan.start_epoch` / `end_epoch` | Span over all files, in the logs' epoch convention (LOCAL wall clock seconds, as `epoch` in rows). 0 = undatable. |
| `scan.tz` | IANA zone name of the phone that built the archive (`Intl.DateTimeFormat().resolvedOptions().timeZone`, the value the web app sends today as `X-Client-Tz`). Readers bucket by it; a fixed offset is wrong across a DST change. Omitted if unknown. With it present the `X-Client-Tz` header is optional. |
| `scan.tz_offset_min` | The offset that local epoch carries (from the phone; omitted if unknown) so a reader can get UTC = epoch − offset·60. |
| `units[]` | Every unit with rows in the archive: from each file's meta, and for fleet files from forwarded member meta lines (`ofile` lines). `role`: `leader` / `client` / `solo`. Display / provenance only; **the rows and meta lines inside the files stay authoritative**. |
| `files[].path` | The entry name. |
| `files[].scanner_id`, `file_id` | The writer and the file's own id (line-1 meta). |
| `files[].stream` | `fg` / `bg` / `fleet` (from the device name: `bg-`, `fleet-`, else `fg`). |
| `files[].device_path` | Where it lived on the SD card when fetched (names change on time sync; informational). |
| `bytes`, `lines`, `sha256` | Of the stored entry (after any tail trim), so a reader can verify it got the whole file. `sha256` is also scan-analyzer's whole-file idempotency key: a re-export stores only the entries that changed. |
| `first_epoch`, `last_epoch` | The file's dated span (the session-list values), 0 = unknown. |
| `trimmed_tail_bytes` | Bytes of a torn last line removed before packing (the live file is being appended while fetched). Normally 0. |

### 2.2 What goes in

- **The scan group the user picked** on the Data page: all its parts (files of one stream within ≤ 6 min of each
  other, as today), plus, on a leader, the fleet file(s) whose span overlaps it (today's "+ Fleet" rule; later the exact
  pairing of 4).
- **Download current session** = the current own file + the current fleet file (leader), as one `.cflo`.
- A selection of several scans → one `.cflo` per scan (not one big archive), so each keeps its own `scan.id`.

### 2.3 File content rules

- Entries are the files **as fetched**, except a torn last line (no trailing `\n`) is cut and counted in
  `trimmed_tail_bytes`. No rewriting: no epoch backfill, no merged meta, no dropped anchors. Backfill stays the reader's
  job, exactly as for a plain `.jsonl` (timesync anchors, per `ofile` for fleet lines).
- A fleet file's lines keep their `"scanner"`, `"ofile"`, `"orow"` prefix.
- Duplicate rows across files (re-logs, a member row also in the leader's own file) are left alone; the server already
  dedups by row.

## 3. Readers

- **scan-analyzer:** check `format` / `version`; then verify **every** `files[]` entry (present, `bytes`, `sha256`)
  **before ingesting any** — one bad entry rejects the whole upload and nothing is stored. Then ingest entry by entry
  (peak memory = one entry) through the existing NDJSON path (unchanged). `scan.id` is stored per session, so a later,
  bigger re-export is recognized as the same scan; row-level dedup keeps re-uploads safe. Entries present in the ZIP
  but not in `files[]` are ignored.
- **Web app:** Map / Explore open a `.cflo` like a `.jsonl` (parse each file entry, join with `joinLogParts`).
- Anything else: unzip and read the `.jsonl` files.

## 4. Upload

Agreed with scan-analyzer (its API): the existing `POST /api/v1/uploads` also accepts the archive as the body, with
`Content-Type: application/vnd.cointelflo+zip` or `application/zip`; the server also sniffs it by magic bytes (§1) in
case a browser relabels the type. **One request = one scan**, replacing today's NDJSON body plus the separate
fleet-file body, so a failure stays scoped to one scan. Plain NDJSON uploads stay supported unchanged for older web
apps. The server's upload limit becomes 64 MB.

**Response (200):** today's top-level keys stay, plus `scan_id` and `files[]`:

```json
{
  "upload_id": 412,
  "session_id": 97,
  "sightings": 5310,
  "events": 42,
  "duplicate": false,
  "scan_id": "0a1b2c3d4e5f-f00dfeed",
  "files": [
    { "path": "logs/0a1b2c3d4e5f/f00dfeed.jsonl", "upload_id": 412, "session_ids": [97], "sightings": 2900, "events": 30, "duplicate": false },
    { "path": "logs/0a1b2c3d4e5f/deadc0de.jsonl", "upload_id": 413, "session_ids": [98, 99], "sightings": 2410, "events": 12, "duplicate": false }
  ]
}
```

Top level: `upload_id` = the first entry's `upload_id`; `sightings` / `events` = summed over the entries;
`session_id` = the first new session, or `null`; `duplicate` is true only when every entry was a duplicate.
`files[]` keeps manifest order. A duplicate entry reports the existing `upload_id`, `session_ids: []`, `sightings: 0`,
`events: 0`, `duplicate: true`. (Numbers synthetic.)

**Errors**, body `{"detail": "..."}` as today:

| Code | When |
|---|---|
| 413 | Too big: over 64 MB compressed, or over an inflate cap (256 MB per entry, 1 GB per archive). |
| 415 | Unknown `format` / `version`, a refused ZIP feature (§1), or a first entry that is not a STORED `manifest.json`. |
| 422 | Manifest malformed or invalid, or a `files[]` entry missing, or its `bytes` / `sha256` mismatched. |

400 and 401 keep their current meanings.

## 5. Firmware addition that goes with it (agreed)

Today the web app pairs a leader's session file with its fleet file by overlapping time (±5 min). Since fw 0.35.2 a
New Session starts both together, so the fleet file's meta could carry `"pair_file_id":"<file_id of the leader's own
file that was live when this fleet file was created>"`. The web app then groups exactly, and `scan.id` is unambiguous.
Additive key; readers that don't know it ignore it.

## 6. Decisions (scan-analyzer, owner-approved 2026-11-11)

1. `scan.id` is made by the client (prefixed by the owner's scanner_id, so no collisions); the server stores it.
2. One archive per scan; no `scans[]`.
3. 64 MB compressed, with the inflate caps in §1.
4. The phone keeps caching per file; the archive is built on demand.
5. `pair_file_id` (§5) and `scan.tz` added.

Status: the spec is final once the owner signs off; nothing is built on either side before that.
