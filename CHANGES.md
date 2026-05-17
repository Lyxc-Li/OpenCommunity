# Changes

## Fix: BedPlates — OpenGL Error 1283 (Stack Overflow) / Vanilla GUI blackout

**File:** `runtime/src/features/visuals/BedPlates.cpp`  
**Function:** `BedPlates::BlockIconCallback`

---

### Problem

Two compounding bugs in `BlockIconCallback` caused `GL_STACK_OVERFLOW` (error 1283), which left the OpenGL state corrupted and prevented vanilla Minecraft GUIs from rendering after BedPlates was active.

**Bug 1 — Projection stack overflow**

`BlockIconCallback` is an ImGui draw callback, meaning it fires *inside* ImGui's render pass. ImGui has already pushed one entry onto the GL projection matrix stack before invoking the callback. The old code called `glPushMatrix()` on the projection stack again, consuming the last available slot (the GL spec only guarantees a minimum depth of 2). When Minecraft's `RenderItemIntoGUI` then attempted any further projection push internally, it overflowed immediately, producing error 1283.

**Bug 2 — Matrix mode corruption → modelview stack leak**

Minecraft's Java rendering code (`RenderItemIntoGUI`) calls `GL11.glMatrixMode(...)` internally and does not restore the matrix mode before returning to C++. The old per-item loop looked like:

```cpp
glPushMatrix();                          // pushes MODELVIEW
renderItem->RenderItemIntoGUI(...);      // changes matrix mode to e.g. GL_PROJECTION
glPopMatrix();                           // pops PROJECTION instead of MODELVIEW — wrong!
```

Each icon rendered leaked one unpopped MODELVIEW entry per frame. After enough frames the modelview stack (32 deep) also overflowed, compounding the corruption.

---

### Fix

**1. Replaced projection/modelview push+pop with save+load**

Instead of pushing the projection stack at all, the matrices are now saved with `glGetFloatv` and restored with `glLoadMatrixf`. This never changes the stack depth, so there is no overflow regardless of how shallow the driver's projection stack is.

```cpp
// Before (dangerous)
glMatrixMode(GL_PROJECTION);
glPushMatrix();
glLoadIdentity();
glOrtho(...);
// ...
glMatrixMode(GL_PROJECTION);
glPopMatrix();

// After (safe)
GLfloat savedProj[16];
glGetFloatv(GL_PROJECTION_MATRIX, savedProj);
glMatrixMode(GL_PROJECTION);
glLoadIdentity();
glOrtho(...);
// ...
glMatrixMode(GL_PROJECTION);
glLoadMatrixf(savedProj);
```

**2. Per-item matrix stack depth tracking**

The projection and modelview stack depths are now snapshotted before each `RenderItemIntoGUI` call and compared afterwards. Any entries left on either stack by Minecraft's Java renderer are unconditionally drained before the next iteration. This handles both the matrix mode corruption case and any other imbalance Minecraft may introduce.

```cpp
GLint projBefore, mvBefore;
glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &projBefore);
glGetIntegerv(GL_MODELVIEW_STACK_DEPTH,  &mvBefore);

glMatrixMode(GL_MODELVIEW);
glPushMatrix();
glTranslatef(...); glScalef(...);
renderItem->RenderItemIntoGUI(stack, 0, 0, env);

GLint projAfter, mvAfter;
glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &projAfter);
glGetIntegerv(GL_MODELVIEW_STACK_DEPTH,  &mvAfter);

if (projAfter > projBefore) {
    glMatrixMode(GL_PROJECTION);
    for (GLint i = projAfter; i > projBefore; --i) glPopMatrix();
}
glMatrixMode(GL_MODELVIEW);
for (GLint i = mvAfter; i > mvBefore; --i) glPopMatrix();
```

---

### Result

- `GL_STACK_OVERFLOW` (1283) no longer occurs during BedPlates rendering.
- Vanilla Minecraft GUIs render correctly after BedPlates plates are drawn.
- Block icons in BedPlates plates are unaffected and render as before.
- Build: **0 errors, 0 new warnings** (`runtime.dll`, `launcher.exe`).
