// Proves the umbrella header and the library link and work from C++.
#include "sfvr/sfvr.h"

#include <cstdio>
#include <cstring>

int main()
{
	int failures = 0;
	sfvr_view_button button = {};
	sfvr_turn_state turn = {};
	sfvr_haptic_queue queue = {};
	sfvr_room_state room;
	sfvr_perf perf;
	char line[160];
	float head[2] = { 0.0f, 0.0f }, step[2];
	float eye[2] = { 1.0f, 1.0f };

	sfvr_view_update(&button, nullptr, 0, 0.0);
	sfvr_view_update(&button, nullptr, 1, 1.0);
	if (sfvr_view_update(&button, nullptr, 0, 1.2) != SFVR_VIEW_SHORT_PRESS)
		failures++;
	if (sfvr_turn_update(&turn, nullptr, 1.0f, 0.01f) != 30.0f)
		failures++;
	sfvr_haptic_event(&queue, SFVR_HAND_BOTH, SFVR_HAPTIC_UI_CLICK);
	if (sfvr_haptic_flush(&queue, 1.0f, nullptr, nullptr) != 2)
		failures++;
	sfvr_room_init(&room);
	if (sfvr_room_step(&room, nullptr, head, 1, step) != 0)
		failures++;
	sfvr_perf_init(&perf, 1.0, nullptr, 0);
	sfvr_perf_add_frame(&perf, 0.0, 0, nullptr, eye, -1.0f);
	if (!sfvr_perf_poll(&perf, 1.0, line, sizeof(line)) || std::strncmp(line, "[vr-perf] fps=1.0", 17) != 0)
		failures++;
	if (!sfvr_settings_find("snap_angle") || SFVR_SETTINGS_SCHEMA != 1)
		failures++;
	if (SFVR_VERSION_MAJOR != 0 || SFVR_VERSION_MINOR != 1 || SFVR_VERSION_PATCH != 0)
		failures++;
	std::printf("test_cxx: %d failures\n", failures);
	return failures ? 1 : 0;
}
