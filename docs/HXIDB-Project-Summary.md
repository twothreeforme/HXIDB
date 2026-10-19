# HXIDB: project summary

HXIDB is a player contributed drop-rate database for HorizonXI. A small passive Ashita plugin reads what a player's own game client already receives, a server stores and checks the reports, and a MediaWiki extension shows the result on each mob's wiki page.

## Contents

1. [Mechanics and the PAFO principle](#1-mechanics-and-the-pafo-principle)
   - 1.1 [What it does, end to end](#11-what-it-does-end-to-end)
   - 1.2 [Rules the design follows](#12-rules-the-design-follows)
   - 1.3 [How this supports the PAFO principle](#13-how-this-supports-the-pafo-principle)
   - 1.4 [No private data is collected or stored](#14-no-private-data-is-collected-or-stored)
2. [The plugin](#2-the-plugin)
   - 2.1 [What it is](#21-what-it-is)
   - 2.2 [Install, load and unload](#22-install-load-and-unload)
   - 2.3 [Packets evaluated](#23-packets-evaluated)
   - 2.4 [What the plugin sends to the server](#24-what-the-plugin-sends-to-the-server)
   - 2.5 [How the encryption and signing work](#25-how-the-encryption-and-signing-work)
   - 2.6 [Disk and memory](#26-disk-and-memory)
   - 2.7 [Tests made on the local server, and why](#27-tests-made-on-the-local-server-and-why)
   - 2.8 [Plugin open items](#28-plugin-open-items)
3. [The server and the wiki extension](#3-the-server-and-the-wiki-extension)
   - 3.1 [Architecture](#31-architecture)
   - 3.2 [Ingestion](#32-ingestion)
   - 3.3 [Storage](#33-storage)
   - 3.4 [Trust and agreement](#34-trust-and-agreement)
   - 3.5 [Controls against abuse](#35-controls-against-abuse)
   - 3.6 [The wiki extension](#36-the-wiki-extension)
   - 3.7 [How Treasure Hunter is shown](#37-how-treasure-hunter-is-shown)
   - 3.8 [Retention (direction decided)](#38-retention-direction-decided)
   - 3.9 [Server and wiki open items](#39-server-and-wiki-open-items)

---

## 1. Mechanics and the PAFO principle

### 1.1 What it does, end to end

```
game client:  							-> 	HXIDB server:  				->  wiki extension
 reads packets, builds "kill events"      checks, merges, stores            reads the server's views and
 signs and sends them                     per-kill facts                    renders the tables on mob pages
```

1. A player kills a monster. The client receives the normal game packets: the monster's defeat message, the treasure pool items, the gil message.
2. The plugin turns those into one **kill event**: zone, which monster, what dropped, how much gil, the party's jobs and levels, and a time taken from the game server's own clock.
3. The event is signed and sent to the server. If several party members run the plugin, each sends its own report of the same kill.
4. The server merges those reports into **one kill** (a kill counts once per group, however many people report it), checks it, and stores it.
5. The wiki extension reads the stored kills and shows, per monster and per zone, every drop with the raw count (for example `12/38`), the calculated percentage with a 95% range, a Treasure Hunter comparison, gil per kill, and how the rate moved over time.

### 1.2 Rules the design follows

- **Passive and read-only.** The plugin never blocks, alters or sends anything to the game. It only listens to packets the client already received.
- **A kill is counted once.** Gil and drops are separate facts: a kill that only gave gil is a kill with zero drops. Different monsters, different drop lists and different zones are never pooled by default.
- **Honest sample sizes.** Every percentage is shown with the numbers behind it. Nothing is hidden below a kill count; a small sample gets a warning line. Rates are also shown month by month.
- **Reference data is only a comparison.** The listed drop rates come from the LandSandBoat reference database. Horizon differs from it in places (for example Qiqirn drop lists), which is the reason to collect real data.
- **The server does the defending.** A client cannot be made tamper-proof, so the server checks everything: signatures, plausibility against the reference data, respawn limits, agreement between reporters, outlier review, and revoke with rollback.

### 1.3 How this supports the PAFO principle

PAFO means **Play and Find Out**. Private servers such as Horizon deliberately do not publish drop data: players are meant to play, discover how the world behaves, and share what they learn with the community. HXIDB is built to fit that principle, not to get around it.

- **The community finds out, not the server.** Nothing comes from Horizon: no server access, no data feed, no logs, no extra packets, and no request is ever sent to the game server. Every number on the wiki is something players actually observed while playing, which is what "find out" means.
- **Only what the player's own client already sees.** The plugin reads packets the client has already received (section 2.3) and gives the player no information they did not already have. It does not probe, scan, or ask the server anything. It reveals nothing hidden: the real Treasure Hunter tier, for example, is never sent to the client, so the wiki shows a labeled estimate from who acted on the monster instead of a server value. Monster positions and player positions are not collected, so it is not a radar.
- **Play is not changed or automated.** The plugin never sends, blocks or alters a packet and adds no commands or in-game actions. It does not play for anyone. (The scripted driver used for local testing is a separate tool outside the project and is never part of what players load.)
- **Observed, not official.** Every page presents the data as community observation: raw counts next to every percentage, a 95% range, the number of kills and reporters, and a month-by-month view, with a warning when the sample is small. The reference drop list from the open-source LandSandBoat project is shown only as an approximate comparison, labeled "Horizon may differ" (and it does differ in places, for example Qiqirn). A small sample is shown as it is, so players can decide how much to trust it, and rare content fills in as more people play it.
- **Shared by the community, owned by no one.** Reports come from players who choose to load the plugin; loading it is the consent. The release plugin has no pause, no opt-out and no settings, because any of those would let outcomes be hidden and skew what the community learns. The server address is fixed in the plugin.
- **No private data** (next subsection): sharing drop knowledge never requires sharing anything about the player.
- **Approval.** Horizon has approved building and testing. A separate packet-use summary (Phase 7) is still needed before wider release, so the server staff can confirm the plugin stays within what they are comfortable with.

### 1.4 No private data is collected or stored

| Item | Collected or stored? | Detail |
|---|---|---|
| Player names (own, party, others) | No | Never read into events. Mob names only. Names inside packets are blanked in dev captures. |
| Character ID | Read on the PC only, never sent or stored | Used locally as the input of the identity derivation (2.6). The server only ever sees a blinded value, from which it learns nothing about the ID. |
| Account, login, email, IP address in the database | No | The server sees the connection address while a request runs (for the identity rate limit, in memory) and a rejected request may appear in an operational log line. It is not stored with kills. |
| Party member IDs | Raw IDs never leave the PC; anonymous tags are sent | Each member's ID is turned into an 8-byte tag through the oblivious PRF (a server key different from the identity key), so every member's plugin produces the same tags and the server can tell reports of one party apart as a fact. The server cannot reverse a tag. IDs are also compared in memory with the actor of an action packet; they are never stored, logged, captured or sent. A unit test checks the encoded event carries the tags and no raw ID. |
| Chat, inventory, other items, equipment | No | Not read. |
| Positions | No for monsters and players | Only chest, coffer and crate events carry the chest's own position, to tell chests with the same ID apart. Locations are not part of the received data. |
| Reporter identity | Pseudonymous key hash only | Identity is derived from the character through an oblivious PRF; the server stores an HMAC of the public key, never the key. Without the server's secret nobody can compute or reverse it. It links the reports of one character, not a name or account. |
| What is stored per kill | Yes | Zone, monster ID and model, kill time (minute and game-server time), items dropped, gil received, party jobs and levels, party counts, Treasure Hunter grouping, chest contents and chest position (chests only). |
| Files on the player's PC | Release: none except the status box position | No key, queue, capture, log or config file. Dev builds can write test files. |

Honest limits: the identity is pseudonymous (it links the reports of one character over time) and anyone with the server's secrets and a database copy could test guesses for an ID. This is why the server's pepper and OPRF key are kept outside the database, why identity derivation is rate limited, and why the retention plan removes the reporter link from old kills (3.8). Any character-based identity depends on Horizon accepting it (assumed approved for the test phase).

---

## 2. The plugin

### 2.1 What it is

An Ashita v4 plugin (32-bit, C++20, built with Visual Studio 2022). Source: `plugin/HXIDB/`.

| Part | File | Job |
|---|---|---|
| Plugin host | `HXIDB.cpp` | Ashita entry points: receives packets, reads party and entity memory, drives the status box, starts the reporter. |
| Event builder | `core/builder.h` | Turns packets into kill events. No Ashita code, so it is tested by replay and unit tests. |
| Event model and encoding | `core/events.h`, `core/pb_writer.h` | The event structure and its Protobuf encoding. |
| Reporter | `core/reporter.h` | Identity, enrollment, signed batching, retries, memory-only queue. |
| Transport | `core/http.h` | A minimal WinHTTP POST. Nothing is cached or written to disk. |
| Identity | `core/oprf.h` | Oblivious PRF client (libsodium). |
| Signing | `third_party/monocypher` | Ed25519 signatures. |

### 2.2 Install, load and unload

- **Install:** copy `HXIDB.dll` into `Ashita\plugins\`.
- **Load:** `/load HXIDB` in the game (or add it to the Ashita load script). Loading is the consent to report.
- **Unload:** `/unload HXIDB`. The DLL is locked while loaded, so unload before replacing it.
- **Build:** `plugin\HXIDB\build.bat [dev|release] [backend url]` produces `bin\testing\HXIDB.dll` (dev) or `bin\release\HXIDB.dll` (release). The backend address is compiled in.
- **Two profiles:**
  - **Release (silent):** no commands, no capture, no probe, no status files, no settings. Nothing is written to disk except the status box position (`config\HXIDB\ui.ini`). The status box is read-only: a connection dot, sent and queued counts, and the last payload.
  - **Dev (tester):** adds `/hxidb status|start|stop|probe`, opt-in packet capture to `config\HXIDB\*.jsonl` for replay, a heartbeat file and notice text. Used only for testing.
- **Checked:** the release DLL was searched for dev strings (`devstatus`, `.jsonl`, `probe`, `capture`, `notice.txt`, `/hxidb`) and has none; the only disk write path in release is the status box position file.

### 2.3 Packets evaluated

Everything arrives through Ashita's incoming-packet event. The plugin never modifies or blocks a packet.

| Packet | Meaning | Fields read | Status |
|---|---|---|---|
| `0x00A` | Zone in | Zone ID (`0x30`), game-server Unix time (`0x38`). The server time is anchored to the PC's monotonic clock so kill times are on the game server's clock, not the PC's. | **Used** |
| `0x00E` | Entity update | Only entity index below `0x400` (monsters and objects; players and pets are dropped): unique ID, index, name (up to 16 characters), model bytes. Position only for chests. | **Used** |
| `0x028` | Action | Actor ID (compared in memory only) and the target list (bit-packed). Used only to learn the highest Thief level of a party member that acted on a monster. | **Used** |
| `0x029` | Battle message | Only message **6** (defeat: target is the monster) and **565** (gil obtained: the amount). Every other message ID is discarded. | **Used** |
| `0x0D2` | Treasure pool | Item ID, quantity, source monster ID, server tick (the same on every client, used to identify the kill). Pool-clear packets (item 0 or source 0) are ignored. | **Used** |
| `0x034` | NPC event | For chests: chest ID, item ID list, event number. Only whitelisted chest, coffer and crate sources with valid item IDs. | **Used** |

Also read from Ashita's memory interfaces (not packets): party slots 0 to 5 (active flag, zone, main and sub job and level, member ID for the in-memory comparison), entity flags and positions of party members and the monster (to count who is within 50 yalms for the gil split), and the character ID (identity input, never sent).

### 2.4 What the plugin sends to the server

Transport is HTTPS to the compiled-in address. Bodies are Protobuf (`proto/hxidb.proto`), at most 64 KB, at most 50 events per batch.

| Request | When | Content | Learns what |
|---|---|---|---|
| `POST /v1/oprf` | At start, once the character is known | One blinded 32-byte point | Nothing about the character ID (it is blinded with a fresh random value). |
| `POST /v1/enroll` | After identity is derived | Protocol version and the public key | A new pseudonym (stored only as a keyed hash). |
| `POST /v1/ingest` | After a kill or chest, batched shortly after | Envelope: protocol version, public key, signed payload, signature. Payload: 16-byte random nonce, timestamp, up to 50 events. | The events below. |

Fields of one event (all from `proto/hxidb.proto`):

| Field | Source | Note |
|---|---|---|
| zone, source ID, source name, model bytes, source type | `0x00A`, `0x00E`, `0x029` | Monster or chest. Name is the game's, not a player's. |
| event minute, kill time (ms), time-anchored flag | Game-server clock | Used for kill identity. |
| kill tick | `0x0D2` | Server tick shared by every client. |
| drops (item, quantity, slot) | `0x0D2`, `0x034` | |
| gil | `0x029` msg 565 | This reporter's share. |
| defeated flag | `0x029` msg 6 | |
| party (job, level) list | Party memory | No names or IDs. |
| gil party (within 50 yalms), gil party in zone | Party memory | Lower and upper bound of how many shared the gil. |
| Thief acted level, acted-known flag | `0x028` | Highest Thief level of a party member seen acting on the monster; 0 means the plugin watched and none acted. |
| Own Treasure Hunter: trait and gear | `0x028` plus equipment memory | When this player's own action hits the monster, the plugin reads the job trait tier (TH I from Thief 15, TH II from 45, main or sub) and the TH bonus of the gear worn at that moment, because gear can be swapped at any time. Item IDs are looked up in memory; only the two numbers are sent. |
| Anonymous party tags | Oblivious PRF over each member's ID | This reporter's tag and the sorted tags of the whole party (up to 6, 8 bytes each). |
| chest position, chest kind, spawn minute | `0x00E`, `0x034` | Chests only. |
| client event ID | Random 64-bit | Only so a resend of the same event is recognised; never links players. |

Timing and failure handling: a failed send backs off from 2 to 120 seconds; a batch the server will never accept is dropped. The queue is memory only, capped at 2000 events (oldest dropped); events still queued when the game closes while offline are lost.

### 2.5 How the encryption and signing work

| Layer | What | Protects |
|---|---|---|
| Transport | TLS (HTTPS through WinHTTP) in production | Confidentiality and integrity on the wire. |
| Payload signature | Ed25519 over the exact payload bytes (Monocypher on the plugin, Go `crypto/ed25519` on the server) | The report was made by the holder of the enrolled key and was not changed. This is authenticity, not secrecy. |
| Replay protection | 16-byte random nonce, stored per reporter, plus timestamp within 5 minutes of the server clock | A captured request cannot be sent again. |
| Identity | 2HashDH oblivious PRF on ristretto255 (libsodium on the plugin, `gtank/ristretto255` on the server) | A stable pseudonym per character without the server learning the character ID, and without the plugin storing a key. |
| Identity at rest | The server stores HMAC-SHA256(pepper, public key), never the key | A copy of the database alone cannot be matched to keys. |
| Offline queue | Memory only | Nothing readable on disk. |

Identity derivation in steps: the plugin takes the character ID, hashes it to a curve point, blinds it with a fresh random scalar, and sends only the blinded point. The server multiplies it by its secret key and returns the result. The plugin unblinds it and hashes it into a 32-byte seed, which becomes an Ed25519 key pair held only in memory. The same character gets the same identity on every launch, another character gets another one, and no one can compute an identity without the server's secret. Each guess of an ID costs a request, limited to 30 per client address per hour. Limits: it is not the verifiable (VOPRF) variant, so the plugin trusts the server over TLS.

Honest limit: nothing on a player's PC can be made tamper-proof; a determined person can modify the DLL or call the API with their own key. The real defenses are on the server (3.3 to 3.7).

### 2.6 Disk and memory

Release build: everything lives in memory for the life of the client. The single disk write is the status box position. Windows crash dumps and the pagefile are outside the plugin's control.

### 2.7 Tests made on the local server, and why

The local test stack is a private LandSandBoat game server (not Horizon), the plugin in two real game clients, the HXIDB server with a local MariaDB, and a local MediaWiki 1.43.10. A separate scripted test driver, kept outside the project and not part of it, ran commands in the two clients (spawn a monster, position the characters, cast a spell, read chat) so tests could be repeated and counted.

| Test | Why | Result |
|---|---|---|
| Packet captures with two clients (zones 100/102) | Find the real packet layouts and what signals a kill, gil and chest contents | Defeat message (`0x029` msg 6) and gil (msg 565) are reliable; the kill with no drop is detectable; chests use `0x034`, not the pool. |
| Replay of scrubbed captures through the builder into the server | Prove the same code path as live without a game running | Counts and drops match the captures. Found two bugs (an unsigned overflow in the event ID, a retry loop), both fixed with regression tests. |
| Two clients in one party kill the same monster | Merge reports of one kill into one event | One event, two reporters; each reporter is never merged into its own kill twice. |
| Party member out of range | The gil split counts only members in range | The client sees stale data beyond about 50 yalms (render flag 0x4000); fixed to use the render flag. |
| Distance test for gil (near, 75, 120 yalms) | Learn how the server splits gil | Even split among members within about 100 yalms (about 55 near, about 116 at 120 yalms). |
| Treasure Hunter tests (idle Thief, Thief acting by spell, melee, sub job) | Check the grouping really follows who acted | Acting Thief gives the right group 4 of 4 (spell), 4 of 4 (melee), sub-job level counts; an idle Thief gives group 0. The Mug ability fired and was grouped; Steal's checks (inventory space, recast) blocked it, so it was not seen firing. |
| Multi-kill (one spell kills two monsters) | Two kills in one second must stay two events | Two events, each with both reporters. |
| Dead member in range | Whether a dead member still reports and counts | A dead member counted in the party and its report arrived in one case, not another; to confirm on Horizon. |
| Pools of about 350 Qiqirn kills (64 spawn points reused) | Enough kills to judge rates, Treasure Hunter and gil | Every scripted kill that died was stored as exactly one event (234 of 234, 32 of 32 in the control run). Observed almond drop 63.5% with an acting Thief at TH4 against a listed 24%, matching Horizon's published TH table (64%). |
| Release string audit | The release plugin must be silent | No dev strings; only the status box file path remains. |
| Server API tests (48, none skipped) | Behavior and attack cases | Dedup, 18 reporters one kill, respawn floor, zero-drop kills, chests, auth, rate limits, kill switch, revoke and rollback, hold, outliers, trust phases, agreement floor, views, retention. |
| Plugin unit tests | Isolated logic | Action-packet parsing (8 checks), and the identity derivation reproduces the Go server's fixed test vector exactly. |
| Level 75 only | Horizon's cap is 75 | Final runs used level 75 characters and monsters no stronger than the character. |

### 2.8 Plugin open items

- A mid-session plugin reload loses the zone-in time anchor; kills then use the minute rule (identity still works through the pool tick and the 30-second respawn floor).
- The plugin sees party members only within about 50 yalms but the server splits gil up to about 100, so it also reports the in-zone count as an upper bound.
- Not seen firing live: Steal; an AoE with the monster as a secondary target (covered by the unit test only).
- Server-side TH level was not read directly (the client truncates the GM chat line); the evidence is the stored grouping against the action used.
- Live deployment values and the Horizon packet-use summary (Phase 7) are still to do.

---

## 3. The server and the wiki extension

### 3.1 Architecture

One Go binary (`server/`) with MariaDB, behind a reverse proxy in production (nginx). Local test: Windows, MariaDB 12.3.3 on port 3307, Apache and PHP 8.3 for the wiki. Production: Linux droplet with nginx, MediaWiki 1.43.10, php-fpm, MariaDB.

| Route | Purpose |
|---|---|
| `POST /v1/oprf` | Oblivious PRF evaluation for identity (metered per address). |
| `POST /v1/enroll` | Register a public key (stored only as a keyed hash). |
| `POST /v1/ingest` | Receive a signed batch of events. |
| `GET /v1/stats` | Read-only published statistics (gated views). |
| `GET /healthz` | Liveness. |

Operator commands run on the host (not over the network): `hxidb-server admin status | reporters | pending | outliers | phase test|live | trust-eval | trust <id> | untrust <id> | revoke <id> | restore <id> | hold-source | hold-event | pause | resume`.

Configuration through environment: database DSN, listen address, `HXIDB_MIN_KILLS`, `HXIDB_MIN_REPORTERS`, `HXIDB_RETENTION_DAYS`, `HXIDB_PEPPER` (keyed hash, kept outside the database), `HXIDB_OPRF_SECRET` (never change it), `HXIDB_TRUST_HEADER` (real-address header behind a proxy).

### 3.2 Ingestion

For each request the server checks, in order: protocol version, public key size, **Ed25519 signature**, enrollment (by keyed hash) and revocation, payload decoding, **timestamp within 5 minutes**, 16-byte **nonce not seen before**, per-minute request limit (60), the **kill switch**, the **hourly event cap** (3000 per reporter), and at most 50 events. Then each event is validated against plausibility rules (known zone, sane quantities and gil, plausible server time) and stored in one transaction.

**Kill identity** (what makes two reports the same kill), in order of preference:

1. The **pool tick** (server milliseconds, the same on every client), within about 5 seconds.
2. The **kill time on the game-server clock**, within about 5 seconds, for kills without items.
3. The **minute** when the plugin had no time anchor.

Plus: a reporter is never merged into a kill it already belongs to (two real kills of a fast respawner stay two); a **respawn floor** (30 seconds) rejects impossible repeat kills; a random per-event ID only makes a resend recognisable. Items of one kill are merged across reports; chests merge by contents and position with a conflict flag when reporters disagree.

**Recompute** rebuilds an event from the reports that count (3.4), so revoking or restoring a reporter is the same operation. Statistics are only ever read through the views `v_ev` (kills that count) and `v_rep` (reports that count), never the raw tables.

### 3.3 Storage

| Table | Holds |
|---|---|
| `source_events` | One row per kill or chest: zone, source, time, TH bucket, hold and excluded flags. |
| `event_drops` | Final drop list per kill. |
| `event_reports` | One row per reporter and kill: that reporter's gil, party counts (in view, in zone), TH bucket, usable flag. |
| `event_report_drops` | What each reporter said dropped. |
| `reporters` | Keyed hash of the public key, trusted and revoked flags, timestamps. |
| `nonces`, `settings` | Replay protection; phase, thresholds and the kill switch. |
| `rollup_*` | Old kills folded into counts by retention. |
| Views `v_stat_*` (ungated) and `v_pub_*` (gated by the kill threshold) | What the wiki and the API read. `v_stat_gil_values` and `v_stat_period` serve the study and over-time tables. |

### 3.4 Trust and agreement

Two states for a reporter, `trusted` or not, and two phases.

- **Test phase** (`live_mode = 0`): every install that enrolls is trusted. One kill from one tester counts. Used for the control group and the dev setup.
- **Live phase** (`live_mode = 1`): new installs enroll untrusted.

How a kill is built from its reports:

1. If at least one trusted reporter reported the kill, only trusted reporters shape it (items, Treasure Hunter group, gil share). An untrusted install cannot add anything to a kill a trusted one saw.
2. If no trusted reporter reported it, the kill counts only when at least **`agree_min` distinct reporters agree, and `agree_min` is 7 or more**. The server enforces a floor of 7 in code, so a lower setting is ignored. Only items that enough reporters saw are kept.
3. Otherwise the kill is stored but excluded (hidden).

**How trust is earned.** An untrusted reporter becomes trusted when `trust_after` (default 20) of its kills were also reported by an already-trusted reporter and it is not an outlier. Agreement among untrusted reporters never earns trust, so a ring of invented identities cannot bootstrap itself. Operators can also `trust`, `untrust`, `revoke` (the reporter is refused and its contribution is subtracted from every statistic, with shared kills rebuilt without its claims), `restore`, and `hold` sources or events (kept for review, not counted).

**Outlier review.** Per monster and item, each reporter's drop rate is compared with the median of all reporters' rates (reporters with at least 15 kills on that monster). A reporter is flagged when the gap is 25 points or more and the median lies outside its own 99% Wilson interval. It is report-only; the operator decides.

**Why the reporter link matters.** Revoke, outlier review and trust all need the link between a report and its reporter. See 3.8.

**Rare content.** A once-killed NM by an alliance counts as one kill. It is accepted when a trusted reporter was present or at least 7 plugin users in the alliance reported it. Its drops are then shown as `1/1` with a very wide range rather than hidden.

### 3.5 Controls against abuse

| Control | Detail |
|---|---|
| Signature, nonce, timestamp | Section 2.5. |
| Per-reporter limits | 60 requests per minute (memory), 3000 events per hour (counted from stored reports). |
| Identity derivation limit | 30 per client address per hour. |
| Kill switch | `settings.accept_events = 0` pauses intake; plugins keep their queues and retry. |
| Plausibility | Known zone and monster, sane quantities and gil, respawn floor, plausible server time. |
| Consensus and trust | 3.4. |
| Moderation | Hold, revoke with rollback, outlier report, `admin status` (including the share of kills corroborated by two or more reporters). |
| No keys stored | Reporters found by HMAC of the public key; migration erases old raw keys. |

### 3.6 The wiki extension

`extension/HXIDB/` (MediaWiki 1.43.10 / PHP 8.3; runs on 1.41 too). It reads the database directly with the wiki's own credentials; no second service or API call is needed on the same host.

**Matching.** `ArticleViewFooter` adds a block to any page in a configured namespace whose title matches a monster name (`mobname` with underscores or `polutils_name` with spaces) from the reference database `LSB_Data`. The block is cached for 5 minutes. `__NOHXIDB__` turns it off for a page.

**Pooling.** The unit is one zone and one drop list. Spawn points that share a drop list in a zone are pooled; the same name in a zone with a different drop list stays separate; zones are never pooled.

**What the table shows, per zone section**
- Level range, number of spawn points and the reference spawn area (a general location only).
- Per item: the reference **listed rate**, then **observed rate** for all kills and for each Treasure Hunter group. Each cell is the raw count and percentage together (for example `23.1% (6/26)`), with the 95% Wilson range on hover. Items seen but not in the reference list are starred.
- A one-line sample note: kills and number of reporters.
- A **small-sample warning** above the table when the kills or reporters are below `HxidbMinKills` or `HxidbMinReporters`. Nothing is hidden.
- **Observed rate over time**: one row per month with kills and each top item's rate (drops and kills shown), running total on hover.
- **Gil per kill**: mean and range of the share received per kill.

**Study tables** appear when the pool has `HxidbStudyMinKills` (100) kills, using every monster of the zone that shares the drop list: a Treasure Hunter rate-ratio table (no Thief acting against a Thief acting, with a 95% range, needing 10 kills in each group), gil by the party size seen and the zone party size, gil with and without a Thief (Gilfinder), and a histogram of the gil received. `Special:HxidbPools` lists qualifying pools; `Special:HxidbMobs` lists every tracked source with kills, reporters and status, and flags monsters missing from the reference data.

### 3.7 How Treasure Hunter is shown

The client is never told a monster's Treasure Hunter tier, so the plugin works it out from what it can see, and the table shows the **tier** (0 to 4, Horizon's maximum).

- **Own TH, read at the time of the action.** When the player's own action hits a monster, the plugin reads the job trait (TH I from Thief 15, TH II from 45, main or sub job) and the Treasure Hunter bonus of every worn piece of gear, and sends both numbers. Any job can add TH with a piece of gear, so a non-Thief with one TH+1 piece records tier 1; swapping the piece off afterwards does not erase it (the server keeps the highest tier a monster received). Checked against the game server: the plugin's reading matched the server's TH for bare, festive cap, Thief with cap, and Thief with cap and armlets.
- **Other players.** Their actions are seen only as the Thief level of the actor (trait tier); their gear is not visible, so their real tier may be higher.
- **Server.** The tier of a kill is the highest of the reports that count: each reporter's own trait plus gear, and the trait tier of any acting Thief level, capped at TH4. A reporter's claim is checked (the trait cannot exceed what its own job and level allow; trait plus gear cannot exceed TH4); claims that had to be reduced, or gear claims nothing corroborates, are flagged for the operator (`admin th-claims`). Reports from members of one party carry the same anonymous party key, so the server knows they describe the same party.
- **Display.** Columns per tier ("No TH", "TH 1", ...) with the kill count in the header; in the study table one "TH applied" group against "No TH" with the rate ratio. A note says the tier is the highest seen on a player who acted, and that other players' gear is not visible.
- **Older reports** (before the plugin watched actions) use the highest Thief level in the party instead.
- **Gil and Gilfinder.** A Thief (Gilfinder from level 5, +50%) raises every member's share, so the study page also compares gil with and without TH applied.

### 3.8 Retention (direction decided)

- Keep all kill-level data forever **without the reporter ID**: monster, zone, time, drops, gil, Treasure Hunter group, party counts.
- Keep the reporter link for a review window (suggested 90 days). Revoke, outlier review and trust evaluation need it, so they run before any unlink. A kill is unlinked only after the window **and** once corroborated (two or more reporters, or one trusted reporter). Single-reporter untrusted kills keep the link longer (suggested one year) or stay held.
- Keep per-kill report counts and the held and excluded flags permanently, and a keyed distinct-reporter sketch for any publication gate.
- Today retention still rolls kills older than 180 days into counts; the change above is designed but not yet built.

### 3.9 Server and wiki open items

- Deployment values: `HXIDB_PEPPER`, `HXIDB_OPRF_SECRET`, `HXIDB_TRUST_HEADER`.
- Run the reference-database verification (`server/tools/verify_lsb/lsbsum.py`) on the live server so its `LSB_Data` matches the verified copy.
- Build the retention change in 3.8; decide the unlink window.
- Confirm on Horizon whether dead members count for loot and gil, and the real gil-split range.
- Pool by monster name or verified drop list: Horizon's lists differ from the reference (for example Qiqirn).
- Re-verify the extension on a 1.43.10 staging instance with nginx and php-fpm.
- Enrollment gate before any public release (the control group has none by decision), alerting and automatic quarantine rules.
- Phase 7: the architecture and packet-use summary for Horizon.
