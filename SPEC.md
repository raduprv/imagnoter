# Imagnoter — design spec

*Lightweight native GTK4 YOLO annotator. Binary: `imagnoter`.*

## Overview

A lightweight, native bounding-box annotation tool for YOLO datasets. Single-file
C, GTK4 + Cairo, with no OpenCV / Qt / libadwaita / browser / Docker. Cross-desktop
(GNOME, KDE, XFCE, …) and touch-capable for tablets.

Goal: fill the gap for a genuinely lightweight native annotator — draw and edit
boxes, manage classes, save YOLO-format labels — without dragging a CV library or a
second GUI toolkit along for the ride.

## Stack & build

- C, single file.
- GTK4 (`libgtk-4-dev`). GLib, Cairo and GdkPixbuf come with it. No other dependencies.
- Build: `gcc imagnoter.c $(pkg-config --cflags --libs gtk4) -O2 -lm -o imagnoter`

## Data & files

**Images.** Open a folder via `GtkFileDialog` (portal-native, async). Formats:
jpg / jpeg / png / bmp. Single directory, non-recursive, in raw `readdir` order
(no sort — every image gets annotated regardless of order, so order is irrelevant
to correctness).

**Labels.** YOLO `.txt`, one per image, same basename, **in the same folder as the
image**. Format: `class_id cx cy w h`, normalized to [0,1], 6 decimals. Ultralytics
finds these via its same-folder fallback (it only looks in a sibling `labels/` when
the path contains an `images/` dir). Caveat: if the image folder is literally named
`images`, Ultralytics will look in a sibling `labels/` instead — avoid that name.

**Negative samples.** An empty `.txt` is written for images with no boxes, governed
by the "save empty txt on pass-through" setting (default on). When on, opening an
image and navigating away writes an empty `.txt` (= reviewed, no objects).

**Classes.** `classes.txt` in the image folder — one name per line, line index =
class id. Auto-loaded on open folder if present; classes start empty if absent.
Classes are **append + rename only — never deleted**, so ids stay contiguous and
gap-free, and a class's panel position always equals its id (no tombstones, no
"real id vs visible position" divergence, keys `0–9` map 1:1). An accidentally
added class simply sits unused; the only cost of no-delete is that stray entry,
which harms nothing — it owns no boxes unless you assign them. `classes.txt` is
written immediately on any class change (add / rename), atomically (temp file +
`rename`), so labels never reference an unnamed id even after a crash. The tool
never writes the dataset YAML (the user maintains `names:`). Optional: a "copy
names: block" helper in the menu.

**Adding / naming.** "Add class" opens an inline `GtkEntry`; committing with ≥1
character creates the class, while empty / `Esc` creates nothing — so no unnamed
lines ever land in the file. Rename likewise requires ≥1 character; an empty
rename is rejected and the old name kept.

**Saving.** Auto-save on navigate and on quit; existing labels reloaded on revisit.
Every write is checked (return value + `errno`, including `fclose`); on any failure
(read-only, ENOSPC, permissions, …) the user is notified — never a silent "saved."
Folder writability is checked at open, and read-only folders warn up front.

## Window layout

**Header bar:** panel toggle · Open folder · current folder name · undo / redo · ☰ menu (settings, help, about).

**Left panel** (collapsible via `Ctrl+B` or the toggle button, `GtkRevealer`): the
class list — each row is a color swatch + name. Left-click sets the active class;
right-click / long-press opens a popover (rename); rename is inline (`GtkEntry`).
"Add class" button at the bottom. No delete — classes are never removed.

**Canvas** (`GtkDrawingArea` + Cairo): the image, letterboxed within the current
zoom/pan view; boxes drawn on top in per-class colors, each tagged with the class
**name** (small, colored, at the corner; `h` toggles the tags). The selected box is
highlighted.

**Status overlay:** a translucent strip painted in Cairo along the bottom — image
index N/M, zoom %, unsaved indicator.

**Canvas overlay buttons** (`GtkOverlay`, small, translucent):
- bottom-right: zoom `+` / fit / `-`
- bottom-left: delete and edit modifier buttons (for touch) — short tap = toggle sticky mode, long press = momentary (active only while held)
- all overlay buttons are inert while a box operation is in progress (mutual-exclusion flag).

## View transform

View = scale (zoom) + offset (pan). Fit-to-window is the initial state and the target
of fit/reset (`f`). Boxes are stored only as normalized [0,1] coordinates — the
single source of truth; widget and image coordinates are derived at draw and input
time, so nothing drifts on resize/zoom/pan. Zoom is clamped (min = fit, max ≈ 8–10×).
Wheel zoom centers on the cursor; the +/- buttons center on the viewport.

## Interaction — desktop pointer

**Left button**
- empty image + drag → new box (active class); edges of existing boxes are inert without a modifier, so a new box never accidentally grabs an old one
- drawing requires an active class that is both **selected and named** — with no usable active class the draw gesture is disabled
- a drag smaller than min box size is **discarded**, not created (kills accidental click-sized boxes)
- `Ctrl` + click on a box → select (then `0–9` reassigns its class)
- `Ctrl` + drag on a box body → move; on an edge/corner → resize
- `Shift` + click on a box → delete

**Right button:** drag → pan (no scrollbars).

**Wheel:** zoom toward the cursor.

Overlap disambiguation (select / delete / edit): act on the **smallest box** under
the point.

## Interaction — touch

- one-finger drag → draw / move (per active modifier button)
- two-finger drag → pan
- pinch → zoom
- delete / edit modifier buttons (bottom-left) stand in for Shift / Ctrl
- long-press a class row → rename menu (no delete)
- undo via the header-bar button (no redo button on touch)

## Undo / redo

Snapshot-based. Before each mutating operation (create, delete, move, resize, class
reassign) the current image's full box list is snapshotted into a ring buffer; undo
restores the previous snapshot, redo the next. ≥20 levels (cheap — boxes are tiny, so
50 is fine too). The move/resize snapshot is taken at drag-begin, so one undo reverts
the whole gesture rather than walking back pixel by pixel. History is per-image and is
cleared on navigate. `Ctrl+Z` / `Ctrl+Y`.

## Classes & colors

`0–9` select the first ten classes from the keyboard; the rest via panel click. Each
class has a color — a hand-picked palette for the first ~10, golden-angle hue rotation
beyond — used identically on the box, its name tag, and its panel swatch, so a box is
instantly tied to its class visually. Add and rename (inline) only — no delete, so
ids stay contiguous. User-editable colors: later.

## Settings (☰ menu, persisted)

- edge-grab tolerance (resize vs move)
- save empty `.txt` on pass-through (default on)
- min box size — setting or fixed default (clamp-to-image-bounds is always on, not a setting)

## Persistence

`GKeyFile` ini file under `~/.config/<app>/`. Stores settings plus session state:
last folder, window size, panel collapsed state, and the **current image filename**
(so it reopens where you left off — saved by name, not index, since `readdir` order
isn't stable across runs once images are added or removed).

## Keyboard map

| Key | Action |
|---|---|
| `a` / `←` | previous image |
| `d` / `→` | next image |
| `0`–`9` | set active class (under `Ctrl`, on a selected box: reassign its class) |
| `+` / `=` | zoom in |
| `-` | zoom out |
| `f` | fit / reset zoom |
| `Ctrl` (held) | edit mode (move / resize / select) |
| `Shift` (held) | delete mode |
| `Ctrl+Z` | undo |
| `Ctrl+Y` | redo |
| `Ctrl+S` | force-save current image (+ `classes.txt` if changed) |
| `Ctrl+B` | toggle panel |
| `h` | toggle box labels |
| `F1` / `?` | help / shortcuts |
| `Esc` | cancel current operation |
| `Ctrl+Q` | quit |

## Open items (TODO)

- license (GPL vs MIT)
- README
- tune default edge tolerance & min box size values
