#ifndef WINDOW_TRAFFIC_LIGHT_H
#define WINDOW_TRAFFIC_LIGHT_H

#include "vex.h"

/**
 * window/traffic_light.h, macOS traffic-light chrome management.
 *
 * Segregated component managing macOS traffic-light buttons (close, minimize,
 * zoom) layout, visibility, and header positioning. Decoupled from the core
 * Window state per the Single Class Per File Law and the Window Decoupling Law.
 */

/**
 * macOS window traffic-light button indices.
 *
 * TRAFFIC_LIGHT_CLOSE       - Red close button (0)
 * TRAFFIC_LIGHT_MINIATURIZE - Yellow minimize button (1)
 * TRAFFIC_LIGHT_ZOOM        - Green zoom/fullscreen button (2)
 * TRAFFIC_LIGHT_COUNT       - Total button count (3)
 */
typedef enum TrafficLightButton {
    TRAFFIC_LIGHT_CLOSE = 0,
    TRAFFIC_LIGHT_MINIATURIZE = 1,
    TRAFFIC_LIGHT_ZOOM = 2,
    TRAFFIC_LIGHT_COUNT = 3
} TrafficLightButton;

typedef struct TrafficLight TrafficLight;

/**
 * Creates a traffic-light chrome controller bound to a native window handle.
 * @param nsWindowHandle opaque NSWindow handle
 * @return allocated TrafficLight controller, or nullptr on failure
 */
TrafficLight *TrafficLight_1(any nsWindowHandle);
#define TrafficLight(nsWindowHandle) TrafficLight_1((nsWindowHandle))

/**
 * Destroys a traffic-light chrome controller.
 * @param self traffic-light controller to release
 */
fn TrafficLight_destroy(TrafficLight *self);

/**
 * Shows or hides a specific traffic-light button.
 * @param self    traffic-light controller
 * @param button  button index to update
 * @param visible true to show the button, false to hide it
 */
fn TrafficLight_setButtonVisible(TrafficLight *self, TrafficLightButton button, bool visible);

/**
 * Queries whether a specific traffic-light button is visible.
 * @param self   traffic-light controller
 * @param button button index to query
 * @return true if the button is visible
 */
bool TrafficLight_isButtonVisible(const TrafficLight *self, TrafficLightButton button);

/**
 * Sets the content-relative header position for the traffic-light cluster.
 * @param self traffic-light controller
 * @param x    horizontal offset in points
 * @param y    vertical offset in points
 */
fn TrafficLight_setHeaderPosition(TrafficLight *self, f32 x, f32 y);

/**
 * Reads the content-relative header position for the traffic-light cluster.
 * @param self traffic-light controller
 * @param outX destination for horizontal offset
 * @param outY destination for vertical offset
 */
fn TrafficLight_getHeaderPosition(const TrafficLight *self, f32 *outX, f32 *outY);

/**
 * Resets cached base coordinates so the next refresh re-measures native positions.
 * @param self traffic-light controller
 */
fn TrafficLight_resetBase(TrafficLight *self);

/**
 * Applies the current visibility and header position to native window chrome.
 * @param self traffic-light controller
 */
fn TrafficLight_refresh(TrafficLight *self);

/**
 * Enables or disables floating chrome positioning.
 * @param self    traffic-light controller
 * @param f32ing  true to enable floating mode
 */
fn TrafficLight_setFloating(TrafficLight *self, bool f32ing);

#endif
