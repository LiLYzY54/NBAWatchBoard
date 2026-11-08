**English** · [简体中文](nba-board.zh_CN.md)

# NBA Scoreboard Application

The NBA scoreboard turns the AI Passport into a pocket scoreboard: it fetches one
NBA game day, lists the games on a dark scoreboard-style screen, and opens a
per-game detail page with quarter-by-quarter scoring, and from there opens a player
page with both teams' points/rebounds/assists. The game day can be stepped
backwards and forwards one day at a time instead of being pinned to "today".

This document describes the application that replaces the baseline hardware-test
menus in `main/`. The baseline demo sources remain in the tree as BSP references
but are no longer compiled into the firmware.

## Screens and controls

| Screen | Content | Keys |
| --- | --- | --- |
| Overview | Up to 16 games, 4 visible; team abbreviation, score, status (quarter + clock / final / tip-off time) | Up/Down: move selection (wraps); OK: open detail; long Up/Down: previous/next game day; long OK: refresh now (reopens provisioning when no usable network) |
| Detail | Both teams' abbreviations, large scores, records, quarter table (Q1–Q4, total), state, game day | Up/Down: switch game; OK: open players; long OK: back to overview |
| Players | Both totals on top; one team at a time, with a header row (away/home, abbreviation, team total) and that team's players; 8 rows visible. The name column is fixed, the three stat columns slide to the next group on each OK press | Up/Down: move cursor (wraps); OK: slide to the next stat group (nothing rotates on its own); long Up: home team; long Down: away team; long OK: back to detail |
| Provisioning | Hotspot name, the address to open on the phone, current state. Shown when there are no credentials, no network can be reached, or the gateway blocks the internet behind a login page | long OK: leave (long OK again reopens the hotspot while no usable network exists); all other keys ignored |

### The hint line at the bottom

Every screen carries one hint line at the bottom, but it deliberately **does not list
every key**. The line is 232 px wide on a 240×320 panel; at 16 px Chinese that is 14
characters, and a complete list would wrap. The rule is: *universal gestures are not
worth mentioning, long presses are.* "Up/Down selects, OK enters" is the same gesture
every list uses; what nobody guesses are the long presses.

| Screen | Hint (on-screen Chinese, translated) |
| --- | --- |
| Overview (provisioned, internet OK) | `长按确定刷新·长按上下换日` — long OK refresh, long Up/Down change day |
| Overview (no credentials / no internet) | `长按确定配网` — long OK configure network |
| Detail | `上下换场·确定球员·长按返回` — Up/Down switch game, OK players, long OK back |
| Players (two lines) | `上下选人·确定换组` / `长按上下换队·长按确定返回` — Up/Down pick, OK next stat group; long Up/Down switch team, long OK back |
| Provisioning | `长按确定返回` — long OK back |

Two of them change with state, on purpose:

- Long OK on the overview refreshes only when credentials exist and the internet
  actually works; it reopens the hotspot when there are no credentials *or* when
  Wi-Fi is associated but a captive portal blocks the internet. One key, two
  unrelated actions - a static hint would be a lie. The captive-portal case needs
  the hint most: Wi-Fi looks fine, so the device otherwise seems broken.
- Players is the only screen needing two lines: three actions (stat group, team,
  back), two of them long presses. The cost is the 8 roster rows shrinking from
  24 px to 22 px (20 px row card).

Line widths are measured from the font's `adv_w` character by character. The longest
one, detail, comes to 224 px against the 232 px limit - 8 px to spare. Adding a
character to that line means removing one first.

Global behaviour:

- The game day of the data on screen (`10-31`) sits next to the title. It turns
  amber whenever it is not "today".
- The battery badge sits in the top-right corner. When the fuel gauge does not
  answer, the badge stays empty instead of showing a fake percentage.
- The backlight drops to 22% after 30 s without a key press and returns to 100%
  on the next press. There is no deep sleep: the board is meant to stay visible
  while a game is on.
- Long OK on the overview triggers an immediate fetch. Automatic refresh is not a
  fixed 60 s timer; the interval follows the schedule:

  | Games currently shown | Automatic refresh |
  | --- | --- |
  | Any game live | 45 s |
  | All pre-game, next tip-off >10 min away | sleep until 5 min before tip-off (15 min cap) |
  | Next tip-off within 10 min | 60 s |
  | All final / no games | 15 min (also keeps the date correct across midnight) |
  | Backlight dimmed | at least 5 min, whatever the state |
  | Clock not synced / last fetch failed | 60 s |

  A fetch is ~276 KB. The old fixed 60 s timer meant 60 downloads an hour even
  after every game had ended. Wi-Fi stays in modem sleep (DTIM) between fetches;
  only the ~15 s HTTP transfer switches to full power, so download speed is unchanged.
  Changing day, long OK, or waking the screen all interrupt the wait immediately.
- A Wi-Fi network that requires a web login (campus/hotel captive portal) does not
  work: Wi-Fi associates, but the gateway answers every HTTP request with a 302 to
  its login page, so no score data arrives and the header shows `无法上网`. The board
  has no browser and cannot complete portal authentication. Whitelist its MAC on
  the portal, or use a network without a captive portal (e.g. a phone hotspot).
  Switching networks needs no computer: the hint becomes `长按确定配网` and a long
  press opens the hotspot; left alone, three minutes without internet opens it once
  automatically.
- The player page is fetched on demand: only when OK is pressed on that game's
  detail page, and the result is cached per game. Scores change during a game, a
  box score does not, so it is not re-fetched on the score refresh cycle.
- One team is shown at a time (long Up/Down switches) because 20 players do not fit,
  and the 12 stat columns do not fit either, so they rotate in groups:

  | Group | Columns |
  | --- | --- |
  | 1 | PTS, REB, AST |
  | 2 | FG, 3PT, FT (`4-9` = 4 made of 9) |
  | 3 | STL, BLK, TO |
  | 4 | MIN, +/-, PF |

  Only the stat area slides: each row's three numbers live in a clipped window that
  slides out, swaps values and slides back in. The name column never moves, so the
  reader never loses track of which player a row belongs to.

  Group changes are driven by the OK key alone; there is no timer. An earlier build
  rotated every 3 s and it swapped the numbers away while someone was reading a row.

## Data source

| Item | Value |
| --- | --- |
| Scoreboard endpoint | `http://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard?limit=16` |
| Game-day parameter | `&dates=YYYYMMDD` (single day, US Eastern); without it the server returns the most recent day that has games |
| Player endpoint | `http://site.api.espn.com/apis/site/v2/sports/basketball/nba/summary?event=<event id>` |
| Auth | none |
| Transport | plain HTTP (port 80) |
| Refresh | scores on an adaptive 45 s - 15 min schedule (or on demand); player data on demand only |

### Who talks to the network

The board itself. It joins the saved Wi-Fi network (SSID/password in NVS under
`nba_cfg`), syncs the clock over SNTP, and `net_task` fetches from ESPN over plain
HTTP directly - no phone, no computer, no relay service, no API key. When the
network is down the header shows the offline state and retries every 3 s without
blocking the keys.

HTTPS is deliberately not used: the chip has 8 MB of flash and no PSRAM, and a TLS
handshake needs a 30-40 KB contiguous block. That rules out any source that needs
TLS, authentication or request signing. Hupu ratings qualify, but they need one
request per player (37 nodes), so the device does not fetch them directly - the
desktop script gathers them and serves them over the LAN (see the Hupu section).

## Game day

The board groups games by **Beijing calendar date**, the same way Chinese
schedule sites do: a game tipping off at 03:00 Beijing time on 11-01 counts as a
11-01 game. The overview header shows that date and the detail page spells it out
as "game day 11-01".

The endpoint buckets games by **US Eastern date**, so the two differ by one day —
Beijing runs 12–13 hours ahead, and a US evening game is the next Beijing morning:

```
Beijing day T  <->  ESPN bucket T-1
```

Measured on 2026-10-31: ESPN bucket `10-31` held 7 games, all of them on Beijing
`11-01 03:00–08:30`, while bucket `11-01` held 4 games, all on Beijing `11-02`.

- The fetch queries `T-1` and then drops the games whose tip-off is not on `T`
  (`nba_board_keep_day`); a bucket occasionally carries a leftover game from the
  previous Beijing day.
- Only when nothing survives the filter does it fetch ESPN's `T` bucket as well:
  games tipping off before noon US Eastern (London, Mexico City) land on the same
  Beijing date. That is rare, so the second request normally never happens.
- The anchor is "today" in Beijing as derived from the device clock
  (`nba_date_today_beijing`), recomputed on every fetch so the board follows the
  device across midnight.
- Only an unsynced clock falls back to a `dates`-less request (the server picks the
  most recent day that has games), and then the anchor comes from the first tip-off
  timestamp; the endpoint itself never says which day it is.
- Long Up/Down steps the anchor by one day within −14 … +7.
- The endpoint accepts a **single day only**: `dates=A-B` returns HTTP 400, so
  browsing costs one request per day.
- When the current day has no games (an off night, or every game belongs to the
  next Beijing morning), the firmware first reuses the second-bucket result when
  all of its games share one Beijing date — those are the upcoming games that were
  already downloaded, so no re-fetch is needed. Otherwise it probes **forward** up
  to 3 days (upcoming games beat finished ones), and only then walks **back** up
  to 3 days for history, marked amber. That automatic jump happens once per boot;
  afterwards only the user moves the day.

Plain HTTP is deliberate. The board has no PSRAM, and a TLS handshake would need
a 30–40 KB contiguous internal-RAM block at the same time as LVGL and the Wi-Fi
stack. The endpoint serves the same JSON on port 80. **If the data source is ever
replaced with an authenticated API, TLS becomes mandatory and the RAM budget must
be re-measured.**

The response is 10–240 KB (typically 10–30 KB for a single day), which does not
fit in RAM. `main/nba_parse.c` is a
streaming collector: `main/nba_json.c` tokenizes the response chunk by chunk and
the collector keeps only the fields the board shows. Memory use is constant and
independent of response size.

## Player data

The summary response is about 450 KB, but `boxscore` is the document's first key
and only occupies the first ~25 KB; play-by-play, win probability and the rest
account for the other 95%. `main/nba_box.c` sets `complete` as soon as both
rosters are read and `main/nba_api.c` drops the connection right there — a fetch
downloads ~25 KB and finishes in well under a second.

Parsing notes:

- `statistics[].keys[]` carries this game's column order (some games add `teamId`
  and friends), so all 12 columns are located **by name**, never by a hardcoded index.
- Compound columns (`fieldGoalsMade-fieldGoalsAttempted`) carry `4-9` and have to be
  split into made/attempted; `plusMinus` is `+26`/`-17`, and the sign must survive —
  an unsigned parse would turn `-17` into `17`.
- Players with `didNotPlay` or an all-zero line are dropped, the rest are sorted by
  points, and at most 10 per team are shown.
- Names use ESPN's `shortName` (`T. Camara`) verbatim — initial first, surname last,
  never reordered. Non-ASCII bytes are stripped: LVGL's Montserrat has no glyphs for
  `ć/š/ü`, which would otherwise draw boxes. Names and values both pick the largest
  font (14 / 12 / 10px) that fits their measured glyph width via `fit_font` in
  `nba_ui.c`; the 124px name column stays single-line, since wrapping would push the
  whole surname onto a clipped second line. The three value columns are 32px with a
  4px gap: shooting groups contain `12-20` style two-digit/two-digit values that are
  35px wide at 14px and used to be clipped to `2-20` in the old 26px columns. Smaller
  fonts keep both numbers intact; ordinary values (`+26`, ~25px) stay at 14px.
- The endpoint orders the two teams by `displayOrder`, not with the visitor first,
  so the away team's abbreviation is used to align the two columns.

## Hupu data (desktop script)

ESPN gives English abbreviations and surnames; Hupu has Chinese names and a fuller
box score. That data does not go to the device: `tools/hupu_scrape.py` fetches it on
a computer (standard library only, no login).

```text
python3 tools/hupu_scrape.py 2026-11-01              # print the day: rosters + top comment
python3 tools/hupu_scrape.py 2026-11-01 --game 169027 --json /tmp/g.json

# player ratings: --per-team caps how many players per side (one request each)
python3 tools/hupu_scrape.py 2026-11-01 --ratings --per-team 8 --json /tmp/score.json

python3 tools/hupu_scrape.py 2026-11-01 --serve 8787 # LAN JSON at http://<IP>:8787/hupu.json
```

It reads three server-rendered pages plus the rating API:

| Page | What it yields |
| --- | --- |
| `nba.hupu.com/games/<date>` | the day's games: Chinese team names, score, Hupu game id |
| `nba.hupu.com/games/boxscore/<id>` | the box score: Chinese player names plus minutes, FG, 3PT, FT, offensive/defensive/total rebounds, assists, fouls, steals, turnovers, blocks, points, plus/minus |
| `bbs.hupu.com/<tid>.html` | the `lights` array in `__NEXT_DATA__` - comments sorted by "lights", i.e. the top comment |
| `games.mobileapi.hupu.com/1/8.2.99/bplcommentapi/...` | ratings: per-player average score, voter count, score distribution, top comment |

Implementation notes:

- Game threads have no stable id, so candidates are filtered by title: both team
  names present, or the game's score present, or a game-thread word plus one team
  name. Titles that quote a different score are rejected.
- The trailing "totals" and "shooting" rows are not players; they land in `totals`
  and `shooting`.
- Compound values such as `4-11` (made-attempted) and `40.8%` stay strings; plain
  integers become numbers.

### Player ratings are reachable - and unsigned

The earlier "ratings only exist in the app, signed API" conclusion came from the wrong
prefix. Ratings live under `bplcommentapi`, which does **not** verify a signature; only
the `1/8.0.0/nba/*` family answers 102. Any version prefix from `1/6.7.0` to `1/8.2.99`
works.

Three hops:

```text
m.hupu.com/nba/schedule                   last 7 days: Chinese team names + matchId (~190KB)
m.hupu.com/nba/live/<matchId>             307 -> detail.html?...&outBizNo=8876   <- rating node id
score_tree/getCurAndSubNodeByBizKey       rosters + pts/reb/ast/minutes/plus-minus (37 nodes)
  ?outBizType=basketball_match&outBizNo=8876
score_tree/getSelfByBizKey                score 9.8 / 48965 voters / top comment
  ?outBizType=basketball_item&outBizNo=<player bizId>
```

Traps:

- `m.hupu.com` needs a **mobile UA**. A desktop UA 307s to
  `bbsactivity.hupu.com/pc-viewer/?t=...`, which is only an `<iframe>` shell around the
  mobile page (plus a QR code) - no data of its own, and cross-origin anyway.
- `scoreAvg` in the listing endpoint is **flaky** (the same URL returns `null` on repeat),
  so each player's score is fetched individually via `getSelfByBizKey`.
- `m.hupu.com/nba/schedule` always returns a 7-day window around today; `?date=` is ignored.
- Rating nodes also include `coach` and `referee`; the board only wants `type=player`.
- One request per player, so the script takes the top N by minutes per side first.

## Wi-Fi provisioning

Credentials live in NVS (namespace `nba_cfg`) and are never printed or compiled
into the firmware. **Everyday provisioning happens on the phone - no computer.**

### Phone provisioning (the normal path)

The board raises a temporary hotspot; the phone joins it and fills in the Wi-Fi
name and password in a browser:

```text
1. The screen shows: hotspot NBA-Board, address 192.168.4.1
2. The phone joins NBA-Board (open hotspot, no password)
3. Open http://192.168.4.1 in the browser
4. Pick/type the Wi-Fi name and password, press connect
5. The phone shows "connected, credentials saved"; the board drops the
   hotspot and returns to the scoreboard
```

Four entry points open that hotspot, covering everything from "first power-on" to
"associated but the internet is blocked":

| Entry point | Condition |
| --- | --- |
| First boot | No credentials in NVS; the provisioning page is the first screen |
| New Wi-Fi environment | Credentials exist but every saved network has been tried and failed for 90 s (old network gone / password changed) |
| Captive portal | Wi-Fi is associated but no ESPN data arrives for 3 minutes; opens once per boot |
| Manual reopen | Long-press OK on the overview when no credentials exist or the internet is blocked, or serial `wifi -p` |

The fourth entry is the escape hatch for captive portals. Those networks hand out
a DHCP lease and the Wi-Fi manager stays `CONNECTED` forever, so it never enters
`FAILED` and `all_failed()` never becomes true. A watchdog that only watches for
connect failures can never fire - an earlier build got stuck showing "no internet"
with no way to switch networks without a USB cable.

Deliberate choices:

- **Connect first, persist second.** The submitted SSID/password is not written
  to NVS directly. The board switches to APSTA and connects with the STA
  interface for real; only after it gets an IP are the credentials saved. A typo
  is the common case, and saving first would strand the device with a config
  that cannot connect and the previous one already overwritten. On failure the
  old credentials are restored untouched.
- **The hotspot lives 10 minutes.** If nobody provisions, it closes and returns
  to the scoreboard - leaving it up wastes battery and is visible to neighbours.
- **Open hotspot, no password.** The window is short and a password mostly locks
  the user out; the page can only submit Wi-Fi credentials and exposes no data
  from the device. That is a trade-off for a personal device, not a general
  security design.
- `wifi_mgr`'s auto-reconnect is suppressed during provisioning: under APSTA the
  STA keeps retrying a network that is not there, occupying the radio and the
  event loop and competing with the page's own connection attempt.

### Why not Bluetooth

BLE provisioning (BLUFI or ESP-IDF's BLE scheme) would work, but this is an
ESP32-C3 with no PSRAM, LVGL already claims a 64 KB static pool, and DRAM has
114 KB left (`idf.py size`):

| Option | Extra stack | Verdict |
| --- | --- | --- |
| Hotspot + web page | ~25 KB (HTTP server + AP buffers) | Chosen. Zero install on any phone |
| BLE + wifi_provisioning | NimBLE ~40 KB + mbedtls | Tight |
| BLUFI (official mini program) | Bluedroid ~80 KB | Riskiest without PSRAM |

See `docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.md`: on a
comparable firmware that had initialized audio, **the largest free block was
about 13 KB** and the AP would not start. This board has no audio, but the lesson
holds - provisioning must be the last subsystem brought up, with explicit
start/stop ownership.

### Serial provisioning (development fallback)

Provisioning while flashing, or injecting keys to walk the pages, goes through
the USB serial console:

```text
nba> wifi                  # show state and syntax
nba> wifi MyHome 12345678  # save and connect
nba> wifi -p               # open the phone provisioning hotspot
nba> wifi -c               # clear saved credentials (and go provision)
nba> key ok                # inject one key press (ok/up/down/long-ok/long-up/long-down)
```

`key` pushes the event through exactly the same queue as a physical press, just
without the ADC. The three buttons are awkward to hit precisely and worse when USB
is attached for logging, so walking the screens this way is the cheapest check.

The console is reachable with `idf.py monitor` or any 115200 8N1 serial client on
the USB-JTAG interface.

### The provisioning page

`main/nba_prov.c` runs the hotspot and an HTTP server. The page is inlined in the
firmware (~1.8 KB in flash `.rodata`, no DRAM) and pulls no external assets: the
phone is attached to a hotspot with no upstream network, so any CDN link would
just spin.

| Route | Purpose |
| --- | --- |
| `GET /` | The form; inline `fetch('/scan')` fills the Wi-Fi name suggestions |
| `GET /scan` | Scans nearby networks, returns a JSON array (max 8) |
| `POST /save` | Connects first, then saves; returns the "connected" or "failed" page |

The form body is capped at 256 bytes (413 beyond that) and a missing
`Content-Length` is an error. The screen also shows the hotspot name and address -
those two facts exist only on the device and cannot be looked up from the phone.

After connecting, SNTP sets the clock (`ntp.aliyun.com`) and the firmware uses
`TZ=CST-8`. Until the clock is synced the detail page hides the tip-off time
rather than showing a 1970 timestamp.

## Chinese text and fonts

On-screen Chinese uses `assets/fonts/app_font_cjk_16.c`: a 16 px, 4 bpp subset of
Noto Sans SC (SIL OFL 1.1) generated with `lv_font_conv`. Its `symbols` list holds
104 non-ASCII characters; the 70 that actually reach the screen are checked by the
gate below. The LVGL built-in CJK subset is **not** used:
it lacks characters the board needs (for example the ones in "scoreboard",
"detail", "select"), which would render as boxes. Digits and Latin text use
Montserrat 14/20/28/40.

`tools/check_font_coverage.py` extracts every non-ASCII character from the files
that produce on-screen text and verifies each one against the font's glyph table,
so a missing glyph fails the gate instead of appearing as a box on the device. It
drops whole lines containing `ESP_LOGx` first: serial logs never go through LVGL,
and counting them would force log-only characters into the font.

## Runtime memory

Both limits below were measured on hardware, not estimated:

| Setting | Value | Why |
| --- | --- | --- |
| `CONFIG_LV_MEM_SIZE_KILOBYTES` | 64 | LVGL's built-in pool is a separate static region, not shared with the system heap. The three screens (overview 4 rows + detail + players 8 rows) hold about 140 objects and report `39528/60112` bytes used at boot, with a 20584-byte largest free block. With only two screens the 24 KB default once ran out partway through, returned NULL from the object allocator, and landed in LVGL's assert handler (`while(1)`), which surfaces only as a watchdog timeout. |
| `NBA_JSON_STACK_MAX` | 24 | Container stack of the JSON tokenizer. |
| `NBA_PARSE_DEPTH` | `NBA_JSON_STACK_MAX` | Container stack of the collector, with a compile-time assert keeping it no shallower than the tokenizer. A single game-day response nests 15 levels deep (`events[] > competitions[] > competitors[] > leaders[] > leaders[] > athlete`); a hardcoded 12 rejected valid responses as syntax errors. |
| `NBA_BOX_DEPTH` | `NBA_JSON_STACK_MAX` | Container stack of the player-data collector, same rule. |

When allocation fails, `main/nba_ui.c` logs a single "LVGL pool exhausted" line and
skips the remaining layout instead of handing NULL back to LVGL.

## Build and flash

```bash
. /path/to/toolchain/activate.sh      # ESP-IDF 5.5.3 for esp32c3
./tools/validate.sh --static          # repository checks + host tests
./tools/validate.sh --firmware        # build + merged image verification
# merged image, written at offset 0x0:
# build/FoloToy-AI-Passport-full.bin
```

## Source layout

| File | Responsibility |
| --- | --- |
| `main/main.c` | Boot order: I2C, display, LVGL, battery, app, Wi-Fi, console |
| `main/nba_app.c` | UI task, network task, key semantics, refresh and dimming policy, on-demand player fetch |
| `main/nba_ui.c` | Overview, detail, player and provisioning screens, including the stat-column slide animation (own design, no demo shell reuse) |
| `main/nba_api.c` | HTTP fetch (scoreboard and single-game box score) that feeds the parsers |
| `main/nba_json.c` | Incremental JSON tokenizer |
| `main/nba_parse.c` | Token stream to `nba_board_t` collector |
| `main/nba_box.c`, `main/nba_box.h` | Token stream to `nba_box_t` collector (single-game player data) |
| `main/nba_model.h`, `main/nba_format.c` | Data model and pure formatting |
| `main/nba_prov.c` | Phone provisioning: SoftAP hotspot plus config page (inline HTML, Wi-Fi scan, connect-then-save) |
| `main/wifi_mgr.c` | STA connection, credentials, SNTP, trial connection |
| `main/nba_console.c` | USB serial console `wifi` and `key` commands |
| `tests/test_nba_parse.c` | Host tests: chunked parsing, truncation, deep nesting, linescore field order, formatting, game-day math |
| `tests/test_nba_box.c` | Host tests: chunked player-data parsing, name-based columns, compound columns and plus/minus, early completion, sorting and away/home alignment |
| `tools/hupu_scrape.py` | Desktop scraper for Hupu: Chinese box score plus top comments (see the Hupu section; not part of the firmware) |

## Known limitations

- Day browsing spans anchor −14 … +7; older seasons need `DAY_OFFSET_MIN` raised.
- The game day follows the Beijing calendar; a pre-noon US Eastern tip-off belongs
  to the same Beijing date and is only fetched when the main bucket comes back empty.
- A day with more than 16 games shows the first 16 (`NBA_MAX_GAMES` and the API
  `limit`; the NBA caps at 15 games per day, so this is headroom).
- The player page lists only the top 10 per team (by points), with raw counting stats
  only: no shooting percentages and no offensive/defensive rebound split.
- Player data is fetched once when the page is opened; refreshing a live game means
  leaving and re-entering the page.
- Team and player names are English abbreviations/names; Chinese names would need a
  much larger font subset, so the Chinese data stays in the desktop script.
- Hupu player ratings are obtainable (unsigned `bplcommentapi`) but cost one request
  per player, so they stay desktop-side and reach the device only via `--serve`. Top
  comments are desktop-only for the same reason.
- No game play-by-play, team stats, or standings.
- A Wi-Fi network requiring web authentication cannot be logged into from the
  device; whitelist its MAC or switch to a network without a captive portal.
- Scores come from a third-party public endpoint without SLA; a schema change
  would surface as the "refresh failed" status and keep the last good board on
  screen.
