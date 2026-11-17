#pragma once
/**
 * Paced scene player (scene_paced1): scenes on e-paper (docs/scenes_paced.md
 * on the platform, issue #229). The server draws every page in the board's
 * own format (a 1-bit BMP or WGN6, what /pic serves); the board keeps them in
 * flash, runs the scene runtime 2 logic (scene_runtime.cpp: variables, guards,
 * set, sound, report, end, page) and shows a page's file. Nothing moves.
 *
 * The rules this player keeps:
 * - Rule 1, the screen lock: after every redraw (a page, /pic, anything the
 *   loop draws) the screen stays as it is until the refresh is over plus
 *   HOLD_MS. Sounds, sets and reports still run at once; pages asked for
 *   meanwhile collapse into the last one, drawn when the lock ends.
 * - Rule 2: after:, idle: and every: count from the end of the refresh. The
 *   runtime is the platform's reference, unchanged: the player gives it a
 *   clock that stands still while the page it asked for is not yet on screen.
 * - Buttons are read in their own task, so a press during a refresh (the loop
 *   is blocked in it for seconds) is never lost.
 *
 * The board's own gestures come first: a ringing alarm takes any press, and a
 * 5 s hold on any button leaves the scene. A paced scene plays only while the
 * board is awake (`Hooks::awake`); otherwise it shows its start page, which
 * is what /pic serves, inert (docs/scenes_paced.md "Decisions").
 */
#include <Arduino.h>

#if defined(WIGNET_SCENES_PACED)
#define WIGNET_PACED_PLAYER 1

namespace PacedPlayer {

struct Hooks {
  // Draws a page file the way /pic is drawn (BMP or WGN6, checked before the
  // panel is touched); blocks for the refresh. False: nothing drawn.
  bool (*draw)(Stream &file, int length) = nullptr;
  // True while the board stays awake (mains, a computer on USB, or a board
  // that never sleeps): a scene plays only then.
  bool (*awake)() = nullptr;
};

void begin(const Hooks &hooks);

// What /schedule adds: "&in=a[,b]&sev=<last scene event seen>".
String scheduleParams();

// The /schedule answer: the scene to play (or none), its events, the clock.
void onSchedule(const String &payload);

// From loop(): loads a new bundle, draws the page wanted once the lock is
// over, ends a finished scene, posts reports.
void service();

// The loop drew something itself (/pic, a bench image): it locks the screen
// as a page would, and the page shown is no longer one of the scene's.
void onRedraw(uint32_t startedMs);

// True while the screen is locked (rule 1): the loop waits to draw /pic.
bool screenLocked();

// The loop must not deep-sleep: a paced scene is named and the board is
// awake, one still plays, or its start page is still to put back.
bool keepsAwake();

bool playing();

} // namespace PacedPlayer

#endif
