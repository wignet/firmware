#pragma once
/**
 * Scenes (docs/scenes.md, issue #229): a bundle the platform compiles, played
 * here with LVGL between two contacts. /schedule names the scene to play
 * (`scene`) and the events sent to it (`sceneEvents`); the manifest and its
 * raw assets come from /scene/<id>; `report` actions go back to /input.
 *
 * The board's own gestures come first, whatever the scene says: a long press
 * on PWR switches it off (power_hold_hal.cpp), a ringing alarm takes any
 * press, and a 5 s hold on any button leaves the scene.
 */
#include <Arduino.h>

#if defined(WIGNET_SCENES) && !defined(WIGNET_LVGL_SPIKE)
#define WIGNET_SCENE_PLAYER 1

namespace ScenePlayer {

// LVGL, its draw buffers and task, the buttons. The panel stays the loop's
// until a scene plays.
void begin();

// What /schedule adds for a scene player: "&in=a,b&sev=<last event seen>".
String scheduleParams();

// The /schedule answer: the scene to play (or none) and its new events.
void onSchedule(const String &payload);

// From loop(): downloads a new bundle, ends a finished one, posts reports.
void service();

bool playing();

// The board is now held at `turn` (OrientationHAL, clockwise): a bundle with
// `layouts` (scene_turn1) moves to the page for it at once, keeping its
// states and events; otherwise (a square board turns the picture itself) the
// scene is drawn again.
void onTurn(int turn);

} // namespace ScenePlayer

#endif
