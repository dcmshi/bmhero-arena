# RenderDoc capture & scripting reference (BMHeroRecomp)

Practical notes for capturing and inspecting frames from `BMHeroRecompiled`
(RT64 / D3D12). Written after RenderDoc settled the A1.5 camera investigation in
one capture — see integration notes **§8.17** for that root-cause analysis.

**Use this whenever a screenshot disagrees with a calculation.** A RenderDoc
capture is the project's visual ground truth; `tools/capture-game.ps1` is the
cheap everyday tool but it is a *derived* view and it has lied before.

---

## The working recipe

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
.\tools\rd-capture.ps1                        # capture a frame in-arena
.\tools\rd-capture.ps1 -Save                  # ...and save the frame to PNG
.\tools\rd-capture.ps1 -Dist 2400 -Mode 6 -Save
```

It launches the game hooked, waits for `[capture] level` in `arena_bridge.log`
(i.e. the arena is actually live), triggers the capture over **target control**,
and optionally replays the capture to write the backbuffer PNG.

Scripts, all in the fork's `tools/`:

| file | role |
|---|---|
| `rd-capture.ps1` | driver — launch, wait, trigger, save |
| `rd_trigger.py` | connects over target control and triggers the capture |
| `rd_saveframe.py` | replays a `.rdc` and saves the backbuffer to PNG |
| `rd_analyse.py` | dumps draws + post-VS clip bounds (see "reading geometry") |

Captures are ~31 MB each and land in `tools/rdcaps/` (gitignored).

---

## Quirks — the things that cost time

### 1. There is no `renderdoc.pyd` in the install

So you **cannot** `import renderdoc` from a system Python. All scripting goes
through qrenderdoc's **embedded** interpreter:

```
qrenderdoc --python <script.py>
```

That interpreter is **Python 3.6.4** (verified). No dataclasses, no walrus, no
3.7+ stdlib. It needs no system Python and no packages — convenient, but old.

### 2. `renderdoccmd` has no "trigger capture" verb

Subcommands are: `capture`, `convert`, `embed`, `extract`, `help`, `inject`,
`remoteserver`, `replay`, `test`, `thumb`, `version`. `capture` launches the
target hooked but gives you no way to *say when* to capture.

### 3. F12 does not work from an automated run

The hook's capture key requires the game window to be **foreground**, and
`SetForegroundWindow` is restricted for a process that isn't already foreground.
Calling it from a background script silently does nothing: no error, no capture,
no log. This wasted a full cycle.

**Use target control instead** — no keyboard, no focus:

```python
ident = 0
while True:
    ident = rd.EnumerateRemoteTargets("localhost", ident)
    if ident == 0: break
    targets.append(ident)

conn = rd.CreateTargetControl("localhost", targets[0], "claude", True)
conn.TriggerCapture(1)
while ...:
    msg = conn.ReceiveMessage(None)
    if msg and msg.type == rd.TargetControlMessageType.NewCapture:
        path = msg.newCapture.path          # the .rdc that was written
```

### 4. Pass paths through the ENVIRONMENT, not argv

`qrenderdoc`'s usage is `qrenderdoc [options] filename`, so a positional argument
after `--python x.py` is not script argv.

**Observed:** adding extra positional arguments made the run produce *no output
at all* — the script did not execute. Our scripts therefore read
`ARENA_RD_LOG`, `ARENA_RD_CAP`, `ARENA_RD_PNG` from the environment, which works
reliably.

> **Caveat, stated honestly:** in a later isolated test, a probe script under a
> scratchpad directory failed to run *even without* extra arguments, while the
> same invocation form against a script in the repo's `tools/` runs every time.
> So "extra argv breaks it" is a real observation but not a fully pinned-down
> mechanism, and something about script location may also matter. **Keep scripts
> in `tools/` and drive them through `rd-capture.ps1`** — that path is verified.

### 5. The script cannot locate itself

`__file__` is **undefined** in the embedded interpreter, and `sys.argv[0]` is
`qrenderdoc.exe` under `C:\Program Files\RenderDoc` — **not writable**. A script
that derives its log path from either dies at import with **no output at all**,
which is indistinguishable from "the script never ran". Always take the log path
from the environment and open it as the very first thing.

### 6. qrenderdoc is a GUI app

stdout is not reliably visible. **Log to a file.** And `--python` runs the script
*before* the main UI opens; the process will not exit on its own, so end the
script with `os._exit(rc)` and have the driver kill it as a backstop.

### 7. API result shapes vary by version

`OpenFile` returns a `ResultDetails` (with `.OK()`) in current builds and a
`ReplayStatus` enum in older ones; `OpenCapture` returns a `(result, controller)`
tuple. Handle both:

```python
r = cap.OpenFile(path, '', None)
ok = r.OK() if hasattr(r, 'OK') else (r == rd.ReplayStatus.Succeeded)
out = cap.OpenCapture(rd.ReplayOptions(), None)
r, controller = out if isinstance(out, tuple) else (None, out)
```

### 8. Saving the backbuffer

Seek to the **end of the frame** first or you get a partially-rendered image:

```python
acts = list(iter_actions(controller.GetRootActions()))
controller.SetFrameEvent(acts[-1].eventId, True)
tex = next(t for t in controller.GetTextures()
           if t.creationFlags & rd.TextureCategory.SwapBuffer)
sav = rd.TextureSave(); sav.resourceId = tex.resourceId
sav.mip = 0; sav.slice.sliceIndex = 0; sav.destType = rd.FileType.PNG
controller.SaveTexture(sav, out_png)
```

The backbuffer is **1600×900** — worth remembering, since assuming the window's
logical 800×450 is exactly what broke `capture-game.ps1`.

---

## Reading geometry out of a capture

The trick that settled A1.5: **clip-space `w` is view-space depth**, so the range
of `w` across the level's draws tells you where geometry sits relative to the
camera, in world units, checkable against a camera model — no image
interpretation at all.

```python
controller.SetFrameEvent(draw.eventId, True)
mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
data = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset,
                                n * mesh.vertexByteStride)
x, y, z, w = struct.unpack_from("<ffff", data, v * mesh.vertexByteStride)
```

Worked example — our camera at pitch 60 / `ARENA_CAM_DIST` 2400 puts the eye at
`at + dist*(0, sin60, cos60)`, so a floor point at world `z` has

```
depth = 2486.6 - 0.5*z
```

For the measured collision floor `z ∈ [-950, 950]` that predicts
`w ∈ [2011.6, 2961.6]`. The capture reported **`w ∈ [2006.6, 2966.6]`**, and
clip-x extremes of ±1537/1553 corresponded to world `x = ±950`. Conclusion: the
drawn floor *is* the collision floor and the camera *was* where we set it —
established before anyone looked at the picture.

Note the frame has ~1500 drawcalls and the level floor is drawn as **many small
chunks** (largest draw is 324 indices), so aggregate over draws rather than
looking for one big mesh.
