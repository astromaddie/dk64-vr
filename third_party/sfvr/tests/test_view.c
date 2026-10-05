#include "sfvr/sfvr_view.h"
#include "sfvr_test.h"

static void test_defaults(void)
{
	sfvr_view_config c;

	sfvr_view_config_default(&c);
	CHECK_NEAR(c.recentre_hold_s, 1.0, 1e-9);
	CHECK_NEAR(c.height_hold_s, 3.0, 1e-9);
}

static void test_short_press(void)
{
	sfvr_view_button b = { 0 };
	sfvr_view_config c;

	sfvr_view_config_default(&c);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 10.0), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 10.1), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 10.2), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 10.3), SFVR_VIEW_SHORT_PRESS);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 10.4), 0);
	/* just under the recentre hold is still short */
	CHECK_INT(sfvr_view_update(&b, &c, 1, 20.0), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 20.999), SFVR_VIEW_SHORT_PRESS);
	/* a one-frame tap: pressed and released on consecutive updates */
	CHECK_INT(sfvr_view_update(&b, &c, 1, 30.0), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 30.014), SFVR_VIEW_SHORT_PRESS);
}

static void test_hold_events(void)
{
	sfvr_view_button b = { 0 };
	sfvr_view_config c;

	sfvr_view_config_default(&c);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 100.0), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 100.5), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 101.0), 0); /* 0.5 s held */
	CHECK_INT(sfvr_view_update(&b, &c, 1, 101.4), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 101.5), SFVR_VIEW_RECENTRE); /* exactly 1.0 s */
	CHECK_INT(sfvr_view_update(&b, &c, 1, 101.6), 0);                  /* once only */
	CHECK_INT(sfvr_view_update(&b, &c, 1, 103.0), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 103.5), SFVR_VIEW_RECALIBRATE_HEIGHT); /* exactly 3.0 s */
	CHECK_INT(sfvr_view_update(&b, &c, 1, 104.0), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 105.0), 0); /* release after a hold: no short press */
	CHECK_INT(sfvr_view_update(&b, &c, 0, 105.1), 0);
}

static void test_release_after_recentre_only(void)
{
	sfvr_view_button b = { 0 };

	sfvr_view_update(&b, NULL, 0, 0.0);
	sfvr_view_update(&b, NULL, 1, 1.0);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 2.2), SFVR_VIEW_RECENTRE);
	CHECK_INT(sfvr_view_update(&b, NULL, 0, 2.5), 0);
	/* a later press starts afresh */
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 5.0), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 6.0), SFVR_VIEW_RECENTRE);
}

static void test_irregular_updates(void)
{
	sfvr_view_button b = { 0 };
	sfvr_view_config c;

	sfvr_view_config_default(&c);
	sfvr_view_update(&b, &c, 0, 0.0);
	sfvr_view_update(&b, &c, 1, 5.0);
	/* a long hitch crosses the first threshold only */
	CHECK_INT(sfvr_view_update(&b, &c, 1, 6.4), SFVR_VIEW_RECENTRE);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 9.0), SFVR_VIEW_RECALIBRATE_HEIGHT);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 30.0), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 31.0), 0);

	/* one huge gap crosses both at once */
	sfvr_view_update(&b, &c, 1, 40.0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 43.5), SFVR_VIEW_RECENTRE | SFVR_VIEW_RECALIBRATE_HEIGHT);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 44.0), 0);
	sfvr_view_update(&b, &c, 0, 44.1);

	/* released late, with no update in between: the hold still counts */
	sfvr_view_update(&b, &c, 1, 50.0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 50.9), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 51.2), SFVR_VIEW_RECENTRE);
	sfvr_view_update(&b, &c, 1, 60.0);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 63.2), SFVR_VIEW_RECENTRE | SFVR_VIEW_RECALIBRATE_HEIGHT);

	/* the clock going backwards counts as no time held */
	sfvr_view_update(&b, &c, 1, 70.0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 69.0), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 0, 69.5), SFVR_VIEW_SHORT_PRESS);
}

static void test_first_call_already_pressed(void)
{
	sfvr_view_button b = { 0 };

	CHECK_INT(sfvr_view_update(&b, NULL, 1, 5.0), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 6.5), 0); /* held, but not a press we saw start */
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 9.0), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 0, 9.1), 0); /* release: no short press either */
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 10.0), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 0, 10.2), SFVR_VIEW_SHORT_PRESS);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 11.0), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 12.0), SFVR_VIEW_RECENTRE);
}

static void test_reset(void)
{
	sfvr_view_button b = { 0 };

	sfvr_view_update(&b, NULL, 0, 0.0);
	sfvr_view_update(&b, NULL, 1, 1.0);
	sfvr_view_reset(&b);
	CHECK_INT(sfvr_view_update(&b, NULL, 0, 1.1), 0); /* cancelled: release gives no short press */
	CHECK_INT(sfvr_view_update(&b, NULL, 0, 1.2), 0);

	/* focus lost mid-hold, still held on return: nothing fires until a fresh press */
	sfvr_view_update(&b, NULL, 1, 2.0);
	sfvr_view_update(&b, NULL, 1, 2.5);
	sfvr_view_reset(&b);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 4.0), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 8.0), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 0, 8.1), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 9.0), 0);
	CHECK_INT(sfvr_view_update(&b, NULL, 0, 9.1), SFVR_VIEW_SHORT_PRESS);

	/* reset after a hold event cancels the rest of that hold */
	sfvr_view_update(&b, NULL, 1, 10.0);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 11.0), SFVR_VIEW_RECENTRE);
	sfvr_view_reset(&b);
	CHECK_INT(sfvr_view_update(&b, NULL, 1, 14.0), 0);
	sfvr_view_reset(NULL);
}

static void test_custom_config(void)
{
	sfvr_view_button b = { 0 };
	sfvr_view_config c;

	c.recentre_hold_s = 0.5f;
	c.height_hold_s = 2.0f;
	sfvr_view_update(&b, &c, 0, 0.0);
	sfvr_view_update(&b, &c, 1, 1.0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 1.5), SFVR_VIEW_RECENTRE);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 2.9), 0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 3.0), SFVR_VIEW_RECALIBRATE_HEIGHT);
	sfvr_view_update(&b, &c, 0, 3.1);

	/* a height hold below the recentre hold is raised to it */
	c.recentre_hold_s = 1.0f;
	c.height_hold_s = 0.2f;
	sfvr_view_update(&b, &c, 1, 10.0);
	CHECK_INT(sfvr_view_update(&b, &c, 1, 11.0), SFVR_VIEW_RECENTRE | SFVR_VIEW_RECALIBRATE_HEIGHT);
}

int main(void)
{
	RUN(test_defaults);
	RUN(test_short_press);
	RUN(test_hold_events);
	RUN(test_release_after_recentre_only);
	RUN(test_irregular_updates);
	RUN(test_first_call_already_pressed);
	RUN(test_reset);
	RUN(test_custom_config);
	return SFVR_TEST_RESULT("test_view");
}
