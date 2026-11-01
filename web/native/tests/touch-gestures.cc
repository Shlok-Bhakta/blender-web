/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native unit test for the platform-neutral touch gestures used by the browser GHOST. */
#include "../ghost/GHOST_TouchGestures.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(condition) \
  do { \
    if (!(condition)) { \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
      failures++; \
    } \
  } while (0)

struct Recorder : GHOST_TouchSink {
  std::vector<std::string> log;
  GHOST_TouchPointerState *pointer = nullptr;
  double scroll_x = 0, scroll_y = 0, zoom = 0;
  GHOST_TouchTablet last_tablet;
  void pointerChanged(const GHOST_TouchTablet &tablet) override
  {
    last_tablet = tablet;
    if (log.empty() || log.back() != "move") {
      log.push_back("move");
    }
  }
  void button(GHOST_TouchPointerButton button, bool down, const GHOST_TouchTablet &) override
  {
    const char *name = button == GHOST_TouchPointerButton::Left ? "L" :
                       button == GHOST_TouchPointerButton::Right ? "R" : "M";
    log.push_back(std::string(name) + (down ? "+" : "-"));
  }
  void modifier(GHOST_TouchModifier modifier, bool down) override
  {
    const char *name = modifier == GHOST_TouchModifier::Shift ? "Shift" :
                       modifier == GHOST_TouchModifier::Control ? "Ctrl" : "Alt";
    log.push_back(std::string(name) + (down ? "+" : "-"));
  }
  void scroll(double dx, double dy, int fingers) override
  {
    scroll_x += dx;
    scroll_y += dy;
    const std::string entry = "scroll" + std::to_string(fingers);
    if (log.empty() || log.back() != entry) {
      log.push_back(entry);
    }
  }
  void magnify(double delta) override
  {
    zoom += delta;
    if (log.empty() || log.back() != "zoom") {
      log.push_back("zoom");
    }
  }
  void shortcut(GHOST_TouchShortcut shortcut) override
  {
    log.push_back(shortcut == GHOST_TouchShortcut::Undo ? "undo" :
                  shortcut == GHOST_TouchShortcut::Redo ? "redo" : "search");
  }
};

struct Fixture {
  GHOST_TouchPointerState pointer;
  GHOST_TouchModifierLatch latch;
  Recorder sink;
  GHOST_TouchGestures gestures{pointer, latch, sink};
  Fixture()
  {
    pointer.initialize(1000, 800, 1.0);
  }
  std::string events() const
  {
    std::string out;
    for (const std::string &entry : sink.log) {
      out += (out.empty() ? "" : " ") + entry;
    }
    return out;
  }
};

static void expect(const Fixture &f, const char *expected, const char *name)
{
  if (f.events() != expected) {
    std::fprintf(stderr, "%s: expected \"%s\", got \"%s\"\n", name, expected, f.events().c_str());
    failures++;
  }
}

int main()
{
  {
    /* A tap clicks at the cursor once the repeat window passes, not on lift. */
    Fixture f;
    f.gestures.touchDown(1, 100, 100, 0.0);
    f.gestures.touchUp(1, 101, 100, 0.08);
    f.gestures.update(0.2);
    expect(f, "", "tap waits");
    f.gestures.update(0.5);
    expect(f, "L+ L-", "tap");
    CHECK(std::abs(f.pointer.x() - 500) < 1e-9 && std::abs(f.pointer.y() - 400) < 1e-9);
  }
  {
    /* A slow drag moves the cursor one-to-one; it never clicks. */
    Fixture f;
    f.gestures.touchDown(1, 100, 100, 0.0);
    for (int i = 1; i <= 10; i++) {
      f.gestures.touchMove(1, 100 + i * 3, 100, i * 0.1);
    }
    f.gestures.touchUp(1, 130, 100, 1.1);
    f.gestures.update(2.0);
    expect(f, "move", "slow drag");
    CHECK(std::abs(f.pointer.x() - 530) < 1e-6);
  }
  {
    /* A fast swipe is accelerated. */
    Fixture f;
    f.gestures.touchDown(1, 100, 100, 0.0);
    f.gestures.touchMove(1, 120, 100, 0.01);
    f.gestures.touchMove(1, 140, 100, 0.02);
    f.gestures.touchUp(1, 140, 100, 0.03);
    CHECK(f.pointer.x() > 540);
  }
  {
    /* Tap, then hold and drag: one left-button drag, no stray click. */
    Fixture f;
    f.gestures.touchDown(1, 100, 100, 0.0);
    f.gestures.touchUp(1, 100, 100, 0.05);
    f.gestures.touchDown(1, 100, 100, 0.15);
    f.gestures.update(0.2);
    expect(f, "", "drag hold not yet");
    f.gestures.update(0.28);
    f.gestures.touchMove(1, 150, 100, 0.6);
    f.gestures.touchUp(1, 150, 100, 0.7);
    f.gestures.update(2.0);
    expect(f, "L+ move L-", "tap hold drag");
  }
  {
    /* Triple tap is a double click; two quick taps are one click. */
    Fixture f;
    double t = 0.0;
    for (int i = 0; i < 3; i++) {
      f.gestures.touchDown(1, 100, 100, t);
      f.gestures.touchUp(1, 100, 100, t + 0.05);
      f.gestures.update(t + 0.06);
      t += 0.15;
    }
    f.gestures.update(2.0);
    expect(f, "L+ L- L+ L-", "triple tap");

    Fixture g;
    g.gestures.touchDown(1, 100, 100, 0.0);
    g.gestures.touchUp(1, 100, 100, 0.05);
    g.gestures.touchDown(1, 100, 100, 0.15);
    g.gestures.touchUp(1, 100, 100, 0.2);
    g.gestures.update(1.0);
    expect(g, "L+ L-", "double tap");
  }
  {
    /* A still two-finger hold is a right click, released with the fingers. */
    Fixture f;
    f.gestures.touchDown(1, 100, 100, 0.0);
    f.gestures.touchDown(2, 160, 100, 0.01);
    f.gestures.update(0.2);
    expect(f, "", "right hold not yet");
    f.gestures.update(0.35);
    f.gestures.touchUp(1, 100, 100, 0.5);
    f.gestures.touchUp(2, 160, 100, 0.52);
    f.gestures.update(2.0);
    expect(f, "R+ R-", "two finger hold");
  }
  {
    /* Quick multi-finger taps are Undo, Redo and Search. */
    const char *expected[] = {"undo", "redo", "search"};
    for (int fingers = 2; fingers <= 4; fingers++) {
      Fixture f;
      for (int i = 0; i < fingers; i++) {
        f.gestures.touchDown(i, 100 + i * 50, 100, i * 0.01);
      }
      for (int i = 0; i < fingers; i++) {
        f.gestures.touchUp(i, 100 + i * 50, 100, 0.1 + i * 0.01);
      }
      f.gestures.update(2.0);
      expect(f, expected[fingers - 2], "multi tap");
    }
  }
  {
    /* Two fingers orbit/scroll and pinch at once; the cursor stays put. */
    Fixture f;
    f.gestures.touchDown(1, 100, 100, 0.0);
    f.gestures.touchDown(2, 200, 100, 0.01);
    for (int i = 1; i <= 5; i++) {
      f.gestures.touchMove(1, 100 + i * 10 - i * 4, 100 + i * 10, 0.02 * i);
      f.gestures.touchMove(2, 200 + i * 10 + i * 4, 100 + i * 10, 0.02 * i + 0.005);
    }
    f.gestures.touchUp(1, 0, 0, 0.3);
    f.gestures.touchUp(2, 0, 0, 0.31);
    f.gestures.update(2.0);
    CHECK(f.events().find("scroll2") != std::string::npos);
    CHECK(f.events().find("zoom") != std::string::npos);
    CHECK(f.events().find("R+") == std::string::npos);
    CHECK(f.events().find("undo") == std::string::npos);
    CHECK(f.sink.scroll_y > 30 && f.sink.zoom > 30);
    CHECK(std::abs(f.pointer.x() - 500) < 1e-9);
  }
  {
    /* Three fingers pan with Shift held for the gesture only. */
    Fixture f;
    for (int i = 0; i < 3; i++) {
      f.gestures.touchDown(i, 100 + i * 40, 100, 0.0);
    }
    for (int step = 1; step <= 4; step++) {
      for (int i = 0; i < 3; i++) {
        f.gestures.touchMove(i, 100 + i * 40 + step * 8, 100, 0.02 * step);
      }
    }
    for (int i = 0; i < 3; i++) {
      f.gestures.touchUp(i, 0, 0, 0.3);
    }
    expect(f, "Shift+ scroll3 Shift-", "three finger pan");
  }
  {
    /* A tapped belt Shift applies to the next click only. */
    Fixture f;
    f.gestures.modifierDown(GHOST_TouchModifier::Shift, 0.0);
    f.gestures.modifierUp(GHOST_TouchModifier::Shift, 0.1);
    f.gestures.touchDown(1, 100, 100, 1.0);
    f.gestures.touchUp(1, 100, 100, 1.05);
    f.gestures.update(2.0);
    f.gestures.touchDown(1, 100, 100, 3.0);
    f.gestures.touchUp(1, 100, 100, 3.05);
    f.gestures.update(4.0);
    /* The finger on the key presses Shift at once, as the iOS latch does. */
    expect(f, "Shift+ Shift- Shift+ L+ L- Shift- L+ L-", "latched shift");
  }
  {
    /* A held belt key is down immediately and survives the click. */
    Fixture f;
    f.gestures.modifierDown(GHOST_TouchModifier::Control, 0.0);
    f.gestures.touchDown(1, 100, 100, 0.5);
    f.gestures.touchUp(1, 100, 100, 0.55);
    f.gestures.update(1.0);
    f.gestures.modifierUp(GHOST_TouchModifier::Control, 1.5);
    expect(f, "Ctrl+ L+ L- Ctrl-", "held ctrl");
  }
  {
    /* The pen points directly, with pressure, and its barrel button right clicks. */
    Fixture f;
    GHOST_TouchTablet tablet;
    tablet.active = true;
    tablet.pressure = 0.4f;
    f.gestures.penHover(300, 300, 0.0);
    f.gestures.penDown(310, 300, 0.1, tablet, false);
    f.gestures.penMove(350, 320, 0.2, tablet);
    f.gestures.penUp(360, 320, 0.3, tablet);
    expect(f, "move L+ move L-", "pen stroke");
    CHECK(std::abs(f.pointer.x() - 360) < 1e-9 && f.sink.last_tablet.pressure == 0.4f);
    CHECK(!f.pointer.visible());

    Fixture g;
    g.gestures.penDown(10, 10, 0.0, tablet, true);
    g.gestures.penUp(10, 10, 0.1, tablet);
    expect(g, "move R+ move R-", "pen barrel");
  }
  {
    /* Losing focus releases a held drag and the belt keys. */
    Fixture f;
    f.gestures.modifierDown(GHOST_TouchModifier::Alt, 0.0);
    f.gestures.touchDown(1, 100, 100, 0.0);
    f.gestures.touchUp(1, 100, 100, 0.05);
    f.gestures.touchDown(1, 100, 100, 0.1);
    f.gestures.update(0.3);
    f.gestures.reset();
    expect(f, "Alt+ L+ L- Alt-", "reset");
    CHECK(!f.gestures.active());
  }
  if (failures) {
    std::fprintf(stderr, "%d touch gesture checks failed\n", failures);
    return 1;
  }
  std::printf("touch gestures: all checks passed\n");
  return 0;
}
