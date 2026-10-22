#!/usr/bin/env python3
"""Show an image on a frame over USB, exactly as push_content would.

    python3 esp32/show_image.py poster.png
    python3 esp32/show_image.py photo.jpg --fit cover --mode dither
    python3 esp32/show_image.py logo.png --profile mono1 --mode threshold
    python3 esp32/show_image.py frame.wgn6          # a device-ready file, as is
    python3 esp32/show_image.py poster.png --preview-only

A picture is rendered by imagePush.render, the function behind
push_content: fit, orientation, dither or threshold, then the six-colour
WGN6 file (--profile spectra6) or the 1-bit BMP (mono1). Without --profile
the frame chooses: its "help" names the formats it draws, so a PhotoPainter
gets WGN6 and a black-and-white frame (the Waveshare driver board, over its
USB-serial bridge) the BMP; --preview-only assumes spectra6. The
exact panel preview is saved beside the source as <name>.panel.png, so the
panel can be compared with what the converter meant; for six colours,
<name>.real.png paints each ink as it actually looks (wgn6.INK_APPEARANCE).
A .bmp or .wgn6 file is sent unchanged.

The file then goes over USB to the frame's bench console
(src/usb_console.cpp) in acknowledged 4 KB chunks, and is drawn by the
decoder that draws downloads, with the same checks: a bad file leaves the
panel alone. The frame reports how long the transfer and the panel took.
No platform, Wi-Fi or deploy is involved. Needs pyserial and Pillow.
"""
import argparse
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, ROOT)
import play_mml  # noqa: E402  (Console, port handling)

CHUNK = 4096
WIDTH, HEIGHT = 800, 480


def render_file(path, profile, fit, mode, orientation, width=WIDTH, height=HEIGHT):
    """(bytes to send, panel preview image or None) for a source file."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] == b"BM" or data[:4] == b"WGN6":
        return data, None
    from PIL import Image
    import imagePush
    image = Image.open(io.BytesIO(data))
    image.load()
    bw_image, quantized = imagePush.render(image, width, height, fit, mode, orientation, profile)
    if profile == "spectra6":
        return imagePush.wgn6_bytes(quantized), quantized
    out = io.BytesIO()
    bw_image.save(out, format="BMP")
    return out.getvalue(), bw_image


def frame_profile(help_reply):
    """The profile a frame draws, from the first line of its `help`.

    "commands (images: bmp):" is a black-and-white frame; a colour one lists
    wgn6, and firmware from before the list ("commands:") was PhotoPainter
    only.
    """
    if "images:" in help_reply and "wgn6" not in help_reply:
        return "mono1"
    return "spectra6"


def connect(port_path):
    """(port, console, its help reply), once the frame's console answers."""
    port = play_mml.open_serial(port_path or play_mml.find_port())
    console = play_mml.Console(port)
    # Wait out a busy frame (redraw, fetch) before streaming binary data.
    for attempt in range(40):
        console.send("help")
        try:
            return port, console, console.wait("commands", timeout=3)
        except TimeoutError:
            if attempt == 0:
                print("Waiting for the frame (it may be redrawing, fetching or asleep)...")
    port.close()
    raise TimeoutError("the frame's bench console never answered")


def send_image(console, data):
    """Upload one file through the console's `image` command; its report."""
    console.send(f"image {len(data)}")
    console.wait("send", timeout=90)
    for start in range(0, len(data), CHUNK):
        console.port.write(data[start:start + CHUNK])
        if start + CHUNK < len(data):
            console.wait("+", timeout=30)
    # The panel refresh follows the transfer; a Spectra 6 one takes a while.
    return console.wait("shown", timeout=180)


def main(argv=None):
    import imagePush  # noqa: F401  (fail early if the repo's render code is missing)
    from backoffice.uploads import DEFAULT_FIT, DEFAULT_MODE, FIT_CHOICES, MODE_CHOICES
    from orientation import DEFAULT_ORIENTATION, ORIENTATIONS

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file", help="an image (PNG, JPEG…), or a .bmp / .wgn6 to send as is")
    parser.add_argument("--profile", choices=("spectra6", "mono1"),
                        help="six-colour WGN6 or the 1-bit BMP (default: what the frame draws)")
    parser.add_argument("--fit", choices=FIT_CHOICES, default=DEFAULT_FIT)
    parser.add_argument("--mode", choices=MODE_CHOICES, default=DEFAULT_MODE,
                        help="dither for photos, threshold for text, logos, QR codes (1-bit only)")
    parser.add_argument("--orientation", default=DEFAULT_ORIENTATION, choices=list(ORIENTATIONS),
                        help=f"as push_content (default {DEFAULT_ORIENTATION})")
    parser.add_argument("--preview-only", action="store_true",
                        help="write <name>.panel.png, send nothing")
    parser.add_argument("--port")
    args = parser.parse_args(argv)

    port = None
    try:
        console = None
        profile = args.profile
        if profile is None and not args.preview_only:
            port, console, reply = connect(args.port)
            profile = frame_profile(reply)
        data = prepare(args, profile or "spectra6")
        if args.preview_only:
            return 0
        if console is None:
            port, console, _ = connect(args.port)
        print(play_mml.PREFIX + send_image(console, data))
        return 0
    except play_mml.ConsoleError as exc:
        print(f"The frame refused: {exc}")
        return 1
    except TimeoutError as exc:
        print(exc)
        return 1
    finally:
        if port is not None:
            port.close()


def prepare(args, profile):
    """The bytes to send; writes and names the panel preview of a picture."""
    data, preview = render_file(args.file, profile, args.fit, args.mode, args.orientation)
    kind = "WGN6" if data[:4] == b"WGN6" else "BMP"
    if preview is not None:
        base = os.path.splitext(args.file)[0]
        preview.convert("RGB").save(base + ".panel.png")
        message = f"{kind}, {len(data)} bytes; preview: {base}.panel.png"
        if preview.mode == "P":
            # The same pixels, each ink painted as it looks on the panel.
            from backoffice.wgn6 import appearance_preview
            appearance_preview(preview).save(base + ".real.png")
            message += f" (as the panel shows it: {base}.real.png)"
        print(message)
    else:
        print(f"{kind}, {len(data)} bytes, sent as is")
    return data

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
