#include "sfvr/sfvr_haptics.h"
#include "sfvr_test.h"

typedef struct recorder {
	int count;
	int hand[4];
	float amplitude[4];
	float seconds[4];
	void *seen_user;
} recorder;

static void record(int hand, float amplitude, float seconds, void *user)
{
	recorder *r = (recorder *)user;

	if (r->count < 4)
	{
		r->hand[r->count] = hand;
		r->amplitude[r->count] = amplitude;
		r->seconds[r->count] = seconds;
	}
	r->count++;
	r->seen_user = user;
}

static void test_table(void)
{
	sfvr_haptic_pulse p;

	p = sfvr_haptic_default(SFVR_HAPTIC_UI_HOVER);
	CHECK_NEAR(p.amplitude, 0.2, 1e-6);
	CHECK_NEAR(p.seconds, 0.010, 1e-6);
	p = sfvr_haptic_default(SFVR_HAPTIC_UI_CLICK);
	CHECK_NEAR(p.amplitude, 0.5, 1e-6);
	CHECK_NEAR(p.seconds, 0.030, 1e-6);
	p = sfvr_haptic_default(SFVR_HAPTIC_SYSTEM);
	CHECK_NEAR(p.amplitude, 0.6, 1e-6);
	CHECK_NEAR(p.seconds, 0.080, 1e-6);
	p = sfvr_haptic_default(SFVR_HAPTIC_GESTURE);
	CHECK(p.amplitude >= 0.4f && p.amplitude <= 0.6f);
	CHECK(p.seconds >= 0.030f && p.seconds <= 0.060f);
	p = sfvr_haptic_default(SFVR_HAPTIC_WEAPON_FIRE);
	CHECK_NEAR(p.amplitude, 0.6, 1e-6);
	CHECK_NEAR(p.seconds, 0.060, 1e-6);
	p = sfvr_haptic_default(SFVR_HAPTIC_IMPACT);
	CHECK(p.amplitude >= 0.35f && p.amplitude <= 1.0f);
	CHECK(p.seconds >= 0.030f && p.seconds <= 0.080f);
	p = sfvr_haptic_default(SFVR_HAPTIC_RUMBLE);
	CHECK_NEAR(p.seconds, 0.040, 1e-6);
	p = sfvr_haptic_default(SFVR_HAPTIC_EVENT_COUNT);
	CHECK_NEAR(p.amplitude, 0.0, 1e-9);
	CHECK_STR(sfvr_haptic_event_name(SFVR_HAPTIC_UI_HOVER), "ui_hover");
	CHECK_STR(sfvr_haptic_event_name(SFVR_HAPTIC_WEAPON_FIRE), "weapon_fire");
	CHECK_STR(sfvr_haptic_event_name(SFVR_HAPTIC_EVENT_COUNT), "unknown");
}

static void test_coalescing(void)
{
	sfvr_haptic_queue q = { { 0 }, { 0 } };
	recorder r = { 0 };

	/* strongest amplitude and longest duration are kept independently */
	sfvr_haptic_submit(&q, 0, 0.3f, 0.100f);
	sfvr_haptic_submit(&q, 0, 0.8f, 0.020f);
	sfvr_haptic_submit(&q, 0, 0.5f, 0.050f);
	CHECK_NEAR(q.amplitude[0], 0.8, 1e-6);
	CHECK_NEAR(q.seconds[0], 0.100, 1e-6);
	CHECK_NEAR(q.amplitude[1], 0.0, 1e-9);
	sfvr_haptic_submit(&q, SFVR_HAND_RIGHT, 0.4f, 0.030f);
	CHECK_INT(sfvr_haptic_flush(&q, 1.0f, record, &r), 2);
	CHECK_INT(r.count, 2);
	CHECK_INT(r.hand[0], 0);
	CHECK_NEAR(r.amplitude[0], 0.8, 1e-6);
	CHECK_NEAR(r.seconds[0], 0.100, 1e-6);
	CHECK_INT(r.hand[1], 1);
	CHECK_NEAR(r.amplitude[1], 0.4, 1e-6);
	CHECK_NEAR(r.seconds[1], 0.030, 1e-6);
	CHECK(r.seen_user == &r);
}

static void test_both_and_events(void)
{
	sfvr_haptic_queue q = { { 0 }, { 0 } };
	recorder r = { 0 };

	sfvr_haptic_submit(&q, SFVR_HAND_BOTH, 0.5f, 0.040f);
	CHECK_NEAR(q.amplitude[0], 0.5, 1e-6);
	CHECK_NEAR(q.amplitude[1], 0.5, 1e-6);
	sfvr_haptic_event(&q, SFVR_HAND_RIGHT, SFVR_HAPTIC_UI_CLICK); /* 0.5/30ms: no stronger, shorter */
	sfvr_haptic_event(&q, SFVR_HAND_LEFT, SFVR_HAPTIC_SYSTEM);   /* both hands whatever hand says */
	CHECK_NEAR(q.amplitude[0], 0.6, 1e-6);
	CHECK_NEAR(q.amplitude[1], 0.6, 1e-6);
	CHECK_NEAR(q.seconds[0], 0.080, 1e-6);
	CHECK_NEAR(q.seconds[1], 0.080, 1e-6);
	sfvr_haptic_flush(&q, 1.0f, record, &r);
	CHECK_INT(r.count, 2);

	/* single-hand events use the table */
	sfvr_haptic_event(&q, SFVR_HAND_RIGHT, SFVR_HAPTIC_UI_HOVER);
	CHECK_NEAR(q.amplitude[0], 0.0, 1e-9);
	CHECK_NEAR(q.amplitude[1], 0.2, 1e-6);
	CHECK_NEAR(q.seconds[1], 0.010, 1e-6);
	sfvr_haptic_event(&q, SFVR_HAND_BOTH, SFVR_HAPTIC_WEAPON_FIRE);
	CHECK_NEAR(q.amplitude[0], 0.6, 1e-6);
	CHECK_NEAR(q.amplitude[1], 0.6, 1e-6);
}

static void test_invalid_input(void)
{
	sfvr_haptic_queue q = { { 0 }, { 0 } };

	sfvr_haptic_submit(&q, 3, 1.0f, 1.0f);
	sfvr_haptic_submit(&q, -1, 1.0f, 1.0f);
	sfvr_haptic_submit(NULL, 0, 1.0f, 1.0f);
	sfvr_haptic_submit(&q, 0, NAN, NAN);
	sfvr_haptic_submit(&q, 1, -0.5f, -1.0f);
	sfvr_haptic_event(&q, 0, SFVR_HAPTIC_EVENT_COUNT);
	CHECK_NEAR(q.amplitude[0], 0.0, 1e-9);
	CHECK_NEAR(q.amplitude[1], 0.0, 1e-9);
	CHECK_NEAR(q.seconds[0], 0.0, 1e-9);
	CHECK_NEAR(q.seconds[1], 0.0, 1e-9);
	CHECK_INT(sfvr_haptic_flush(NULL, 1.0f, NULL, NULL), 0);
}

static void test_flush(void)
{
	sfvr_haptic_queue q = { { 0 }, { 0 } };
	recorder r = { 0 };

	/* strength scales amplitude, not duration */
	sfvr_haptic_submit(&q, 0, 0.8f, 0.1f);
	CHECK_INT(sfvr_haptic_flush(&q, 0.5f, record, &r), 1);
	CHECK_NEAR(r.amplitude[0], 0.4, 1e-6);
	CHECK_NEAR(r.seconds[0], 0.1, 1e-6);

	/* the queue is cleared by a flush */
	r.count = 0;
	CHECK_INT(sfvr_haptic_flush(&q, 1.0f, record, &r), 0);
	CHECK_INT(r.count, 0);

	/* strength 0 plays nothing but still clears */
	sfvr_haptic_submit(&q, 1, 1.0f, 0.1f);
	CHECK_INT(sfvr_haptic_flush(&q, 0.0f, record, &r), 0);
	CHECK_INT(r.count, 0);
	CHECK_INT(sfvr_haptic_flush(&q, 1.0f, record, &r), 0);

	/* strength over 1 is clamped to 1; amplitude over 1 clamps to 1 */
	sfvr_haptic_submit(&q, 0, 0.6f, 0.1f);
	sfvr_haptic_flush(&q, 5.0f, record, &r);
	CHECK_NEAR(r.amplitude[0], 0.6, 1e-6);
	r.count = 0;
	sfvr_haptic_submit(&q, 0, 7.0f, 0.1f);
	sfvr_haptic_flush(&q, 1.0f, record, &r);
	CHECK_NEAR(r.amplitude[0], 1.0, 1e-9);

	/* negative or NaN strength is 0 */
	r.count = 0;
	sfvr_haptic_submit(&q, 0, 1.0f, 0.1f);
	CHECK_INT(sfvr_haptic_flush(&q, -1.0f, record, &r), 0);
	sfvr_haptic_submit(&q, 0, 1.0f, 0.1f);
	CHECK_INT(sfvr_haptic_flush(&q, NAN, record, &r), 0);
	CHECK_INT(r.count, 0);

	/* zero-amplitude submissions are skipped; a NULL callback is fine */
	sfvr_haptic_submit(&q, 0, 0.0f, 0.5f);
	CHECK_INT(sfvr_haptic_flush(&q, 1.0f, record, &r), 0);
	sfvr_haptic_submit(&q, 0, 0.5f, 0.5f);
	CHECK_INT(sfvr_haptic_flush(&q, 1.0f, NULL, NULL), 1);
	CHECK_NEAR(q.amplitude[0], 0.0, 1e-9);
}

static void test_rumble(void)
{
	sfvr_haptic_queue q = { { 0 }, { 0 } };

	CHECK_NEAR(sfvr_rumble_to_haptic(0, 0), 0.0, 1e-9);
	CHECK_NEAR(sfvr_rumble_to_haptic(65535, 0), 1.0, 1e-6);
	CHECK_NEAR(sfvr_rumble_to_haptic(0, 65535), 1.0, 1e-6);
	CHECK_NEAR(sfvr_rumble_to_haptic(32768, 16384), 32768.0 / 65535.0, 1e-6);
	CHECK_NEAR(sfvr_rumble_to_haptic(16384, 32768), 32768.0 / 65535.0, 1e-6);
	CHECK_NEAR(sfvr_rumble_to_haptic(100000, 5), 1.0, 1e-6);
	CHECK_NEAR(SFVR_RUMBLE_PULSE_SECONDS, 0.040, 1e-9);
	sfvr_haptic_rumble(&q, SFVR_HAND_BOTH, 65535, 1000);
	CHECK_NEAR(q.amplitude[0], 1.0, 1e-6);
	CHECK_NEAR(q.seconds[1], 0.040, 1e-6);
}

int main(void)
{
	RUN(test_table);
	RUN(test_coalescing);
	RUN(test_both_and_events);
	RUN(test_invalid_input);
	RUN(test_flush);
	RUN(test_rumble);
	return SFVR_TEST_RESULT("test_haptics");
}
