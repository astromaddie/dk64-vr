#include "sfvr/sfvr_room.h"
#include "sfvr_test.h"

static void test_defaults(void)
{
	sfvr_room_config c;
	sfvr_room_state s;

	sfvr_room_config_default(&c);
	CHECK_NEAR(c.step_limit, 0.5, 1e-6);
	CHECK_NEAR(c.lean_limit, 0.35, 1e-6);
	sfvr_room_init(&s);
	CHECK_INT(s.held, 1);
}

static void test_seed_then_step(void)
{
	sfvr_room_state s;
	sfvr_room_config c;
	float head[2] = { 1.0f, -2.0f }, step[2] = { 9.0f, 9.0f };

	sfvr_room_config_default(&c);
	sfvr_room_init(&s);
	/* the first call seeds and returns 0 */
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 0);
	CHECK_NEAR(step[0], 0.0, 1e-9);
	CHECK_NEAR(step[1], 0.0, 1e-9);
	CHECK_INT(s.held, 0);
	CHECK_NEAR(s.now[0], 1.0, 1e-9);
	CHECK_NEAR(s.now[1], -2.0, 1e-9);
	CHECK_NEAR(s.previous[0], 1.0, 1e-9);

	/* standing still is a (zero) step to apply */
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	CHECK_NEAR(step[0], 0.0, 1e-9);

	/* walking: step is head minus standing point */
	head[0] = 1.1f;
	head[1] = -2.2f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	CHECK_NEAR(step[0], 0.1, 1e-6);
	CHECK_NEAR(step[1], -0.2, 1e-6);
	/* stepping does not itself move the standing point; moved() does */
	CHECK_NEAR(s.now[0], 1.0, 1e-9);
	sfvr_room_moved(&s, head);
	CHECK_NEAR(s.previous[0], 1.0, 1e-6);
	CHECK_NEAR(s.previous[1], -2.0, 1e-6);
	CHECK_NEAR(s.now[0], 1.1, 1e-6);
	CHECK_NEAR(s.now[1], -2.2, 1e-6);
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	CHECK_NEAR(step[0], 0.0, 1e-6);
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, NULL), 1); /* output optional */
}

static void test_moved_takes_whole_step_even_if_blocked(void)
{
	sfvr_room_state s;
	sfvr_room_config c;
	float head[2] = { 0.0f, 0.0f }, step[2];

	sfvr_room_config_default(&c);
	sfvr_room_init(&s);
	sfvr_room_step(&s, &c, head, 1, step);
	head[0] = 0.3f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	/* the game's wall stopped the player entirely; moved() still takes the step */
	sfvr_room_moved(&s, head);
	CHECK_NEAR(s.now[0], 0.3, 1e-6);
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	CHECK_NEAR(step[0], 0.0, 1e-6); /* the blocked step is not retried */
}

static void test_step_limit(void)
{
	sfvr_room_state s;
	sfvr_room_config c;
	float head[2] = { 0.0f, 0.0f }, step[2] = { 5.0f, 5.0f };

	sfvr_room_config_default(&c);
	sfvr_room_init(&s);
	sfvr_room_step(&s, &c, head, 1, step);

	/* exactly at the limit still walks */
	head[0] = 0.5f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	CHECK_NEAR(step[0], 0.5, 1e-6);

	/* longer: taken up without moving, 2-D distance counts */
	head[0] = 0.4f;
	head[1] = 0.4f; /* sqrt(0.32) = 0.566 */
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 0);
	CHECK_NEAR(step[0], 0.0, 1e-9);
	CHECK_NEAR(step[1], 0.0, 1e-9);
	CHECK_NEAR(s.now[0], 0.4, 1e-6);
	CHECK_NEAR(s.now[1], 0.4, 1e-6);
	CHECK_NEAR(s.previous[0], 0.4, 1e-6);
	CHECK_INT(s.held, 0);
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1); /* and walking resumes at once */
	CHECK_NEAR(step[0], 0.0, 1e-9);

	/* a large jump, e.g. a system recentre */
	head[0] = 10.0f;
	head[1] = -10.0f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 0);
	head[0] = 10.1f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	CHECK_NEAR(step[0], 0.1, 1e-5);

	/* custom limit */
	c.step_limit = 0.1f;
	head[0] = 10.3f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 0);
}

static void test_hold(void)
{
	sfvr_room_state s;
	sfvr_room_config c;
	float head[2] = { 0.0f, 0.0f }, step[2];

	sfvr_room_config_default(&c);
	sfvr_room_init(&s);
	sfvr_room_step(&s, &c, head, 1, step);
	head[0] = 0.2f;
	sfvr_room_step(&s, &c, head, 1, step);
	sfvr_room_moved(&s, head); /* previous 0.0, now 0.2 */

	/* tracking invalid: hold, previous = now, no step */
	head[0] = 0.3f;
	step[0] = 7.0f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 0, step), 0);
	CHECK_NEAR(step[0], 0.0, 1e-9);
	CHECK_INT(s.held, 1);
	CHECK_NEAR(s.previous[0], 0.2, 1e-6);
	CHECK_NEAR(s.now[0], 0.2, 1e-6);

	/* explicit hold */
	sfvr_room_hold(&s);
	CHECK_INT(s.held, 1);

	/* resumes with a seed (the standing point jumps to the head, no step), then walks */
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 0);
	CHECK_NEAR(s.now[0], 0.3, 1e-6);
	CHECK_INT(s.held, 0);
	head[0] = 0.35f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	CHECK_NEAR(step[0], 0.05, 1e-6);

	/* non-finite head is a hold too */
	head[0] = NAN;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 0);
	CHECK_INT(s.held, 1);
	head[0] = 0.0f;
	head[1] = INFINITY;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 0);
	CHECK_INT(sfvr_room_step(NULL, &c, head, 1, step), 0);
}

static void test_recentre(void)
{
	sfvr_room_state s;
	sfvr_room_config c;
	float head[2] = { 3.0f, 4.0f }, step[2];

	sfvr_room_config_default(&c);
	sfvr_room_init(&s);
	sfvr_room_step(&s, &c, head, 1, step);
	head[0] = 3.2f;
	sfvr_room_moved(&s, head);

	{
		float at[2] = { 0.5f, -0.5f };

		sfvr_room_recentre(&s, at);
	}
	CHECK_NEAR(s.previous[0], 0.5, 1e-9);
	CHECK_NEAR(s.now[0], 0.5, 1e-9);
	CHECK_NEAR(s.now[1], -0.5, 1e-9);
	CHECK_INT(s.held, 1);
	/* the next step re-seeds wherever the head now is */
	head[0] = 0.4f;
	head[1] = -0.4f;
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 0);
	CHECK_NEAR(s.now[0], 0.4, 1e-6);
	CHECK_INT(sfvr_room_step(&s, &c, head, 1, step), 1);
	sfvr_room_recentre(NULL, head);
}

static void test_lean(void)
{
	sfvr_room_state s;
	sfvr_room_config c;
	float head[3] = { 1.2f, 1.6f, 0.0f }, out[3];

	sfvr_room_config_default(&c);
	sfvr_room_init(&s);
	s.previous[0] = 1.0f;
	s.previous[1] = 0.0f;
	s.now[0] = 1.1f;
	s.now[1] = 0.0f;

	/* head minus standing point, y is the raw head height; but clamped to 0.35 in 3-D */
	sfvr_room_lean(&s, &c, head, 0.0f, out);
	CHECK_NEAR(sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]), 0.35, 1e-5);
	CHECK(out[1] > 0.0f && out[0] > 0.0f);
	CHECK_NEAR(out[0] / out[1], 0.2 / 1.6, 1e-5); /* direction kept */

	/* below the limit passes through untouched; blend moves the standing point */
	head[1] = 0.0f;
	head[0] = 1.3f;
	sfvr_room_lean(&s, &c, head, 0.0f, out);
	CHECK_NEAR(out[0], 0.3, 1e-6);
	CHECK_NEAR(out[1], 0.0, 1e-6);
	sfvr_room_lean(&s, &c, head, 1.0f, out);
	CHECK_NEAR(out[0], 0.2, 1e-6);
	sfvr_room_lean(&s, &c, head, 0.5f, out);
	CHECK_NEAR(out[0], 0.25, 1e-6);
	sfvr_room_lean(&s, &c, head, -3.0f, out); /* t clamped to 0..1 */
	CHECK_NEAR(out[0], 0.3, 1e-6);
	sfvr_room_lean(&s, &c, head, 7.0f, out);
	CHECK_NEAR(out[0], 0.2, 1e-6);
	sfvr_room_lean(&s, &c, head, NAN, out);
	CHECK_NEAR(out[0], 0.3, 1e-6);

	/* z axis and custom limit */
	head[0] = 1.1f;
	head[2] = -0.6f;
	c.lean_limit = 0.15f;
	sfvr_room_lean(&s, &c, head, 1.0f, out);
	CHECK_NEAR(out[0], 0.0, 1e-6);
	CHECK_NEAR(out[2], -0.15, 1e-6);

	/* no state: standing point at the origin; NULL config: defaults */
	head[0] = 0.1f;
	head[2] = 0.1f;
	sfvr_room_lean(NULL, NULL, head, 0.5f, out);
	CHECK_NEAR(out[0], 0.1, 1e-6);
	CHECK_NEAR(out[2], 0.1, 1e-6);
}

static void test_lean_fixed(void)
{
	float origin[3] = { 1.0f, 0.0f, 2.0f }, head[3] = { 1.0f, 1.0f, 2.0f }, out[3];

	sfvr_room_lean_fixed(origin, head, 0.35f, out);
	CHECK_NEAR(out[0], 0.0, 1e-6);
	CHECK_NEAR(out[1], 0.35, 1e-6); /* clamped */
	CHECK_NEAR(out[2], 0.0, 1e-6);

	head[1] = 0.2f;
	head[0] = 1.1f;
	sfvr_room_lean_fixed(origin, head, 0.35f, out);
	CHECK_NEAR(out[0], 0.1, 1e-6);
	CHECK_NEAR(out[1], 0.2, 1e-6);
	CHECK_NEAR(out[2], 0.0, 1e-6);

	/* a 3-4-5 offset clamps to the limit, direction kept */
	head[0] = 1.3f;
	head[1] = 0.4f;
	head[2] = 2.0f;
	sfvr_room_lean_fixed(origin, head, 0.25f, out);
	CHECK_NEAR(out[0], 0.15, 1e-6);
	CHECK_NEAR(out[1], 0.20, 1e-6);

	/* limit 0 or less: no lean */
	sfvr_room_lean_fixed(origin, head, 0.0f, out);
	CHECK_NEAR(out[0], 0.0, 1e-9);
	CHECK_NEAR(out[1], 0.0, 1e-9);
	sfvr_room_lean_fixed(origin, head, -1.0f, out);
	CHECK_NEAR(out[0], 0.0, 1e-9);
	sfvr_room_lean_fixed(origin, head, 1.0f, NULL);
}

int main(void)
{
	RUN(test_defaults);
	RUN(test_seed_then_step);
	RUN(test_moved_takes_whole_step_even_if_blocked);
	RUN(test_step_limit);
	RUN(test_hold);
	RUN(test_recentre);
	RUN(test_lean);
	RUN(test_lean_fixed);
	return SFVR_TEST_RESULT("test_room");
}
