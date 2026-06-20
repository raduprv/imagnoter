# Imagnoter

**Lightweight native GTK4 bounding-box annotator for YOLO datasets.**

![language](https://img.shields.io/badge/C-GTK4-blue)
![platform](https://img.shields.io/badge/platform-Linux-555)
![deps](https://img.shields.io/badge/deps-gtk4%20only-green)

A genuinely small, native annotation tool for drawing YOLO bounding boxes — draw and
edit boxes, manage classes, save YOLO-format labels — without dragging an OpenCV, a
second GUI toolkit, a browser, or a Python runtime along for the ride. One C file,
GTK4, and nothing else.

![Imagnoter](screenshot.png)


## Features

- **One self-contained C file.** Only GTK4 (and its bundled GLib / Cairo / GdkPixbuf). No OpenCV, Qt, libadwaita, Electron, or Python. The optimized binary is ~65 KB.
- **YOLO labels next to images** — `class_id cx cy w h`, normalized, 6 decimals, one `.txt` per image.
- **Draw / move / resize / delete** boxes with mouse or touch; per-class colors shown identically on the box, its name tag, and the panel swatch.
- **Append + rename class management** (no delete) — class ids stay contiguous, so panel position equals id and keys `0–9` map 1:1. `classes.txt` is written atomically on every change.
- **Undo / redo** — snapshot ring buffer, per image, whole gesture per step.
- **Smooth, low-CPU canvas.** The visible region is pre-scaled into a viewport cache, so panning and annotating at fit-to-screen don't re-scale the full image every frame — easy on laptops and batteries.
- **Fit / zoom / pan**, cursor-anchored wheel zoom, and pinch-zoom.
- **Negative samples** — an empty `.txt` is written on pass-through (toggleable), marking an image as reviewed-with-no-objects.
- **Safe saves.** Every write is checked (return value + `errno`, including `fclose`); failures are reported, never a silent "saved." Read-only folders warn up front.
- **Session resume** — reopens the last folder at the image you left off on; settings persist via `GKeyFile`.

## Dependencies

- A C compiler (`gcc` or `clang`)
- GTK 4 development files
  - Debian / Ubuntu / Mint: `sudo apt install libgtk-4-dev`
  - Fedora: `sudo dnf install gtk4-devel`
  - Arch: `sudo pacman -S gtk4`
- `pkg-config`, and `make` (optional, for the Makefile)

GTK ≥ 4.12 is recommended (uses `GtkFileDialog`, `gtk_widget_compute_point`, and `gtk_css_provider_load_from_string`).

## Build & install

```sh
make                      # optimized build -> ./imagnoter
sudo make install         # install to /usr/local/bin
# or, without root:
make PREFIX="$HOME/.local" install
```

Other targets: `make debug` (AddressSanitizer + UBSan), `make run`, `make clean`, `make uninstall`, `make check`.

Or build by hand:

```sh
gcc imagnoter.c $(pkg-config --cflags --libs gtk4) -O2 -lm -o imagnoter
```

## Quick start

1. Run `imagnoter` and click **Open folder**; pick a directory of images.
2. In the left panel, **+ Add class** for each label you need (commit with ≥1 character).
3. Pick the active class (click its row, or press `0`–`9`).
4. **Drag** on the image to draw a box. Navigate with `a` / `d`.

Labels save automatically when you move to another image or quit, and reload when you come back.

## Usage

### Mouse

| Action | Result |
|---|---|
| Drag on empty area | draw a new box (active class) |
| `Ctrl` + click a box | select it (then `0`–`9` reassigns its class) |
| `Ctrl` + drag a box body | move it |
| `Ctrl` + drag a box edge/corner | resize it |
| `Shift` + click a box | delete it |
| Right-button drag | pan |
| Mouse wheel | zoom toward the cursor |

Overlapping boxes disambiguate to the **smallest box** under the pointer.

### Keyboard

| Key | Action |
|---|---|
| `a` / `←` | previous image |
| `d` / `→` | next image |
| `0`–`9` | set active class (`Ctrl`+digit on a selected box: reassign its class) |
| `+` / `=` | zoom in |
| `-` | zoom out |
| `f` | fit / reset zoom |
| `h` | toggle box labels |
| `Ctrl+Z` / `Ctrl+Y` | undo / redo |
| `Ctrl+S` | force-save current image (+ `classes.txt` if changed) |
| `Ctrl+B` | toggle the class panel |
| `Esc` | cancel the current operation |
| `Ctrl+Q` | quit |

### Touch (partial)

Pinch to zoom, and use the bottom-left **edit** / **delete** overlay buttons as sticky
stand-ins for `Ctrl` / `Shift` while drawing or editing with one finger. Two-finger pan
is not implemented yet (see Roadmap) — use zoom controls or a mouse to pan for now.

## Dataset layout

Labels live in the **same folder** as the images, with the same basename:

```
my_dataset/
├── img001.jpg
├── img001.txt        # e.g. "0 0.512 0.431 0.220 0.180"
├── img002.jpg
├── img002.txt
└── classes.txt       # one class name per line; line index = class id
```

- **Label format:** `class_id cx cy w h` — center x/y and width/height normalized to `[0,1]`, 6 decimals, one box per line.
- **`classes.txt`:** auto-loaded on open; line 0 is class id 0, and so on.
- **Ultralytics note:** this same-folder layout works via Ultralytics' fallback. Don't name the image folder literally `images`, or Ultralytics will look for labels in a sibling `labels/` directory instead.

The tool never touches your dataset YAML — you keep `names:` yourself.

## Configuration

Settings live in the **☰ menu** and persist to `~/.config/imagnoter/imagnoter.ini`:

- **Edge-grab tolerance** — how close (in *screen* pixels) the pointer must be to a box edge to resize rather than move. Default 10.
- **Min box edge** — boxes whose width *or* height is below this (in *image* pixels) are discarded on draw and clamped on resize, so a thin 11×100 box is fine but a stray sliver isn't. Default 10.
- **Write empty `.txt` on pass-through** — negative-sample handling. Default on.

A few build-time constants near the top of `imagnoter.c` are worth knowing: `CACHE_MARGIN` (how much extra around the viewport is cached, trading RAM for fewer rebuilds on pan) and the zoom clamp.

## Design

The design rationale — why labels sit next to images, why classes are append-only, the
coordinate model, the rendering cache, and the rest — lives in
[`SPEC.md`](SPEC.md).

## Roadmap

- Two-finger touch pan
- User-editable class colors
- Optional higher-quality downscale filter for the fit view (`GDK_INTERP_HYPER`)

## License
Public Domain

