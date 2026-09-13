# Design handoff map (Qt Widgets)

Sources (not included in the repository): `design-handoff/README.md` (spec text), `design-handoff/Surveillance UI.dc.html` (pixel reference; artboards are templates filled by the inline `data-dc-script`, whose arrays hold the mock values), `support.js` (dc runtime only, carries no design values).

Precedence when they disagree: screen artboard > README > design-system sheet / Qt notes card. README states the artboards are the pixel reference.

Milestone context: `docs/PLAN.md` M1 = cameras, live frames, status, reconnect, segmented recording, playback. No analytics, rules, events, search, users or sites. `docs/API.md` `CameraStatus` is the only live datum source.

## 1. Artboards

| # | Label in HTML | Lines | Size | Content |
| --- | --- | --- | --- | --- |
| DS | Design system | 31–118 | 1920 × auto | palette swatches, metrics text, type ramp, button/chip/input/toggle/checkbox samples |
| WF | Wireframe / layout skeleton | 120–143 | 2-col grid, 5 cards | Monitor, Camera settings, Search, Alert log, Model Train. No Statistics wireframe |
| 01 | Monitor · live wall | 145–295 | 1920 × 1080 | default screen; Layout dropdown drawn open |
| 02 | Camera settings · RTSP source | 297–416 | 1920 × auto, dialog 880 wide, content height ≈ 660 | backdrop `rgba(5,6,7,.86)` |
| 03 | Search | 418–534 | 1920 × 1080 | |
| 04 | Alerts & Analytics · alert log | 536–737 | 1920 × 1080 | rule card 1 drawn expanded |
| 05 | Alerts & Analytics · statistics | 739–857 | 1920 × 1080 | |
| 06 | Model Train | 859–1043 | 1920 × 1080 | |
| QN | Qt handoff notes | 1045–1056 | 3-col text | six cards (window, tokens, video, lists, state, operator) |
| — | `data-dc-script` | 1058–1288 | — | mock data arrays; props `showEventFeed` (default true), `wallColumns` (2–5, default 3), `showDetectionBoxes` (default true) |

HTML comment markers are off by one (`SCREEN 2 — SEARCH` precedes artboard 03, etc.). Trust the visible labels.

### 1a. Values present in the HTML that the README does not state

| Value | Where | Note |
| --- | --- | --- |
| `#0E0F11` | tile / thumbnail / player base fill, stripe dark band | needs a token (`bg/video`) |
| `#3A4046` | placeholder captions (`CAMERA FEED`, `SIGNAL LOST`, `TEST FRAME`, …), letter-spacing `.1em` | README only mentions it in the tile line; used on every placeholder |
| `#7FB9D8`, `#A8D3EA` | `a` / `a:hover` in the page `<style>` | no `<a>` in any artboard; unused. No hover colour exists anywhere else |
| `rgba(90,169,214,.9)` | `96% MATCH` badge fill | README says "accent fill" |
| `rgba(224,96,60,.16)` | `MISSED BY vlm-v4` badge (h24, radius 5, padding 0 10) | README chip tint is `.14` |
| `rgba(5,6,7,.8)` / `.82` / `.85` | duration badge; example caption bar; player control bar | overlay scrim family beside `.72` (tile chips) and `.86` (dialog backdrop) |
| `rgba(0,0,0,.6)` shadows | dialog `0 24px 60px`, dropdown `0 16px 40px` | README lists both; nothing else has a shadow |
| Title bar padding `0 14px`, mark–name gap 10, `Fovea` letter-spacing `.01em` (artboard 01 only) | title bar | |
| `Ask anything… ⌘K` control | Monitor tab bar right | h32, padding 0 12, radius 6, `#121315`, border `#26292D`, text 13 `#6A7178`, kbd mono 11 `#4A5056`. README never mentions it |
| Tab-bar right cluster per screen | all screens | 01: Site selector + ⌘K field + avatar. 03: Site + avatar. 04: avatar only. 05: `Last 7 days ▾` + `Export report` (h32, padding 0 14, secondary) + avatar. 06: `Base model: vlm-v4 ▾` + avatar. Gap 10 |
| Site selector | 01, 03 | h32, padding 0 12, gap 8, radius 6, `#121315`, border `#26292D`, 13 `#F2F4F5`, caret `#6A7178` |
| `＋ Add` = h24, padding 0 9, radius 5, 12px `#F2F4F5`; `⚙` = 24×24, radius 5, 12px `#9BA1A8` | camera rail header | README says h28 for Add (see 1b) |
| Rail filter wrapper padding `12 12 0`; tree padding `12 8`, row gap 2 | camera rail | |
| Tree glyph column: 11px, `#4A5056`, gap 8 — for group `▾ ▸` too | camera rail | README implies only `▪` is `#4A5056` |
| Wall toolbar padding `0 20`, gap 8 | | |
| `Overlays` text `#9BA1A8` (Rules text `#F2F4F5`) | wall toolbar | README says "same style"; reads as the off state of a toggle button |
| Layout popup: offset 6px below trigger, right-aligned; padding 6, gap 2; `GRID` label mono **10** padding `8 12 6`; row padding 0 12, gap 16; divider margin `6 4`; auto row padding `2 12 6`; save row padding `2 12 8`; `Save` 12px | | README gives width/row heights only |
| Popup toggle 30×16, radius 8, knob 12, padding 0 2; off `#34383D` / knob `#9BA1A8` | | |
| Wall stripe opacity `.85` (only on the wall; other placeholders opaque) | video wall | |
| Tile chip geometry: top 8 / left 8 / right 8, gap 6; id chip mono 10 `#F2F4F5`; name chip **Sans 11** `#9BA1A8`; state chip gap 5, padding 0 7 | tile | |
| Detection label tab offset `top −19px, left −1.5px`, padding 0 5 | tile | |
| Footer padding `0 10 8`, items bottom-aligned | tile | |
| Detection box geometry per tile (% top/left/w/h): CAM-01 30/22/26/44 `person 0.94`; CAM-04 42/46/30/38 `forklift 0.88`; CAM-02 34/14/18/40 `person 0.91`; CAM-09 46/52/34/30 `vehicle 0.96`; CAM-18 38/30/22/42 `person 0.83`; CAM-03 58/54/30/22 `plate 0.97` | tile | mock data only |
| Tile border `#34383D` only when a box is drawn; offline always `#26292D` | tile | |
| Feed row: bar radius 2, content gap 5, title/time baseline row gap 8, chip row gap 6; cam chip h19 padding 0 6 `#1A1C1F` border `#26292D` mono 10 `#9BA1A8` | live event feed | |
| Feed footer buttons h32 | | README gives padding only |
| First feed row tinted unconditionally (`i === 0`) | feed data | README: "when critical". Implement README |
| Severity `RESOLVED` = `#4FB286` / `rgba(79,178,134,.14)` | alert table data | README lists critical / review / info only |
| Dialog `✕` mono **13** (title bar `✕` is 12) | dialog header | |
| Dialog tab list padding `12 8`, gap 2; selected row weight **500** | | |
| Credentials row columns `1fr 1fr 140px 120px`, gap 14; all form grids gap 14; label→field gap 6 | dialog | README: "4-column row" |
| Password value mono 13 `#9BA1A8`; `show` mono 11 `#6A7178` | dialog | |
| Test block padding 14, gap 16; frame border `#26292D`; caption mono **10**; metric grid gap `6 20`; button row bottom-aligned | dialog | |
| Dialog toggles 34×18, radius 9, knob 14, padding 0 2; on `#5AA9D6` / knob `#07131A` | dialog, concept status, design sheet | README only ever states 30×16 |
| Footer buttons: Remove h32 padding 0 12; Cancel padding 0 16; Save padding 0 18 | dialog | |
| Button samples: h32, padding 0 16, radius 6, 13px; primary 600 `#5AA9D6`/`#07131A`; secondary `#1A1C1F` border `#34383D` `#F2F4F5`; ghost `#9BA1A8` no fill; destructive 600 `#E0603C`/`#160703`; disabled `#121315` border `#26292D` `#4A5056` | design sheet | README names variants but never specs them |
| Chip samples h22, padding 0 8, mono 11; neutral `CAM-04` chip `#1A1C1F` border `#34383D` | design sheet | three neutral-chip variants exist: h22/`#34383D` (sheet), h19/`#26292D` (feed, rule cards), h24/`#34383D` mono 11 (rule editor camera chips) |
| Checkbox 16×16, radius 4, `#5AA9D6`, `✓` 11px `#07131A` | design sheet | not in README, not used on any screen |
| `focus ring 1px #5AA9D6` | design sheet metrics | README describes focus only as "border accent" |
| Search: results header row (`RESULTS` label, 1px `#1A1C1F` rule, `Sorted by confidence` 12 `#9BA1A8`); query block padding `24 32 18`; results padding `20 24`; card body padding `10 12 12` gap 7 | 03 | |
| Alert log: sub-tab padding 0 14; status tabs h28 padding 0 12; toolbar selects h28; rule list padding 8 gap 4; editor labels **11px**; editor inputs h32 padding 0 10 on `#0A0B0C`; textarea padding `9 10`; pager gap 4 | 04 | |
| Statistics: content padding `24 32`, gap 20; KPI gap 16; lower grid `1fr 420px` gap 16; legend squares 8×8 radius 2; bar column gap 8, segment gap 2, segment radii `2 2 0 0` / `0 0 2 2`; bar heights = seed×1.6 % (people) and seed×0.7 % (vehicles); busiest-camera count column 70px right-aligned; ask field h38 on `#0A0B0C` border `#34383D`; answer card padding 14 radius 6 | 05 | |
| Model Train: workspace padding `16 20` gap 14; region label h18 (detection label is h17), offset `−20/−1.5`; resize handle 7×7 radius 1 at `−4/−4`; filmstrip inner h26 stripes `90deg #1A1C1F 0 4px, #141618 4px 8px`; selection left 34 % width 22 %; handles at top 13; ticks mono 11; segmented control border `#34383D`, halves padding 0 16; example badge 18×18 radius 4 mono 11 at `6/6`; caption bar padding `4 6`; version rows padding `8 10` radius 6, non-current border `#1A1C1F`; `Train adapter` h36 | 06 | |
| Design-sheet metrics: `space 4 · 8 · 12 · 16 · 24 · 32 · 40`, `radius 4 · 6 · 8 · 12`, `rail 248–400 · inspector 340–360 · row 40` | DS | see 1b |

### 1b. Contradictions

| Topic | README | HTML | Resolve as |
| --- | --- | --- | --- |
| Camera tree row height | 30 | artboard 01: 30. Design sheet: `row 40`. Qt notes card: `Row height 40 (rail)` | **30** (artboard) |
| `＋ Add` button height | h28 | 24 | **24** |
| Space scale | 2 4 6 8 10 12 14 16 20 24 32 40 | sheet: 4 8 12 16 24 32 40; artboards also use 3 5 7 9 11 18 | tokens carry the full set actually used |
| Radius scale | 4 5 6 7 8 12 50 % | sheet: 4 6 8 12; artboards also use 1 2 9 | add 1, 2, 9 (pills/badges) |
| Minimum text size | ≥ 10, body ≥ 12 | Qt notes card: "nothing below 12px"; artboards use Sans 11 (tile name chip, detection summary, rule-editor labels, `show`, `＋ Add` chip) | README wins; Sans 11 stays. Design owner should confirm the notes card is stale |
| Button heights | 30–34 | 24 26 28 30 32 34 36 38 40 | size variants xs 24 · sm 28 · md 30 · lg 32 · xl 34 · 36 / 38 / 40 one-offs |
| Input heights | 34 | 30 (rail filter), 32 (rule editor), 34, 38 (teach / ask fields), 56 (query bar) | variants |
| Chip heights | 19–24 | 17 (tab badge) 18 (± badge) 19 20 22 24 | variants |
| Toggle size | 30×16 | 30×16 (popup, rule cards) and 34×18 (dialog, concept status, sheet) | two sizes: `md` 34×18 knob 14, `sm` 30×16 knob 12 |
| Section label in dropdown | mono label 11 | `GRID` mono 10 | 10; add "popup section header" to the mono-micro role |
| Placeholder caption size | mono 11 | 11 on wall tiles and clip viewport; 10 on test frame, search thumbs, players | 11 for surfaces ≥ 190 px tall, 10 below |
| LIVE dot pulse | pulses 2 s ease-in-out | `@keyframes pulseLive` (1 → .35 → 1) declared, never applied | implement README (opacity 1 → .35 → 1, 2 s) |
| Match badge fill | accent | `rgba(90,169,214,.9)` | `.9` alpha over the thumbnail |
| Newest feed row tint | when critical | index 0 always | README |
| "24 stacked bar pairs" | pairs | 24 columns, each two stacked segments (people top accent, vehicles bottom `#34383D`) | stacked columns |
| Tab-bar right side | "site selector, date range, base model selector and avatar" | varies per screen; Alert log has avatar only; Monitor adds the ⌘K launcher | per-screen table in 1a |
| `#E0603C` never decorative | rule | Alert-breakdown category dots use `#E0603C`, `#E2A43C`, `#5AA9D6` as category colours | tension; keep as severity-linked category colour, flag to design |
| Tile detection border | `#34383D` when it has a detection | only while a box is drawn; offline never | same thing; note "detection" means a drawn box |
| Wireframes | six screens | five (no Statistics) | ignore |

## 2. Component trees → Qt Widgets

Conventions: `Class#objectName`. Sizes in px. "mono" = IBM Plex Mono, otherwise IBM Plex Sans. `label` = mono 11, letter-spacing .12em, uppercase, `#6A7178`. Letter-spacing in absolute px: .12em@11 = 1.32, .1em@11 = 1.1, .1em@10 = 1.0, −.02em@30 = −0.6, −.02em@28 = −0.56.

Glyphs (exact code points): `—` U+2014, `▢` U+25A2, `✕` U+2715, `▾` U+25BE, `▸` U+25B8, `▪` U+25AA, `＋` U+FF0B, `⚙` U+2699, `▶` U+25B6, `‹` U+2039, `›` U+203A, `×` U+00D7, `·` U+00B7, `⌘` U+2318, `✓` U+2713, `−` U+2212, `–` U+2013, `…` U+2026, `•` U+2022, `→` U+2192, `≈` U+2248, `≥` U+2265. Plex coverage of the geometric-shape and dingbat code points (U+25A2, U+25AA, U+25B6, U+25B8, U+25BE, U+2699, U+2715, U+FF0B, U+2318) is not verified here (no font file on disk); where a glyph is missing Qt substitutes a per-OS fallback font and the optical size drifts. README asks for the codebase icon set at the same optical size, so `▢ ✕ ▾ ▸ ▪ ＋ ⚙ ▶ ‹ ›` should become SVG icons (Qt Svg is already linked) and only `—` `×` `·` `−` `–` `…` `•` `→` `≈` `≥` stay as text.

### 2a. Global chrome (all screens)

| Component | Qt | Spec |
| --- | --- | --- |
| `MainWindow` | `QMainWindow`, `Qt::FramelessWindowHint`, min 1440×900, design 1920×1080 | central `QWidget#AppRoot`, `QVBoxLayout` 0/0: TitleBar 40 · MainTabBar 52 · `QStackedWidget#ScreenStack` |
| `TitleBar#TitleBar` | `QWidget`, fixed h40 | bg `#050607`, border-bottom 1 `#1A1C1F`, padding 0 14. Left `QHBoxLayout` gap 10: `QWidget#BrandMark` 16×16 radius 4 `#5AA9D6`; `QLabel#BrandName` "Fovea" 13/600 `#F2F4F5`. Right gap 18: `QToolButton#WinMinimize` `—`, `#WinMaximize` `▢`, `#WinClose` `✕`, mono 12 `#6A7178`, flat. Bar = drag handle (`QWindow::startSystemMove`); double-click → maximise (convention, not in spec) |
| `MainTabBar#MainTabBar` | `QWidget` fixed h52 | bg `#0A0B0C`, border-bottom 1 `#1A1C1F`, padding 0 20; `QHBoxLayout`: `NavTabs` · stretch · `#TabBarRight` |
| `NavTabs#NavTabs` | `QTabBar` subclass, `drawBase(false)`, `expanding(false)`, custom `paintEvent` | tabs Monitor · Search · Alerts & Analytics · Model Train; tab padding 0 16, gap 4, full 52 height; inactive 13/400 `#9BA1A8`; active 13/600 `#F2F4F5` + 2px `#5AA9D6` line inset at the bottom edge. Count badge after tab 3 label: gap 7, h17, min-w 17, padding 0 5, radius 9, fill `#E0603C`, mono 10 `#160703`; hidden when count = 0 |
| `#TabBarRight` | `QWidget`, `QHBoxLayout` gap 10 | screen-scoped; each screen installs its own controls (1a table). Shared: `QToolButton[role="select"]#SiteSelector` (h32 padding 0 12 gap 8 radius 6 `#121315` border `#26292D` 13 `#F2F4F5`, caret `▾` `#6A7178`); `QToolButton#QueryLauncher` "Ask anything…" 13 `#6A7178` + `⌘K` mono 11 `#4A5056`, same box; `QLabel#Avatar` 32×32 radius 16 `#1A1C1F` border `#34383D` initials 12 `#9BA1A8` |
| `SubTabBar#SubTabBar` (screens 04/05) | `QWidget` h44 | bg `#0A0B0C`, border-bottom `#1A1C1F`, padding 0 20, gap 6; `QToolButton[role="subtab"]` h28 padding 0 14 radius 5; checked `#1A1C1F` border `#34383D` 13/500 `#F2F4F5`; unchecked transparent 13/400 `#9BA1A8` |

### 2b. Screen 1 — Monitor

`MonitorScreen#MonitorScreen` `QWidget`, `QHBoxLayout` 0/0: `CameraRail` 248 fixed · `WallArea` stretch · `LiveEventFeed` 340 fixed. Tile size at 1920×1080 ≈ 429×299, at 1440×900 ≈ 269×239 (not aspect-locked; frame fit mode contain vs cover is unspecified, decide before M1 UI).

| Component | Qt | Spec |
| --- | --- | --- |
| `CameraRail#CameraRail` | `QWidget` fixed w248, `QVBoxLayout` 0/0 | bg `#0A0B0C`, border-right 1 `#1A1C1F` |
| `#RailHeader` | `QWidget` h48 | padding 0 16, border-bottom `#1A1C1F`. `QLabel[role="section-label"]#RailTitle` "CAMERAS". Right gap 6: `QPushButton[role="secondary"][size="xs"]#AddCameraButton` "＋ Add" h24 padding 0 9 radius 5 `#121315` border `#26292D` 12 `#F2F4F5`; `QToolButton#CameraSettingsButton` `⚙` 24×24 radius 5 same fill 12 `#9BA1A8`. Both open CameraSettingsDialog |
| `QLineEdit#CameraFilter` | in a wrapper padded 12 12 0 | h30 radius 6 `#121315` border `#26292D` padding 0 10, 12px, placeholder "Filter cameras…" `#6A7178`; focus border `#5AA9D6` |
| `CameraTree#CameraTree` | `QTreeView` + `CameraTreeModel` (`QAbstractItemModel`, groups → cameras) + `CameraTreeDelegate` | viewport padding 12 8; indentation 0, no branch decoration, header hidden, frameless, no hover. Row 30 (+2 gap → 32 sizeHint, rounded rect 30 tall inside), radius 5, padding 0 8, 13px. Glyph column 11px `#4A5056` gap 8: group `▾` (open) / `▸` (collapsed), text `#F2F4F5`, no dot; camera `▪`, text `#9BA1A8`. Selected row `#1A1C1F`. Right end dot 6×6 circle: online `#4FB286`, offline `#6A7178`. Mock rows: Harbour District ▾ · North Gate (selected) · Loading Dock B · Perimeter West · Fence Line NE (offline) · Rail Siding · Terminal A ▾ · Terminal Lobby · Server Corridor · Parking P2 · Main Gate ANPR · Berth 4 — East ▸ |
| `WallArea#WallArea` | `QWidget`, `QVBoxLayout` 0/0 | bg `#050607` |
| `WallToolbar#WallToolbar` | `QWidget` h48 | bg `#0A0B0C`, border-bottom `#1A1C1F`, padding 0 20, right-aligned `QHBoxLayout` gap 8 |
| `QToolButton#RulesButton` | custom paint (three colours) | "Rules: 4 applied ▾" h30 padding 0 12 gap 8 radius 6 `#121315` border `#26292D` 13; prefix `#F2F4F5`, value `#5AA9D6`, caret `#6A7178` |
| `QToolButton#OverlaysButton` | checkable | "Overlays" h30 padding 0 12 radius 6 `#121315` border `#26292D` 13 `#9BA1A8` (state drawn = off; on-state not specified) |
| `QToolButton[role="select"]#LayoutButton` | checkable, opens popup | "Layout: 3×3 ▾" h30 padding 0 12 gap 8 radius 6; open: `#1A1C1F` border `#5AA9D6` 13 `#F2F4F5` caret `#5AA9D6`; closed: assume Rules-button style (not drawn) |
| `LayoutPopup#LayoutPopup` | `QWidget` `Qt::Popup`, translucent top-level (shadow) | w236, 6px below trigger, right edges aligned; `#121315` border `#34383D` radius 8, shadow 0 16 40 `rgba(0,0,0,.6)`, padding 6, gap 2. `QLabel#PopupSectionLabel` "GRID" mono 10 .12em uppercase `#6A7178` padding 8 12 6. `QListWidget#LayoutPresetList` + delegate: rows h32 padding 0 12 radius 5, label 13 left, hint mono 11 right; unselected `#9BA1A8` / `#4A5056`; selected `#1A1C1F` fill, `#F2F4F5` / `#5AA9D6`. Items `2 × 2` `4 tiles` · `3 × 3` `9 tiles` · `4 × 3` `12 tiles` · `1 + 5` `hero + row`. `QFrame#PopupDivider` 1px `#26292D` margin 6 4. Row "Auto — follow alerts" 13 `#9BA1A8` + `ToggleSwitch[size="sm"]` 30×16 (padding 2 12 6). Row "Save as default" 13 `#9BA1A8` + `QPushButton[role="link"]` "Save" 12 `#5AA9D6` (padding 2 12 8). Closes on outside click / Esc; fade+translate 120–160 ms ease-out |
| `VideoWall#VideoWall` | `QWidget`, `QGridLayout` margins 12, spacing 10, equal row/column stretch | presets 2×2 · 3×3 · 4×3 · 1+5 (hero geometry not drawn; define later). Double-click tile → maximise; drag → reorder; tiles bind to the sub stream |
| `VideoTile` | `QWidget`, rounded clip on the container (radius 8), `QVBoxLayout` 0 | bg `#0E0F11`, border 1 `#26292D`, `#34383D` while a detection box is drawn. Children: `FrameSurface` (paints the ring frame; placeholder = 115° stripes `#141618` 0–3 / `#0E0F11` 3–9 at opacity .85 + caption mono 11 `#3A4046` .1em `CAMERA FEED` / `SIGNAL LOST`) and `TileOverlay` (transparent, repaints on status tick) |
| `TileOverlay` chrome | painted | Top row inset 8/8/8, left gap 6: id chip h20 padding 0 7 radius 4 `rgba(5,6,7,.72)` mono 10 `#F2F4F5` (`CAM-04`); name chip same box Sans 11 `#9BA1A8` (`Loading Dock B`). Top-right state chip same box, gap 5: 5×5 dot + mono 10; `LIVE` `#4FB286` (dot opacity 1 → .35 → 1 over 2 s ease-in-out) or `NO SIGNAL` `#6A7178`. Detection box: 1.5px `#E0603C` radius 2 at % geometry; label tab anchored top −19 / left −1.5, h17 padding 0 5 fill `#E0603C` mono 10 `#160703` (`person 0.94`). Footer: h34, linear gradient 180° `rgba(5,6,7,0)` → `rgba(5,6,7,.9)`, padding 0 10 8, bottom-aligned; left Sans 11 `#9BA1A8` (detection summary, or `last frame 11 min ago` offline); right mono 10 `#6A7178` `25 fps · H.265` or `—` |
| `LiveEventFeed#LiveEventFeed` | `QWidget` fixed w340, `QVBoxLayout` 0/0; hideable (mock prop `showEventFeed`) | bg `#0A0B0C`, border-left `#1A1C1F` |
| `#FeedHeader` | `QWidget` h48 | padding 0 16, border-bottom `#1A1C1F`; `QLabel[role="section-label"]` "LIVE EVENTS"; `QPushButton[role="link"]` "Filter" 12 `#5AA9D6` |
| `QListView#EventList` + `EventRowDelegate` | model = ring buffer newest first | row padding 12 16, border-bottom 1 `#141618`, gap 10 after severity bar; bar 3 wide × full row, radius 2: critical `#E0603C`, review `#E2A43C`, info `#5AA9D6`. Content gap 5: title 13/500 `#F2F4F5` + time mono 11 `#6A7178` right (baseline row, gap 8); detail 12 `#9BA1A8`; chip row gap 6: camera chip h19 padding 0 6 radius 4 `#1A1C1F` border `#26292D` mono 10 `#9BA1A8`; severity chip h19 padding 0 6 radius 4 mono 10 colour + `.14` tint. Newest critical row bg `rgba(224,96,60,.06)`. Row ≈ 86 with one detail line |
| `#FeedFooter` | `QWidget` | border-top `#1A1C1F`, padding 12 16, gap 8; `QPushButton[role="secondary"]` "Acknowledge" h32 radius 6 `#1A1C1F` border `#34383D` 13 `#F2F4F5`; `QPushButton[role="primary"]` "Open case" h32 `#5AA9D6` 13/600 `#07131A`; both stretch 1 |

### 2c. Screen 2 — Camera settings dialog

| Component | Qt | Spec |
| --- | --- | --- |
| `OverlayScrim#OverlayScrim` | `QWidget` child of MainWindow, raised while a dialog is open | fill `rgba(5,6,7,.86)` |
| `CameraSettingsDialog#CameraSettingsDialog` | `QDialog`, frameless, translucent background, modal | w880, height content-driven (≈ 660 for Connection), `#0A0B0C`, border 1 `#34383D`, radius 12, shadow 0 24 60 `rgba(0,0,0,.6)`; `QVBoxLayout` 0/0: header · body · footer. Esc closes |
| `#DialogHeader` | `QWidget` h52 | border-bottom `#1A1C1F`, padding 0 20; `QLabel#DialogTitle` "Camera settings" 15/600 `#F2F4F5`; `QLabel#DialogSubtitle` `CAM-04 · Loading Dock B` mono 11 `#6A7178`, gap 10, baseline-aligned; `QToolButton#DialogClose` `✕` mono 13 `#6A7178` |
| `QListWidget#DialogTabList` | fixed w180 | border-right `#1A1C1F`, padding 12 8, gap 2; items h32 padding 0 12 radius 6 13px: selected `#1A1C1F` 500 `#F2F4F5`; others `#9BA1A8`. Items: Connection · Stream · Analytics · Recording · Placement |
| `QStackedWidget#DialogPages` | one page per tab | `ConnectionPage` below; Recording page has no design (see §5); Stream / Analytics / Placement = NotImplementedState |
| `ConnectionPage` | `QWidget` padding 20, `QVBoxLayout` gap 18 | rows below. `FormField` pattern: `QLabel[role="field-label"]` 12 `#6A7178`, gap 6, control h34 radius 6 `#121315` border `#26292D` padding 0 12 13 `#F2F4F5`; focus border `#5AA9D6` |
| Row 1 | `QGridLayout` 1fr 1fr gap 14 | `QLineEdit#DisplayName` "Loading Dock B"; `QComboBox#GroupSelect` "Harbour District" caret `▾` `#6A7178` (editable: groups are free text) |
| Row 2 | grid 180px 1fr gap 14 | `QComboBox#ProtocolSelect` "RTSP"; main URL field with label row (`Main stream URL` + `QPushButton[role="link"]#DiscoverOnvif` "Discover via ONVIF" 12 `#5AA9D6` right); `QLineEdit[mono="true"]#MainUrl` mono 12 `#F2F4F5`, drawn focused (border `#5AA9D6`), `rtsp://10.4.18.62:554/Streaming/Channels/101` |
| Row 3 | full width | label `Sub stream URL` + span `— used for the wall grid` `#4A5056`; `QLineEdit[mono="true"]#SubUrl` mono 12 `#9BA1A8` `rtsp://10.4.18.62:554/Streaming/Channels/102` |
| Row 4 | grid 1fr 1fr 140px 120px gap 14 | `QLineEdit#Username` "svc_vms" 13; `QLineEdit#Password` echo mode password, mono 13 `#9BA1A8` (`••••••••••`) + trailing `QToolButton#RevealPassword` "show" mono 11 `#6A7178`; `QComboBox#TransportSelect` "TCP"; `QLineEdit#Timeout` "8 s" (or `QSpinBox` suffix ` s`) |
| `QFrame#TestBlock` | `QHBoxLayout` gap 16 | padding 14 radius 8 `#121315` border `#26292D`. `TestFrame` 200×118 radius 6 `#0E0F11` border `#26292D`, stripes, caption mono 10 `#3A4046` .1em `TEST FRAME`. Right `QVBoxLayout` gap 10: status row gap 10 → `StatusChip[tone="positive"]` `CONNECTED` h22 padding 0 8 radius 4 `rgba(79,178,134,.14)` `#4FB286` mono 11 with 5×5 dot gap 6, `QLabel` `handshake 184 ms` mono 11 `#6A7178`; metric grid 2 cols gap 6 20, 12px, key `#6A7178` left, value mono `#F2F4F5` right: Resolution `1920×1080` · Codec `H.265` · Frame rate `25 fps` · Bitrate `4.2 Mbps`; button row bottom-aligned gap 8: `QPushButton[role="secondary"][size="md"]#TestConnection` h30 padding 0 12; `QPushButton[role="ghost"]#CopyFfmpeg` "Copy ffmpeg command" h30 13 `#9BA1A8` |
| Toggle rows | `QVBoxLayout` gap 10 | label 13 `#F2F4F5` left, `ToggleSwitch[size="md"]` 34×18 right: on `#5AA9D6` knob 14 `#07131A`, off `#34383D` knob `#9BA1A8`, padding 0 2. "Run analytics on this camera" (on), "Record continuously" (on) |
| `#DialogFooter` | `QWidget` h60 | border-top `#1A1C1F`, padding 0 20. Left `QPushButton[role="destructive-ghost"]#RemoveCamera` "Remove camera" h32 padding 0 12 13 `#E0603C`. Right gap 8: `QPushButton[role="secondary"]#CancelButton` h32 padding 0 16; `QPushButton[role="primary"]#SaveCamera` "Save camera" h32 padding 0 18 13/600 |

## 3. QSS token sheet

### 3a. Colour tokens

| Token | Value | Source |
| --- | --- | --- |
| `--bg-deep` | `#050607` | README |
| `--bg-app` | `#0A0B0C` | README |
| `--bg-panel` | `#121315` | README |
| `--bg-raised` | `#1A1C1F` | README |
| `--bg-video` | `#0E0F11` | HTML only |
| `--line` | `#26292D` | README |
| `--line-strong` | `#34383D` | README |
| `--line-quiet` | `#1A1C1F` | README |
| `--line-row` | `#141618` | README (also the stripe light band) |
| `--text-primary` | `#F2F4F5` | README |
| `--text-secondary` | `#9BA1A8` | README |
| `--text-muted` | `#6A7178` | README |
| `--text-disabled` | `#4A5056` | README |
| `--text-placeholder-caption` | `#3A4046` | HTML |
| `--accent` | `#5AA9D6` | README |
| `--accent-ink` | `#07131A` | README |
| `--critical` | `#E0603C` | README |
| `--critical-ink` | `#160703` | README |
| `--warning` | `#E2A43C` | README |
| `--positive` | `#4FB286` | README |
| `--positive-ink` | `#04170F` | README |
| `--tint-accent` | `rgba(90,169,214,.14)` | README |
| `--tint-critical` | `rgba(224,96,60,.14)` | README |
| `--tint-warning` | `rgba(226,164,60,.14)` | README |
| `--tint-positive` | `rgba(79,178,134,.14)` | README |
| `--tint-neutral` | `rgba(106,113,120,.16)` | README |
| `--tint-critical-row` | `rgba(224,96,60,.06)` | README |
| `--tint-critical-badge` | `rgba(224,96,60,.16)` | HTML (MISSED badge) |
| `--tint-accent-range` | `rgba(90,169,214,.16)` | README (timeline selection) |
| `--accent-badge` | `rgba(90,169,214,.9)` | HTML (match badge) |
| `--border-positive-soft` | `rgba(79,178,134,.4)` | README (positive thumbnails) |
| `--scrim-chip` | `rgba(5,6,7,.72)` | README |
| `--scrim-badge` | `rgba(5,6,7,.8)` | HTML |
| `--scrim-caption` | `rgba(5,6,7,.82)` | HTML |
| `--scrim-controls` | `rgba(5,6,7,.85)` | HTML |
| `--scrim-backdrop` | `rgba(5,6,7,.86)` | README |
| `--scrim-footer-end` | `rgba(5,6,7,.9)` (from `rgba(5,6,7,0)`) | README |
| `--shadow` | `rgba(0,0,0,.6)` — dialog 0 24 60, dropdown 0 16 40 | README |
| `--stripe-dark` / `--stripe-light` | `#0E0F11` / `#141618`, 115°, 3 / 6 px bands | README |
| `--filmstrip-a` / `--filmstrip-b` | `#1A1C1F` / `#141618`, 90°, 4 / 4 px | HTML |

Type tokens: `--font-ui` IBM Plex Sans, `--font-mono` IBM Plex Mono. Sizes 10 11 12 13 14 15 17 20 28 30. Weights 400 500 600. Radii 1 2 4 5 6 7 8 9 12 16 (avatar). Fixed heights: title 40 · tab bar 52 · sub-tab 44 · toolbar / panel header 48 · dialog header 52 · dialog footer 60 · table header 34 · tree row 30 · alert row 76 · rail footer 44. Fixed widths: camera rail 248 · rules rail 400 · concepts rail 320 · inspector / detail / feed 340 · concept status 360 · statistics ask column 420 · dialog 880 · dialog tab list 180 · layout popup 236.

### 3b. Recommended selectors

Regions use `objectName`; reusable controls use dynamic properties (`role`, `size`, `tone`, `mono`). Changing a dynamic property at runtime needs `unpolish`/`polish`.

| Selector | Styles |
| --- | --- |
| `QWidget#AppRoot` | bg `--bg-app`, color `--text-primary`, font Sans 13 |
| `#TitleBar`, `#MainTabBar`, `#SubTabBar`, `#WallToolbar`, `#RailHeader`, `#FeedHeader`, `#DialogHeader`, `#DialogFooter`, `#FeedFooter` | fixed heights, bg, 1px `--line-quiet` edge via `border-bottom` / `border-top` |
| `#CameraRail`, `#LiveEventFeed`, `#DialogTabList` | fixed width, `border-right` / `border-left` `--line-quiet` |
| `#WallArea`, `#VideoWall` | bg `--bg-deep` |
| `QLabel[role="section-label"]` | mono 11 `--text-muted` (letter-spacing and uppercase set in code) |
| `QLabel[role="field-label"]` | Sans 12 `--text-muted` |
| `QLabel[role="hint"]` | Sans 12 `--text-disabled` |
| `QLabel[role="mono"]` | mono; size via `[size="10"]`, `[size="11"]`, `[size="12"]` |
| `QPushButton[role="primary"]` | bg `--accent`, color `--accent-ink`, 600, radius 6, no border |
| `QPushButton[role="secondary"]` | bg `--bg-raised`, border 1 `--line-strong`, color `--text-primary` |
| `QPushButton[role="secondary"][size="xs"]` | bg `--bg-panel`, border `--line`, 12px (rail header variant) |
| `QPushButton[role="ghost"]` | transparent, color `--text-secondary` |
| `QPushButton[role="destructive"]` | bg `--critical`, color `--critical-ink`, 600 |
| `QPushButton[role="destructive-ghost"]` | transparent, color `--critical` |
| `QPushButton[role="link"]` | transparent, 12px, color `--accent`, no padding |
| `QPushButton:disabled` | bg `--bg-panel`, border `--line`, color `--text-disabled` |
| `[size="xs"]` … `[size="xl"]` (xs, sm, md, lg, xl) | min/max height 24 / 28 / 30 / 32 / 34; padding 0 9 / 0 12 / 0 12 / 0 16 / 0 18 |
| `QToolButton[role="select"]` | bg `--bg-panel`, border `--line`, radius 6, padding 0 12, 13 `--text-primary`; `:checked` bg `--bg-raised` border `--accent` (open dropdown) |
| `QToolButton[role="subtab"]` | h28 padding 0 14 radius 5; `:checked` bg `--bg-raised` border `--line-strong` 500 `--text-primary`; else transparent `--text-secondary` |
| `QToolButton[role="status-tab"]` | h28 padding 0 12 radius 5, same checked rule (alert toolbar) |
| `QLineEdit`, `QComboBox`, `QPlainTextEdit` | h34 radius 6 bg `--bg-panel` border `--line` padding 0 12 13; `:focus` border `--accent`; placeholder `--text-muted`; `[mono="true"]` mono 12; `[size="sm"]` h30 12px padding 0 10; `[surface="app"]` bg `--bg-app` (rule editor) |
| `QComboBox::drop-down` / `::down-arrow` | no border; arrow = `▾` SVG 8px `--text-muted` |
| `QLabel[role="chip"][tone="…"]` with tone critical, review, info, positive, neutral | h19–22, padding 0 6–8, radius 4, mono 10–11, colour + `.14` tint (`neutral`: `--tint-neutral` `--text-muted`) |
| `QLabel[role="chip"][tone="id"]` | bg `--bg-raised`, border `--line` (h19) or `--line-strong` (h22/24), `--text-secondary` |
| `QListWidget#DialogTabList::item` | h32 padding 0 12 radius 6; `:selected` bg `--bg-raised` 500 `--text-primary`; else `--text-secondary` |
| `QListWidget#LayoutPresetList::item` | h32 radius 5; `:selected` bg `--bg-raised` |
| `QTreeView#CameraTree`, `QListView#EventList` | transparent bg, no frame, `::item` transparent (delegates paint); `selection-background-color` transparent |
| `QLabel#Avatar` | 32×32, radius 16, bg `--bg-raised`, border `--line-strong`, 12 `--text-secondary` |
| `QFrame[role="card"]` | bg `--bg-panel`, border `--line`, radius 8 |
| `QFrame[role="divider"]` | 1px `--line` |
| `QSlider#ThresholdSlider` | groove h4 radius 2 `--line`; sub-page `--accent`; handle 14×14 radius 7 `--text-primary` margin −5 0 |
| `QScrollBar:vertical` | design shows none (all overflow hidden); define 6px `--line-strong` thumb on transparent track (unspecified, needs sign-off) |
| `QToolTip` | unspecified; use panel bg / line-strong border / 12px |

### 3c. QSS cannot express these — custom painting or code

| Item | Why | Where |
| --- | --- | --- |
| Chip / tree status dot (5 or 6 px circle before text) | no pseudo-elements | `StatusChip` widget; `CameraTreeDelegate` |
| Tile chrome: chips over video, gradient footer, detection box 1.5px + label tab, placeholder stripes, rounded clip of a live surface, pulsing LIVE dot | overlay over a painted frame; 1.5px borders are non-integer; no keyframes | `TileOverlay::paintEvent`, `QVariantAnimation` 2 s |
| Tab active line + count badge inside the tab | `QTabBar::tab` cannot host a pill | `NavTabs::paintEvent` |
| Detection / region boxes on thumbnails, players, clip viewport | same | overlay paint |
| Toggle switch (34×18 / 30×16, knob slides) | `QCheckBox` indicator is a static image | `ToggleSwitch : QAbstractButton` |
| Delegate rows: severity bar, bottom rule `--line-row`, critical tint, elided detail, chips inside rows | delegate-drawn | `EventRowDelegate`, `AlertRowDelegate`, `RuleCardDelegate`, `ResultCardDelegate` |
| Multi-colour button text (`Rules: 4 applied ▾`) | one colour per widget | paint or QLabel rich text inside a button-like widget |
| `letter-spacing` (.12em / .1em / −.02em / .01em) | not in Qt QSS | `QFont::setLetterSpacing` from Theme |
| `text-transform: uppercase` | not in QSS | uppercase in code |
| `box-shadow` (dialog, popup) | not in QSS | translucent top-level + painted shadow margin or `QGraphicsDropShadowEffect` |
| `line-height` 1.5 / 1.6 / 1.7 on paragraphs | not in QSS | `QTextBlockFormat::setLineHeight` via QTextDocument, or accept default |
| Text eliding | `QLabel` never elides | `ElidedLabel` / delegates |
| Range timeline (filmstrip, selection, handles, IN/OUT labels), region drawing with resize handle, stacked bar chart, busiest-camera bars | custom widgets | `RangeTimeline`, `RegionCanvas`, `HourlyBars` |
| Popup / panel fade+translate 120–160 ms | no transitions | `QPropertyAnimation` |
| Dialog backdrop tint over the main window | a `QDialog` cannot tint its parent | `OverlayScrim` |

## 4. Screens 3–6 — shells only

Regions and widths so layouts can be stubbed. "NI" = implies data that does not exist in M1; render `NotImplementedState` (centered, one sentence 13 `#9BA1A8` + milestone tag mono 11 `#4A5056`, no stripe placeholder, no fake rows, no disabled fake controls).

**Screen 3 — Search** (`SearchScreen`): query block (padding 24 32 18, border-bottom `#1A1C1F`, gap 14): query bar h56 radius 8 `#121315` border `#34383D` (padding 0 8 0 18, gap 14: `ASK` mono 12 accent · query 17 `#F2F4F5` · `＋ Reference image` h32 secondary · `Search` h40 padding 0 20 primary 14/600) · filter row h30 chips + `Save as alert rule` link + stats mono 12 `#6A7178` + `Grid`/`Timeline` switch | results (padding 20 24, header row, 4-col grid gap 14, card thumb 158 + body) | inspector 340 (header 48, player 190 + 34 control bar, body padding 16). NI: everything below the tab bar (SearchSession, EmbeddingRecord, results, cases arrive M4/M5). M1 renders one state for the whole screen; no query bar that cannot search.

**Screen 4 — Alert log** (`AlertsScreen`, sub-tab 1): sub-tab bar 44 | rules rail 400 (header 48: `RULES` + `18 total · 14 active` mono 11 `#4A5056` + `＋ New rule` h28 primary radius 5; list padding 8 gap 4, cards radius 7 padding 12; footer 44 pager 26×26) | alert table (toolbar 48 status tabs h28 + selects h28; header 34 mono 10; rows 76, columns `118 | 1fr | 150 | 130 | 120 | 130`, gap 16, side padding 20; preview 56 radius 5) | alert detail 340 (header 48, player 190, body padding 16, `ACTIVITY` block, two secondary h32 + primary h34). NI: Rule, Event, EventReview, EvidenceRef (M3). Sub-tab bar itself is real chrome.

**Screen 5 — Statistics** (`AlertsScreen`, sub-tab 2): tab-bar right `Last 7 days ▾` + `Export report`; content padding 24 32 gap 20: KPI row 4 cards (padding 18 20, label mono 11, value 30/600 −.02em, delta 13, sub 12) · lower grid `1fr | 420` gap 16: left = detections-by-hour card (flex; 24 stacked columns gap 8; axis mono 11) over busiest-cameras card h230 (label col 190, track h8 radius 4 `#1A1C1F`, count col 70); right = ask-the-estate card (field h38, answer 28/600) over alert-breakdown card (rows 42, dot 6, count / trend mono 12). NI: all KPIs and charts (M3/M4/M5). `Cameras healthy` and `Footage indexed` could later come from `CameraStatus` and segment bytes (M2 metrics) but must not appear alone as a partial dashboard until the click-through-to-clips rule can hold.

**Screen 6 — Model Train** (`TrainScreen`): tab-bar right `Base model: vlm-v4 ▾`; concepts rail 320 (header 48 `CONCEPTS` + `＋ New`; cards padding 11 12 radius 7) | workspace (header 48 name 15/600 + `INSTANT · NO RETRAIN` chip + `Open in Search` + source selector; body padding 16 20 gap 14: clip viewport flex radius 8 with region box and `MISSED BY vlm-v4` badge; range timeline 46 + tick row; label row h38 field + segmented Positive/Negative + `Add example`; examples strip header + 8 thumbs h96 gap 10) | concept status 360 (header 48; live card; meta list; threshold slider; `USE THIS CONCEPT IN` toggles; `VERSIONS` rows; `Train adapter` h36). NI: concepts, examples, adapters, versions, thresholds (after M5).

## 5. Monitor screen — real data in M1

Source: `GET /v1/cameras` → `Camera` + `CameraStatus` (`state connecting|online|reconnecting|offline|disabled`, `stale`, `last_frame_age_ms`, `codec`, `width`, `height`, `fps_new`, `recording recording|paused_disk|disabled|error`), 1 Hz polling (D3). Frames via the shared-memory ring.

| Visible datum | Exists in M1 | Source | Honest rendering in M1 |
| --- | --- | --- | --- |
| Camera id chip `CAM-04` (tile, tree, dialog subtitle, feed chips) | **No** — `Camera.id` is a UUIDv4; no short code field | — | Hide the id chip and the mono subtitle id; name only. Recommend adding a `code` field (unique, editable, default `CAM-nn` by creation order) to the M1 contract; tile/tree/dialog then show it verbatim |
| Camera name | Yes | `Camera.name` | name chip / tree text / `Display name` field |
| Group rows in the tree | Yes | `Camera.group_name` | group rows from distinct `group_name`; expand state client-side; empty group name → ungrouped rows at root |
| Tree status dot | Partly | `CameraStatus.state` | `online && !stale` → `#4FB286`; everything else → `#6A7178`. Design has no colour for connecting / reconnecting / disabled; use muted until design adds one |
| Tree filter field | Yes | client-side over name + group (+ code) | real |
| Selected tree row | Yes | client state | real; selection scrolls / highlights the tile (behaviour unspecified) |
| `LIVE` / `NO SIGNAL` state chip | Yes | `state`, `stale` | `LIVE` `#4FB286` when online and not stale; `NO SIGNAL` `#6A7178` when offline or stale; `CONNECTING` / `RECONNECTING` `#6A7178` for those states (words not in design, colour rule kept); `DISABLED` `#4A5056` for disabled. Design owner to confirm the three added words |
| Placeholder caption | Yes | state, frames | `SIGNAL LOST` when offline / stale after frames were seen; `CONNECTING` before first frame; none once frames paint |
| Pulsing LIVE dot | Yes | derived | pulse only while `LIVE` |
| `25 fps · H.265` | Yes | `fps_new`, `codec` (map GStreamer caps `video/x-h264` → `H.264`, `video/x-h265` → `H.265`) | rounded fps + codec; `—` when no frames or offline |
| `last frame 11 min ago` (offline footer-left) | Yes | `last_frame_age_ms` | show in the footer-left slot when not `LIVE`; `no frames yet` before the first frame |
| Detection summary (`2 people · 1 vehicle`, `clear`, `plate SJK 4412`) | **No** | analytics M3+ | footer-left slot empty while `LIVE` (never `clear`: that asserts an analytic result). The gradient footer stays for fps / codec |
| Detection boxes + label tabs | **No** | M3+ | never drawn; tile border stays `#26292D` |
| Recording state (`recording`, `paused_disk`, `error`) | Yes, but **no design** | `CameraStatus.recording` | must be visible (ARCHITECTURE: disk floor "shows it"). Provisional: footer-left Sans 11 `#E2A43C` `recording paused · disk full` / `recording error`, nothing when recording normally. Needs design sign-off |
| `Rules: 4 applied ▾` | **No** | Rule (M3) | hide the control in M1 (empty dropdown would be fake). Reappears with M3 |
| `Overlays` | **No** | boxes / labels / zones (M3) | hide in M1 |
| `Layout: 3×3 ▾` + presets | Yes | console-local setting (`Setting` entity exists; `docs/API.md` has no settings route → `QSettings` in M1) | real; M1 offers 1×1 (PLAN; not in design, add a row `1 × 1` / `1 tile`), 2×2, 3×3. `4 × 3` and `1 + 5` hidden until the wall supports them (M2+) |
| `Auto — follow alerts` toggle | **No** | alerts (M3) | hide the row (or disabled with hint `needs alerts`); prefer hide |
| `Save as default` | Yes | setting | real |
| Empty wall slots (M1 has 1 camera, M2 has 4) | Yes, **no design** | — | slot with border `#26292D`, bg `#0E0F11`, caption mono 11 `#3A4046` `EMPTY SLOT`, no chips. Needs design sign-off |
| Frame fit inside a non-16:9 tile | unspecified | — | recommend contain (letterbox on `#0E0F11`); decide before M1 UI |
| Double-click maximise, drag reorder | Yes | client state | real; order persisted with the layout setting |
| Live event feed (rows, `Filter`, `Acknowledge`, `Open case`) | **No** | Event (M3) | hide the whole 340 panel in M1 — the mock parameterises exactly this (`showEventFeed`). Do not show an empty feed with disabled buttons. `ReceiveGap` (real) could later feed camera-health rows; not an Event yet, leave out |
| Tab badge `3` on Alerts & Analytics | **No** | Event count | no badge |
| Site selector `Site: Harbour District` | **No** — no site entity | — | hide |
| `Ask anything… ⌘K` | **No** — Search is M4 | — | hide (⌘K unbound) |
| Avatar `MK` | **No** — no operator / auth | — | hide |
| Search / Alerts & Analytics / Model Train tabs | tabs exist, screens NI | — | tabs navigable; each screen = `NotImplementedState` (ARCHITECTURE requires the explicit state) |

Camera settings dialog in M1 (the M1 plan makes Connection + Recording real):

| Field | M1 | Note |
| --- | --- | --- |
| Display name, Group | yes | `name`, `group_name` (editable combo of existing groups) |
| Protocol | yes | `kind` (rtsp or file) → options `RTSP`, `File`; `File` swaps the URL fields for a path picker (no design; keep same field geometry) |
| Main / Sub stream URL | yes | `main_url`, `sub_url`; sub optional in M1 (falls back to main) |
| `Discover via ONVIF` | no | hide the link |
| Username / Password + `show` | yes | sent on POST/PUT only; never returned. After save the password field is empty with placeholder `stored` — do not paint fake dots |
| Transport, Timeout | yes | `transport` (tcp or udp), `timeout_ms` (shown in seconds) |
| Test block | yes | `POST /v1/cameras/test` → `codec`, `width×height`, `fps`, `bitrate_kbps` → Mbps, `handshake_ms`; chip `CONNECTED` / error text in `#E0603C`. The 200×118 frame: endpoint returns no image → caption `NO PREVIEW` until the endpoint returns a thumbnail (recommend adding one) |
| `Copy ffmpeg command` | optional | composable from fields; it puts credentials on the clipboard, D6 redaction applies to logs only. Ship or hide, never a dead button |
| Run analytics on this camera | field exists, no effect | `analytics_enabled` persists; show hint `no effect until analytics ships` (`[role="hint"]`) or disable; do not hide (M3 reads it) |
| Record continuously | yes | `record_enabled` |
| Tabs Stream / Analytics / Placement | no | `NotImplementedState` page; rows stay listed so the geometry is final |
| Tab Recording | real in M1, **no design** | segment length / retention / storage path are provisional; flag to design |
| Remove camera | yes | `DELETE`; segments stay on disk, marked `deleted` |
| Cancel / Save camera | yes | `POST` / `PUT`; PUT restarts the pipeline when connection fields changed |

## 6. Fonts

Required: **IBM Plex Sans** and **IBM Plex Mono** (README, Google Fonts link in the HTML requests Sans 400/500/600 and Mono 400/500).

Weights actually used in the artboards: Sans 400 (body), Sans 500 (event / result / alert titles, concept names, selected dialog tab, selected sub-tab, `Live on 12 cameras`), Sans 600 (headings, primary / destructive buttons, active tab, rule names, KPI values). Mono 400 only — zero occurrences of mono at 500 or 600 (the `Train adapter` mono span forces 400 inside a 600 button). Mono 500 is requested by the font link but unused; bundle it only if a future screen needs it.

Official source: GitHub `IBM/plex` releases (SIL Open Font License 1.1, `LICENSE.txt` at the repo root and inside each package). Since v7 the repo is a monorepo with one release per family (verified via the GitHub API, no files downloaded):

| Family | Latest static release tag | Asset | TTF files to bundle (inside `fonts/complete/ttf/`) |
| --- | --- | --- | --- |
| IBM Plex Sans | `@ibm/plex-sans@1.1.0` (2024-11-13) | `ibm-plex-sans.zip` (≈ 9.5 MiB, all styles and formats) | `IBMPlexSans-Regular.ttf` 196 KiB, `IBMPlexSans-Medium.ttf` 198 KiB, `IBMPlexSans-SemiBold.ttf` 198 KiB |
| IBM Plex Mono | `@ibm/plex-mono@2.5.0` (2026-06-11) | `ibm-plex-mono.zip` (≈ 6.6 MiB) | `IBMPlexMono-Regular.ttf` 169 KiB (optional `IBMPlexMono-Medium.ttf` 170 KiB) |

Same files ship on npm as `@ibm/plex-sans` 1.1.0 and `@ibm/plex-mono` 2.5.0 (`license: OFL-1.1`). Legacy monolithic release `v6.4.0` (`TrueType.zip`) carries identical file names. Variable fonts exist (`@ibm/plex-sans-variable@0.2.0`, `@ibm/plex-mono-variable@1.0.0`) — do not use them; static instances map cleanly to QSS `font-weight: 400 / 500 / 600` and to `QFont::Normal / Medium / DemiBold`.

Bundling notes: ship the four (or five) TTFs plus `LICENSE.txt` (4 KiB, sits next to the TTFs as `license.txt` and at the package root as `LICENSE.txt`) in a Qt resource, ≈ 0.8 MiB total. Call `QFontDatabase::addApplicationFont` at startup before any QSS is applied, then verify `QFontDatabase::styles("IBM Plex Sans")` lists Regular, Medium, SemiBold — on macOS a face can register under a legacy family name (`IBM Plex Sans SemiBold`) and weight 600 then silently falls back to synthetic bold. Extract only the needed TTFs from the zip; never commit the full archive (disk budget).
