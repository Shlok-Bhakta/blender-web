# Touch, pen and on-screen keys

Phones and tablets use the touch controls of the
[3D Modeling Thingy iOS port](https://github.com/Shlok-Bhakta/3d-modeling-thingy-ios).
The gesture and modifier logic is the same C++, ported into the browser GHOST
backend. Mouse, trackpad and keyboard input are unchanged.

Your finger moves a cursor, like a trackpad. Drag one finger to aim, then tap
anywhere to click at the cursor. The cursor wraps around the screen edges, and
fast swipes move it farther.

| Gesture | Action |
| --- | --- |
| One-finger drag | Move the cursor |
| One-finger tap | Left click at the cursor |
| Tap, then hold and drag | Left-button drag (sliders, nodes, gizmos, box select) |
| Triple tap | Double click (rename and other double-click controls) |
| Two-finger hold | Right click; keep holding and move for a right-button drag |
| Two-finger drag | Orbit in the 3D Viewport; pan or scroll in 2D editors |
| Pinch | Zoom |
| Three-finger drag | Pan the 3D Viewport |
| Two-, three-, four-finger tap | Undo, Redo, Search (F3) |

Gestures go to the editor under the cursor, so move the cursor there first.

The keyboard button in a corner opens **Shift**, **Ctrl** and **Alt**. Hold a
key for as long as you need it, tap it to apply it to the next click, or double
tap it to lock it until you tap it again. Drag the button to another corner;
it stays there. The keys fade while the cursor is over them and hide once a
hardware keyboard is used.

A pen points directly where it touches, with pressure and tilt, and hovering
moves the cursor where the browser reports hover. The barrel button right
clicks.

Editing a text or number field with touch opens a text sheet at the top of the
page. Tap the field to bring up the keyboard, then tap **Done** to keep the
edit or **Cancel** to restore the old value. Numeric fields accept expressions
such as `3+4`.

## Implementation

- `touch.js` sends raw pointer events through a ring in shared Wasm memory.
  It also draws the cursor, the modifier belt and the text sheet.
- `GHOST_TouchGestures.cc` recognizes gestures.
- `GHOST_WebTouch.cc` turns them into ordinary GHOST cursor, button, key and
  trackpad events.
- Two guarded calls in `interface_handlers.cc` open and close the text sheet
  ([patch](../patches/native-touch.patch)).

`web/native/tests/touch-gestures.cc` checks the gestures natively.
`web/tests/touch.cjs` drives the application with real multi-touch in Chromium:
aim and tap, undo, redo, orbit, pinch, a Shift-latched multi-select and a
rename through the text sheet.

iOS Safari only raises the software keyboard from a tap, so the sheet waits
for you to tap its field.
