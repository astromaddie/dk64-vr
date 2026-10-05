#include "sfvr/sfvr_turn.h"
#include "sfvr_test.h"

static void test_defaults(void)
{
	sfvr_turn_config c;

	sfvr_turn_config_default(&c);
	CHECK_INT(c.mode, SFVR_TURN_SNAP);
	CHECK_NEAR(c.snap_angle_deg, 30.0, 1e-9);
	CHECK_NEAR(c.arm_threshold, 0.7, 1e-6);
	CHECK_NEAR(c.rearm_threshold, 0.3, 1e-6);
	CHECK_NEAR(c.smooth_speed_dps, 120.0, 1e-9);
	CHECK_NEAR(c.smooth_deadzone, 0.2, 1e-6);
	CHECK_NEAR(c.max_dt_s, 0.1, 1e-6);
}

static void test_snap_sign_and_angle(void)
{
	sfvr_turn_state s = { 0 };

	CHECK_NEAR(sfvr_turn_update(&s, NULL, 1.0f, 0.014f), 30.0, 1e-6); /* right = positive */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.0f, 0.014f), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, NULL, -1.0f, 0.014f), -30.0, 1e-6); /* left = negative */
}

static void test_snap_hysteresis(void)
{
	sfvr_turn_state s = { 0 };

	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.69f, 0.01f), 0.0, 1e-6); /* below arm */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.70f, 0.01f), 30.0, 1e-6); /* arm threshold is inclusive */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 1.0f, 0.01f), 0.0, 1e-6);  /* held: only once */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.5f, 0.01f), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.3f, 0.01f), 0.0, 1e-6); /* 0.3 is not below 0.3 */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.9f, 0.01f), 0.0, 1e-6); /* not re-armed yet */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.29f, 0.01f), 0.0, 1e-6); /* re-arms */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.6f, 0.01f), 0.0, 1e-6);  /* armed but below 0.7 */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.8f, 0.01f), 30.0, 1e-6);
	/* flick straight through the other side re-arms through the dead band */
	CHECK_NEAR(sfvr_turn_update(&s, NULL, -0.9f, 0.01f), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, NULL, 0.0f, 0.01f), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, NULL, -0.9f, 0.01f), -30.0, 1e-6);
}

static void test_snap_config(void)
{
	sfvr_turn_state s = { 0 };
	sfvr_turn_config c;

	sfvr_turn_config_default(&c);
	c.snap_angle_deg = 45.0f;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.01f), 45.0, 1e-6);
	sfvr_turn_update(&s, &c, 0.0f, 0.01f);
	c.snap_angle_deg = 50.0f; /* normalised to the nearest allowed (45) */
	CHECK_NEAR(sfvr_turn_update(&s, &c, -1.0f, 0.01f), -45.0, 1e-6);
	sfvr_turn_reset(&s);
	c.snap_angle_deg = 90.0f;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.01f), 90.0, 1e-6);
	sfvr_turn_reset(&s); /* reset re-arms */
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.01f), 90.0, 1e-6);
	c.arm_threshold = 0.5f;
	c.rearm_threshold = 0.1f;
	sfvr_turn_reset(&s);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 0.5f, 0.01f), 90.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 0.2f, 0.01f), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 0.6f, 0.01f), 0.0, 1e-6);
}

static void test_normalise_snap_angle(void)
{
	CHECK_INT(sfvr_turn_normalise_snap_angle(15.0f), 15);
	CHECK_INT(sfvr_turn_normalise_snap_angle(30.0f), 30);
	CHECK_INT(sfvr_turn_normalise_snap_angle(45.0f), 45);
	CHECK_INT(sfvr_turn_normalise_snap_angle(60.0f), 60);
	CHECK_INT(sfvr_turn_normalise_snap_angle(90.0f), 90);
	CHECK_INT(sfvr_turn_normalise_snap_angle(0.0f), 15);
	CHECK_INT(sfvr_turn_normalise_snap_angle(-45.0f), 15);
	CHECK_INT(sfvr_turn_normalise_snap_angle(22.0f), 15);
	CHECK_INT(sfvr_turn_normalise_snap_angle(22.5f), 15); /* tie goes to the smaller */
	CHECK_INT(sfvr_turn_normalise_snap_angle(23.0f), 30);
	CHECK_INT(sfvr_turn_normalise_snap_angle(37.5f), 30);
	CHECK_INT(sfvr_turn_normalise_snap_angle(38.0f), 45);
	CHECK_INT(sfvr_turn_normalise_snap_angle(52.5f), 45);
	CHECK_INT(sfvr_turn_normalise_snap_angle(53.0f), 60);
	CHECK_INT(sfvr_turn_normalise_snap_angle(75.0f), 60);
	CHECK_INT(sfvr_turn_normalise_snap_angle(76.0f), 90);
	CHECK_INT(sfvr_turn_normalise_snap_angle(180.0f), 90);
	CHECK_INT(sfvr_turn_normalise_snap_angle(NAN), 30);
}

static void test_smooth(void)
{
	sfvr_turn_state s = { 0 };
	sfvr_turn_config c;

	sfvr_turn_config_default(&c);
	c.mode = SFVR_TURN_SMOOTH;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 0.0f, 0.01f), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 0.2f, 0.01f), 0.0, 1e-6);   /* inside the deadzone */
	CHECK_NEAR(sfvr_turn_update(&s, &c, -0.15f, 0.01f), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.01f), 1.2, 1e-5);   /* full stick: 120 deg/s */
	CHECK_NEAR(sfvr_turn_update(&s, &c, -1.0f, 0.01f), -1.2, 1e-5); /* left is negative */
	CHECK_NEAR(sfvr_turn_update(&s, &c, 0.6f, 0.01f), 0.6, 1e-5);   /* (0.6-0.2)/0.8 = 0.5 -> 60 deg/s */
	CHECK_NEAR(sfvr_turn_update(&s, &c, 0.2001f, 1.0f), 0.0, 0.01);  /* just past the deadzone starts from ~0 */
	CHECK_NEAR(sfvr_turn_update(&s, &c, 2.0f, 0.01f), 1.2, 1e-5);   /* stick clamped to 1 */
	CHECK_NEAR(sfvr_turn_deadzoned(0.6f, 0.2f), 0.5, 1e-6);
	CHECK_NEAR(sfvr_turn_deadzoned(-0.6f, 0.2f), -0.5, 1e-6);
	CHECK_NEAR(sfvr_turn_deadzoned(1.0f, 0.2f), 1.0, 1e-6);
	CHECK_NEAR(sfvr_turn_deadzoned(0.5f, 0.0f), 0.5, 1e-6);
}

static void test_smooth_dt_cap(void)
{
	sfvr_turn_state s = { 0 };
	sfvr_turn_config c;

	sfvr_turn_config_default(&c);
	c.mode = SFVR_TURN_SMOOTH;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.1f), 12.0, 1e-4);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.5f), 12.0, 1e-4);  /* capped at 0.1 s */
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 30.0f), 12.0, 1e-4);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, -1.0f), 0.0, 1e-6);  /* no negative time */
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.0f), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, NAN), 0.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, &c, NAN, 0.05f), 0.0, 1e-6);
	c.max_dt_s = 0.05f;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.1f), 6.0, 1e-4);
}

static void test_smooth_speed_clamp(void)
{
	sfvr_turn_state s = { 0 };
	sfvr_turn_config c;

	sfvr_turn_config_default(&c);
	c.mode = SFVR_TURN_SMOOTH;
	c.smooth_speed_dps = 1000.0f;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.1f), 36.0, 1e-4);
	c.smooth_speed_dps = 1.0f;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.1f), 3.0, 1e-4);
	CHECK_NEAR(sfvr_turn_normalise_smooth_speed(200.0f), 200.0, 1e-9);
	CHECK_NEAR(sfvr_turn_normalise_smooth_speed(NAN), 120.0, 1e-9);
}

static void test_off_and_mode_switch(void)
{
	sfvr_turn_state s = { 0 };
	sfvr_turn_config c;

	sfvr_turn_config_default(&c);
	c.mode = SFVR_TURN_OFF;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.1f), 0.0, 1e-9);
	CHECK_NEAR(sfvr_turn_update(&s, &c, -1.0f, 0.1f), 0.0, 1e-9);

	/* snap fired, then the user switches to smooth and back: armed again */
	c.mode = SFVR_TURN_SNAP;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.01f), 30.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.01f), 0.0, 1e-6);
	c.mode = SFVR_TURN_SMOOTH;
	sfvr_turn_update(&s, &c, 1.0f, 0.01f);
	c.mode = SFVR_TURN_SNAP;
	CHECK_NEAR(sfvr_turn_update(&s, &c, 1.0f, 0.01f), 30.0, 1e-6);
	CHECK_NEAR(sfvr_turn_update(NULL, &c, 1.0f, 0.01f), 0.0, 1e-9);
}

int main(void)
{
	RUN(test_defaults);
	RUN(test_snap_sign_and_angle);
	RUN(test_snap_hysteresis);
	RUN(test_snap_config);
	RUN(test_normalise_snap_angle);
	RUN(test_smooth);
	RUN(test_smooth_dt_cap);
	RUN(test_smooth_speed_clamp);
	RUN(test_off_and_mode_switch);
	return SFVR_TEST_RESULT("test_turn");
}
