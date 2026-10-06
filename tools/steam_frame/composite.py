#!/usr/bin/env python3
"""Rebuild what SteamVR composites for one eye from an unattended run's dumps (needs numpy and
Pillow). The game logs every layer's geometry (`[vr] layers: ...`, xr_host.cpp logLayers) with
each eye dump and every 2 s while panel dumps run; this draws, for one eye and in submission
order, the projection layer (an eye dump), the panel quad (a panel dump; "panelpm" in the log is
the VR menu's panel, blended with premultiplied alpha), the HUD quad (a HUD
dump, unpremultiplied alpha), view-space quads (the fade) and the pointer's beam and dot (the static UI image, rebuilt here
exactly as createUiImage makes it). It is a reconstruction for checking layer placement, order
and alpha, not a capture of the compositor.

  composite.py RUN_DIR --layers N --eye 0|1 [--eye-image F] [--panel F] [--hud F] [--out F]

N picks the N-th logged layers line (negative counts from the end). Images default to none
(the layer is then drawn as a magenta outline only, so its placement shows without tinting).
"""
import argparse
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image


def qrot(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    v = np.asarray(v, dtype=np.float64)
    t = 2.0 * np.cross(u, v)
    return v + w * t + np.cross(u, t)


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qrot_many(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    t = 2.0 * np.cross(u, v)
    return v + w * t + np.cross(u, t)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def parse_layers(log, index):
    lines = [l for l in Path(log).read_text(errors="replace").splitlines() if l.startswith("[vr] layers:")]
    if not lines:
        raise SystemExit("no layers lines in the log")
    line = lines[index]
    out = []
    for part in line.split("|")[1:]:
        f = part.split()
        name = f[0]
        nums = [float(x) for x in re.findall(r"-?\d+\.?\d*(?:e-?\d+)?", part[len(name) + 1:])]
        if name.startswith("proj"):
            p, q, fov, rect = nums[0:3], nums[3:7], nums[7:11], nums[11:15]
            out.append(dict(kind="proj", eye=int(name[4]), p=p, q=q, fov=fov, rect=rect))
        else:
            p, q, size, rect = nums[0:3], nums[3:7], nums[7:9], nums[9:13]
            out.append(dict(kind="quad", name=name, p=p, q=q, size=size, rect=rect))
    return line, out


def ui_image():
    img = np.zeros((64, 128, 4), dtype=np.float64)
    for y in range(64):
        for x in range(64):
            dx, dy = x + 0.5 - 32, y + 0.5 - 32
            r = math.hypot(dx, dy)
            if r < 22:
                img[y, x] = [235, 250, 255, 255 * min(1.0, 22 - r)]
            else:
                img[y, x] = [20, 30, 40, 255 * max(0.0, min(1.0, 31 - r)) * 0.85]
        for x in range(64, 96):
            u = (x - 64 + 0.5) / 32
            a = max(0.0, 1 - abs(u - 0.5) * 2)
            img[y, x] = [200, 235, 255, 255 * a * a * 0.9]
        img[y, 96:128] = [0, 0, 0, 255 * (y // 4) / 15]  # the fade and blink bands (FadeLevels 16)
    return img / 255.0


def load(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.float64) / 255.0


def sample(img, rect, u, v):
    """img HxWx4, rect = x y w h in pixels, u/v in 0..1 (v down)."""
    x0, y0, w, h = rect
    xs = np.clip((x0 + u * w).astype(int), 0, img.shape[1] - 1)
    ys = np.clip((y0 + v * h).astype(int), 0, img.shape[0] - 1)
    return img[ys, xs]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--layers", type=int, default=-1)
    ap.add_argument("--eye", type=int, default=0)
    ap.add_argument("--eye-image")
    ap.add_argument("--panel")
    ap.add_argument("--hud")
    ap.add_argument("--out", default="composite.png")
    ap.add_argument("--size", type=int, default=900)
    ap.add_argument("--tan", type=float, default=1.1, help="half-extent of the output view, as a tangent")
    ap.add_argument("--order", choices=["depth", "submit"], default="depth",
                    help="quad compositing: by distance per pixel, as SteamVR does (default), or in submission order")
    a = ap.parse_args()

    line, layers = parse_layers(Path(a.run) / "run-stdout.txt", a.layers)
    proj = [l for l in layers if l["kind"] == "proj" and l["eye"] == a.eye][0]
    eye_pos = np.array(proj["p"])
    view_q = tuple(proj["q"])

    n = a.size
    t = np.linspace(-a.tan, a.tan, n)
    tx, ty = np.meshgrid(t, -t)
    dirs_view = np.stack([tx, ty, -np.ones_like(tx)], axis=-1)
    dirs = qrot_many(view_q, dirs_view)
    out = np.zeros((n, n, 3))

    # The projection layer: the eye image through the view's own FOV.
    eye_img = load(a.eye_image) if a.eye_image else None
    l, r, u, d = proj["fov"]
    loc = qrot_many(qconj(view_q), dirs)
    fx, fy = loc[..., 0] / -loc[..., 2], loc[..., 1] / -loc[..., 2]
    tl, tr, tu, td = math.tan(l), math.tan(r), math.tan(u), math.tan(d)
    inside = (loc[..., 2] < 0) & (fx >= tl) & (fx <= tr) & (fy <= tu) & (fy >= td)
    uu, vv = (fx - tl) / (tr - tl), (tu - fy) / (tu - td)
    if eye_img is not None:
        out[inside] = sample(eye_img, proj["rect"], uu[inside], vv[inside])[:, :3]
    else:
        out[inside] = [0.2, 0.25, 0.3]

    images = {"panel": load(a.panel) if a.panel else None, "hud": load(a.hud) if a.hud else None, "ui": ui_image()}
    # The view space (quads marked @view) is the head: between the two eyes, with the eye's
    # orientation.
    projs = [l for l in layers if l["kind"] == "proj"]
    head_p = np.mean([np.array(l["p"]) for l in projs], axis=0)
    drawn = []  # (distance per pixel, mask, rgb, alpha, premultiplied)
    for q in [l for l in layers if l["kind"] == "quad"]:
        name = q["name"].split("@")[0]
        qq = tuple(q["q"])
        centre = np.array(q["p"])
        if q["name"].endswith("@view"):
            qq = qmul(view_q, qq)
            centre = head_p + qrot(view_q, centre)
        normal = qrot(qq, [0, 0, 1])
        denom = dirs @ normal
        dist = ((centre - eye_pos) @ normal) / np.where(np.abs(denom) < 1e-9, 1e-9, denom)
        hit = eye_pos + dirs * dist[..., None]
        rel = hit - centre
        x = rel @ qrot(qq, [1, 0, 0])
        y = rel @ qrot(qq, [0, 1, 0])
        w, h = q["size"]
        on = (dist > 0) & (np.abs(x) <= w / 2) & (np.abs(y) <= h / 2)
        if not on.any():
            continue
        img = images.get("panel" if name == "panelpm" else name)
        rgb = np.zeros((n, n, 3))
        alpha = np.zeros((n, n, 1))
        if img is None:
            # No image for this layer: draw only its outline (1 px), so the placement shows
            # without tinting what lies under it.
            print(f"note: no image for the {name} layer; drawn as an outline only")
            px_w = 2 * a.tan / n
            on = on & ((np.abs(np.abs(x) - w / 2) < px_w * dist) | (np.abs(np.abs(y) - h / 2) < px_w * dist))
            rgb[on] = [1.0, 0.0, 1.0]
            alpha[on] = 1.0
            drawn.append((dist, on, rgb, alpha, False))
            continue
        px = sample(img, q["rect"], x[on] / w + 0.5, 0.5 - y[on] / h)
        rgb[on] = px[:, :3]
        alpha[on] = px[:, 3:4] if name != "panel" else 1.0
        # "panelpm": the panel blended with premultiplied alpha (the VR settings menu, which
        # floats over the world); "panel" is opaque.
        drawn.append((dist, on, rgb, alpha, name == "panelpm"))

    # SteamVR depth-composites quad layers (lesson 83): per pixel, the farthest first. With
    # --order submit they go in submission order instead.
    if drawn:
        dists = np.stack([np.where(d[1], d[0], -np.inf) for d in drawn])  # (L, n, n)
        order = np.argsort(-dists, axis=0) if a.order == "depth" else np.broadcast_to(np.arange(len(drawn))[:, None, None], dists.shape)
        for k in range(len(drawn)):
            idx = order[k]
            for li, (dist, on, rgb, alpha, pm) in enumerate(drawn):
                m = on & (idx == li)
                if not m.any():
                    continue
                if pm:
                    out[m] = rgb[m] + out[m] * (1 - alpha[m])
                else:
                    out[m] = rgb[m] * alpha[m] + out[m] * (1 - alpha[m])

    Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8)).save(a.out)
    print(f"{a.out}: eye {a.eye}, {len(layers)} layer entries from: {line[:120]}...")


if __name__ == "__main__":
    main()
