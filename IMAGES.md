# libvte with inline images

This is a fork of [VTE](https://gitlab.gnome.org/GNOME/vte) 0.84 that adds
inline image support to the GTK 4 widget. It was made for
[Vmux](https://github.com/turbineBMW/Vmux) but is a drop-in replacement for
`libvte-2.91-gtk4` (same soname, same API plus a few additions), so any
GTK 4 VTE application can use it.

Branch: `image-support`, based on the `0.84.1` tag.

## What is supported

| Protocol | Status |
|---|---|
| SIXEL (DCS q) | Upstream's implementation, restored on the stable branch, now actually painted on GTK 4 and enabled by default. |
| kitty graphics protocol (APC G) | Transmit / display / put / query / delete; formats 24, 32 and 100 (PNG); zlib compression; direct, file, temporary file and shared memory media; chunked uploads; image ids and image numbers; placement ids; source rectangles; `c`/`r` scaling with letterboxing; `X`/`Y` cell offsets; z-index layering (under cell backgrounds, between backgrounds and text, above text); cursor movement policy; Unicode placeholders (virtual placements with `U=1`); all delete variants; responses with `q` suppression. |
| iTerm2 inline images (OSC 1337 File) | `inline=1` images in any format gdk-pixbuf decodes; `width`/`height` in cells, `px`, `%` or `auto`; `preserveAspectRatio`; `doNotMoveCursor`; `MultipartFile`/`FilePart`/`FileEnd`. |

Not implemented: kitty animations (`a=f`, `a=a`, `a=c` answer `ENOTSUP`),
relative placements (`P`/`Q`), and scrolling of images inside a DECSTBM
scroll region (images stay put while the region scrolls, like SIXEL always
did in VTE).

## Behaviour notes

- Images are anchored to rows, scroll with the text into the scrollback,
  and are dropped when their rows leave the scrollback.
- Kitty placements survive text being written over them; only the kitty
  delete commands, `ESC[2J`, leaving the alternate screen and a terminal
  reset remove them. SIXEL and iTerm2 images are treated the way VTE
  always treated SIXEL: a later image that completely covers one erases it.
- Unicode placeholders: a virtual placement (`a=p` or `a=T` with `U=1`,
  which needs an image id) draws nothing by itself. Every U+10EEEE cell
  shows a cell-sized piece of it: the foreground colour (256-colour index
  or 24-bit) is the image id, the third diacritic its high byte, the
  underline colour the placement id (256-colour index only; VTE keeps
  24-bit underline colours at reduced precision, so those fall back to
  the image's newest virtual placement), and the first two diacritics
  the row and column, inferred from the cell to the left when missing.
  The image is fitted into its `c` x `r` cells keeping its aspect ratio,
  and centred. The U+10EEEE glyph itself is never drawn. Virtual
  placements are only removed by id-based deletes (`d=i`/`n`/`r`), by
  re-transmitting the image, and by a reset.
- Kitty image data is kept in a 320 MiB per-terminal store; when it fills
  up, images without placements are evicted first, then the oldest ones.
- Files read for the `t=f`/`t=t`/`t=s` media are validated as the protocol
  demands (regular files only, nothing under `/proc`, `/sys` or `/dev`
  except `/dev/shm`, one uniform error for every failure, temporary files
  are only deleted when they live in a temp dir and contain
  `tty-graphics-protocol` in their name).

## API additions

```c
void     vte_terminal_set_enable_kitty_graphics(VteTerminal*, gboolean);
gboolean vte_terminal_get_enable_kitty_graphics(VteTerminal*);
void     vte_terminal_set_enable_iterm2_images(VteTerminal*, gboolean);
gboolean vte_terminal_get_enable_iterm2_images(VteTerminal*);
```

with matching `enable-kitty-graphics` and `enable-iterm2-images` GObject
properties. Together with the existing `enable-sixel` they all default to
`TRUE`. The `vte-2.91-gtk4` test application gains `--no-sixel`,
`--no-kitty-graphics` and `--no-iterm2-images`.

## Building

```sh
meson setup build -Dgtk3=false -Dgir=false -Dvapi=false -Ddocs=false
ninja -C build
```

The `sixel` meson option (now "inline image support") defaults to `true`.
Building needs `glib2-devel` (for `glib-mkenums`) on Arch in addition to
the usual VTE dependencies. `packaging/arch/PKGBUILD` builds a `vte4-images`
package that replaces `vte4` system-wide; Vmux instead vendors the library
with its `scripts/build-vte.sh`.

## Tests

`meson test -C build` runs upstream's suite plus `test-kitty`, which covers
the protocol layer (control data parsing, base64, RGB/RGBA/PNG/zlib
decoding, file medium rules, the image store and its eviction order).

## Layout of the changes

- `src/parser.hh`, `src/parser-seq.py`: APC strings are collected and
  dispatched as `VTE_CMD_APC`.
- `src/image.{hh,cc}`: the placement model (shared surfaces, source and
  destination rectangles, z-index, protocol ids, GTK 4 texture cache).
- `src/ring.{hh,cc}`: generic image bookkeeping, pruning of scrolled-out
  images.
- `src/drawing-context.hh`, `src/drawing-gsk.cc`, `src/drawing-cairo.cc`:
  `draw_image()`.
- `src/vte.cc`: `draw_images()` in three layers around `draw_rows()`.
- `src/kitty-graphics.{hh,cc}`: protocol parsing, decoding, media, store.
- `src/vte-kitty.cc`: kitty command handling, placement, deletion, replies.
- `src/vte-iterm2.cc`: OSC 1337 handling and the shared
  `place_inline_image()`.
- `src/vtegtk.cc`, `src/vte/vteterminal.h`: the new properties and functions.
