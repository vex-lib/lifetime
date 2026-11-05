#ifndef WINDOW_TRAFFIC_LIGHT_H
#define WINDOW_TRAFFIC_LIGHT_H

#include <stdbool.h>

// window/traffic_light.h — macOS traffic-light chrome management.
//
// Segregated component managing macOS traffic-light buttons (close, minimize,
// zoom) layout, visibility, and header positioning. Decoupled from the core
// Window state per the Single Class Per File Law and the Window Decoupling Law.

typedef enum TrafficLightButton {
    TRAFFIC_LIGHT_CLOSE = 0,        // Red close button
    TRAFFIC_LIGHT_MINIATURIZE = 1,   // Yellow minimize button
    TRAFFIC_LIGHT_ZOOM = 2,          // Green zoom/fullscreen button
    TRAFFIC_LIGHT_COUNT = 3
} TrafficLightButton;

typedef struct TrafficLight TrafficLight;

// --- Constructors ---
TrafficLight *TrafficLight_create(void *nsWindowHandle);
void TrafficLight_destroy(TrafficLight *self);

// --- Visibility ---
void TrafficLight_setButtonVisible(TrafficLight *self, TrafficLightButton button, bool visible);
bool TrafficLight_isButtonVisible(const TrafficLight *self, TrafficLightButton button);

// --- Header Position (content-relative) ---
void TrafficLight_setHeaderPosition(TrafficLight *self, float x, float y);
void TrafficLight_getHeaderPosition(const TrafficLight *self, float *outX, float *outY);

// --- State & Layout ---
void TrafficLight_resetBase(TrafficLight *self);
void TrafficLight_refresh(TrafficLight *self);
void TrafficLight_setFloating(TrafficLight *self, bool floating);

#endif // WINDOW_TRAFFIC_LIGHT_H
