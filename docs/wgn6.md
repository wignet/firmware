# WGN6 Spectra 6 transport

WGN6 is Wignet's additive six-colour transport for the Waveshare 7.3-inch
Spectra 6 PhotoPainter. It deliberately does not replace the historical BMP
contract.

## Compatibility contract

- Every renderer continues to produce the legacy `.bmp` (and normally `.xbm`).
- A device is `spectra6` when its firmware declares `spectra6` in `caps=` on
  `/schedule`, or when `devices.params.display_profile` says `spectra6` (the
  setting from before firmware could declare it). Otherwise it is `mono1`.
- The existing `.bmp` URL returns BMP unless both conditions are true:
  1. the device is `spectra6`, as above;
  2. the request explicitly accepts `application/vnd.wignet.spectra6`.
- `*/*` never opts a client into WGN6. `.xbm` URLs never negotiate WGN6.
- A missing, stale, wrong-size, corrupt, or unsupported WGN6 artifact falls
  back to the current BMP with status 200. Responses for a Spectra profile use
  `Vary: Accept`, and BMP/WGN6 have content-specific ETags.
- The PhotoPainter firmware accepts either representation. It validates the
  complete WGN6 frame before touching the panel and makes a second request with
  `Accept: image/bmp` if the color response is rejected.

This makes rollout order irrelevant: the dual-format firmware works with an
old server, and the updated server remains byte-for-byte compatible with old
firmware and old devices.

## Version 1 wire format

All integers are little-endian. The fixed 20-byte header is the struct
`<4sBBBBHHII>`:

| Offset | Size | Field | v1 value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `WGN6` |
| 4 | 1 | version | `1` |
| 5 | 1 | header size | `20` |
| 6 | 1 | profile | `1` (Spectra 6) |
| 7 | 1 | packing | `1` (4 bpp, high nibble first) |
| 8 | 2 | width | unsigned pixels |
| 10 | 2 | height | unsigned pixels |
| 12 | 4 | payload length | exactly `ceil(width * height / 2)` |
| 16 | 4 | payload CRC-32 | IEEE/zlib CRC-32 of payload only |

The payload is top-down, row-major, packed continuously across row boundaries.
The first/even pixel is the high nibble. If the total pixel count is odd, the
single unused final low nibble must be white (`1`). Codes `6..15` are invalid.

| Code | Logical color | RGB preview value |
| ---: | --- | --- |
| 0 | black | `#000000` |
| 1 | white | `#ffffff` |
| 2 | green | `#00ff00` |
| 3 | blue | `#0000ff` |
| 4 | red | `#ff0000` |
| 5 | yellow | `#ffff00` |

At 800x480 the payload is 192,000 bytes and the complete frame is 192,020
bytes. `backoffice/wgn6.py` is the reference encoder/decoder; the matching
firmware parser is `esp32/src/wgn6_format.*`.

## Current producer

The `custom` and `image_push` services produce color for Spectra-profile
devices. Both still write the normal monochrome BMP/XBM and additionally write:

- `<output_base>.wgn6` — the quantized panel payload (see "Photos" below);
- `<output_base>.png` — exact six-colour browser preview.

`image_push` keeps its existing fit, orientation, and monochrome dithering
semantics; its color artifact is derived from the stored full-color source and
uses the same fit and orientation. Other services remain monochrome and
therefore fall back naturally to BMP.
When another service updates the BMP, an older color sidecar is rejected by
mtime so a previous color image cannot remain stuck on screen.
Pushes stored before the color-preserving upload change contain only greyscale
pixels; re-push their original source once after deployment to recover color.

## Photos

`imagePush.quantize_spectra6` treats a source with at most 64 colours as a
flat graphic: Pillow's Floyd-Steinberg against the pure ink values, so exact
ink pixels print as drawn. Anything richer is a photo, converted by
`wgn6.quantize_photo`:

1. saturation ×1.3 and contrast ×1.1 (`PHOTO_SATURATION`, `PHOTO_CONTRAST`);
2. each pixel matched to the nearest of `PHOTO_INKS` in OKLab, where
   distance follows how different two colours look;
3. Burkes error diffusion, rows alternating direction, passing on 80 % of
   the error (`PHOTO_KEEP`), the rest dropped so the dots clump less.
4. a near-grey pixel (OKLab chroma under `PHOTO_NEUTRAL`, 0.04) is kept
   to black and white: each coloured ink is charged `NEUTRAL_PENALTY` (50)
   times its own chroma squared, fading out toward the threshold.

`PHOTO_INKS` is EPD Optimize's measurement of a Spectra 6 panel (greyish
white, dark grey black), not ours yet. `INK_APPEARANCE` stays for previews.

Step 4 came from the `calibrate` test card (`esp32/testcard.py`) on
2026-10-06: the measured green is barely coloured, so plain matching made a
mid-grey 46-63 % green ink, and on the panel the grey read as green. With
step 4 greys from 64 to 160 are black and white only; green, blue and red
areas keep their inks. The same card showed our black as true black (the
measurement says dark grey), our white as a very light grey, green as
forest and blue as deep blue.

Chosen on the PhotoPainter on 2026-10-06 with `tools/dither_compare.py`,
which renders one photo several ways with a score and a `.wgn6` per method
for the USB bench. Against the previous converter (the same boost, Pillow's
Floyd-Steinberg in RGB against `INK_APPEARANCE`), on a family photo:
average lightness 0.575 instead of 0.469 for 0.585 in the source (it was too
dark), blue ink 8 % of the pixels instead of 30 % (a blue cast), and less
than half the isolated dots. Passing on all the error looked about the
same; squeezing the photo's lightness into the palette's narrower range
looked flat on our panel. Plain Python, about 1.5 s at 800x480, once per
publication.

## Activation

The firmware declares it: the PhotoPainter build appends `spectra6` to its
`caps=` on `/schedule` (issue #17). When a declaration changes the profile,
the server renders the frame's slides, push and events again in a background
thread (about 1.5 s per colour publication, longer than the frame waits for
`/schedule`). The frame keeps its image for that cycle and downloads the
colour one on the next. Dropping the token switches back to `mono1` the same
way.

A false declaration gains nothing: `/schedule` is identified only by the
device id, but WGN6 is only served to a request that accepts it, and a mono
panel asks for BMP. At worst the content is rendered again for nothing.

A PhotoPainter flashed before the token keeps working in colour if its
`display_profile` was set to `spectra6` (the admin "Matériel" section, hidden
once the firmware declares the profile). Owners cannot change the profile.
