#pragma once
/**
 * Which edge is up, from the board's accelerometer (self_orient1).
 *
 * A board with WIGNET_HAS_IMU finds how it is held and turns the picture
 * itself: the server always sends the upright page (docs/lcd_154.md). The
 * turn is a clockwise quarter turn of that page on its way to the panel,
 * the same meaning as /schedule's ori= and the platform's orientations.
 */
namespace OrientationHAL {

// Starts the sensor (I2C) and reads the turn once, before the first draw.
// False when no sensor answers: the turn then stays 0.
bool begin();

// Reads the sensor every few hundred ms; returns true when the turn has
// changed (held steadily for a moment, not while lying flat).
bool poll();

// Polls in a task of its own and hands each new turn to `onTurn`, so the
// screen turns even while setup() sits in the Wi-Fi portal or the loop
// waits on a download. The sensor's I2C bus is then this task's alone.
void startTask(void (*onTurn)(int degrees));

// The current turn: 0, 90, 180 or 270.
int turn();

// The last reading in g, in the panel's frame as an unturned page shows:
// x the component pointing up along the screen's x (right), y along its y
// (up), z out of the screen's face. False until a reading came in.
bool gravity(float &x, float &y, float &z);

// While a scene follows the tilt (motion1): read every 20 ms instead of 200,
// and hold the turn still, so tilting does not turn the page under it.
void follow(bool on);

} // namespace OrientationHAL
