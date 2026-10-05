#include "sfvr/sfvr_perf.h"
#include "sfvr_test.h"

static const char *const stages2[] = { "logic", "render" };

/* n frames evenly from t0 over `span` seconds, constant per-frame data */
static void feed(sfvr_perf *p, double t0, double span, int n, int missed_frames, float logic, float render, float eye0, float eye1, float gpu)
{
	int i;

	for (i = 0; i < n; i++)
	{
		float stage[2];
		float eye[2];

		stage[0] = logic;
		stage[1] = render;
		eye[0] = eye0;
		eye[1] = eye1;
		sfvr_perf_add_frame(p, t0 + span * i / (n - 1), i < missed_frames, stage, eye, gpu);
	}
}

static void test_golden_line(void)
{
	sfvr_perf p;
	char line[256];

	sfvr_perf_init(&p, 10.0, stages2, 2);
	/* 719 frames over a 10 s window: 71.9 fps */
	feed(&p, 100.0, 9.99, 719, 3, 1.2f, 5.02f, 2.21f, 2.19f, 8.4f);
	CHECK_INT(sfvr_perf_poll(&p, 109.9, line, sizeof(line)), 0); /* window not over yet */
	CHECK_INT(sfvr_perf_poll(&p, 110.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=71.9 missed=3 cpu=6.22ms logic=1.20 render=5.02 eye=2.21/2.19ms gpu=8.40ms");
}

static void test_gpu_unknown(void)
{
	sfvr_perf p;
	char line[256];

	sfvr_perf_init(&p, 10.0, stages2, 2);
	feed(&p, 0.0, 9.0, 900, 0, 1.0f, 2.0f, 1.5f, 1.5f, -1.0f);
	CHECK_INT(sfvr_perf_poll(&p, 10.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=90.0 missed=0 cpu=3.00ms logic=1.00 render=2.00 eye=1.50/1.50ms gpu=n/a");
}

static void test_gpu_partial_mean(void)
{
	sfvr_perf p;
	char line[256];
	float stage[2] = { 1.0f, 1.0f }, eye[2] = { 1.0f, 1.0f };

	/* the GPU mean is over the frames that had a GPU time only */
	sfvr_perf_init(&p, 1.0, stages2, 2);
	sfvr_perf_add_frame(&p, 0.0, 0, stage, eye, 4.0f);
	sfvr_perf_add_frame(&p, 0.3, 0, stage, eye, -1.0f);
	sfvr_perf_add_frame(&p, 0.6, 0, stage, eye, 6.0f);
	sfvr_perf_add_frame(&p, 0.9, 0, stage, eye, -1.0f);
	CHECK_INT(sfvr_perf_poll(&p, 1.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=4.0 missed=0 cpu=2.00ms logic=1.00 render=1.00 eye=1.00/1.00ms gpu=5.00ms");
	/* a zero GPU time is a time, not unknown */
	sfvr_perf_add_frame(&p, 1.5, 0, stage, eye, 0.0f);
	CHECK_INT(sfvr_perf_poll(&p, 2.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=1.0 missed=0 cpu=2.00ms logic=1.00 render=1.00 eye=1.00/1.00ms gpu=0.00ms");
}

static void test_window_resets(void)
{
	sfvr_perf p;
	char line[256];

	sfvr_perf_init(&p, 10.0, stages2, 2);
	feed(&p, 0.0, 10.0, 11, 2, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
	CHECK_INT(sfvr_perf_poll(&p, 10.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=1.1 missed=2 cpu=2.00ms logic=1.00 render=1.00 eye=1.00/1.00ms gpu=1.00ms");
	CHECK_INT(p.frames, 0);
	CHECK_INT(p.missed, 0);
	CHECK_INT(sfvr_perf_poll(&p, 10.5, line, sizeof(line)), 0);
	/* the next window starts where the last ended (10.0) and has its own data */
	feed(&p, 10.5, 9.0, 6, 0, 3.0f, 3.0f, 2.0f, 2.0f, 2.0f);
	CHECK_INT(sfvr_perf_poll(&p, 19.9, line, sizeof(line)), 0);
	CHECK_INT(sfvr_perf_poll(&p, 20.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=0.6 missed=0 cpu=6.00ms logic=3.00 render=3.00 eye=2.00/2.00ms gpu=2.00ms");
}

static void test_poll_starts_window(void)
{
	sfvr_perf p;
	char line[256];

	/* a poll before any frame starts the window; an empty window still reports */
	sfvr_perf_init(&p, 0.0, stages2, 2); /* 0 = default 10 s */
	CHECK_NEAR(p.window_s, 10.0, 1e-9);
	CHECK_INT(sfvr_perf_poll(&p, 50.0, line, sizeof(line)), 0);
	CHECK_INT(sfvr_perf_poll(&p, 59.0, line, sizeof(line)), 0);
	CHECK_INT(sfvr_perf_poll(&p, 60.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=0.0 missed=0 cpu=0.00ms logic=0.00 render=0.00 eye=0.00/0.00ms gpu=n/a");
}

static void test_no_stages_and_nulls(void)
{
	sfvr_perf p;
	char line[256];
	float eye[2] = { 2.0f, 3.0f };

	sfvr_perf_init(&p, 1.0, NULL, 0);
	sfvr_perf_add_frame(&p, 0.0, 1, NULL, eye, -1.0f);
	sfvr_perf_add_frame(&p, 0.5, 0, NULL, NULL, -1.0f);
	CHECK_INT(sfvr_perf_poll(&p, 1.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=2.0 missed=1 cpu=0.00ms eye=1.00/1.50ms gpu=n/a");
}

static void test_stage_names(void)
{
	sfvr_perf p;
	char line[512];
	const char *names[10] = { "a", "b", "c", "d", "e", "f", "g", "h", "i", "j" };
	float stage[10] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
	float eye[2] = { 0, 0 };

	/* more than 8 stages are clamped to 8 */
	sfvr_perf_init(&p, 1.0, names, 10);
	CHECK_INT(p.stage_count, SFVR_PERF_MAX_STAGES);
	sfvr_perf_add_frame(&p, 0.0, 0, stage, eye, -1.0f);
	CHECK_INT(sfvr_perf_poll(&p, 1.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=1.0 missed=0 cpu=8.00ms a=1.00 b=1.00 c=1.00 d=1.00 e=1.00 f=1.00 g=1.00 h=1.00 eye=0.00/0.00ms gpu=n/a");

	/* names are copied and truncated to 15 characters; NULL names are numbered */
	{
		char volatile_name[32] = "averyveryverylongstagename";
		const char *two[2];

		two[0] = volatile_name;
		two[1] = NULL;
		sfvr_perf_init(&p, 1.0, two, 2);
		volatile_name[0] = 'X'; /* the library must not read this later */
	}
	sfvr_perf_add_frame(&p, 0.0, 0, stage, eye, -1.0f);
	CHECK_INT(sfvr_perf_poll(&p, 1.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=1.0 missed=0 cpu=2.00ms averyveryverylo=1.00 stage1=1.00 eye=0.00/0.00ms gpu=n/a");
}

static void test_truncation(void)
{
	sfvr_perf p;
	char line[24];
	char tiny[1];
	float stage[2] = { 1.0f, 1.0f }, eye[2] = { 1.0f, 1.0f };

	sfvr_perf_init(&p, 1.0, stages2, 2);
	sfvr_perf_add_frame(&p, 0.0, 0, stage, eye, 1.0f);
	CHECK_INT(sfvr_perf_poll(&p, 1.0, NULL, 10), 0);
	CHECK_INT(sfvr_perf_poll(&p, 1.0, line, 0), 0);
	CHECK_INT(p.frames, 1); /* nothing was consumed */
	CHECK_INT(sfvr_perf_poll(&p, 1.0, line, sizeof(line)), 1);
	CHECK_STR(line, "[vr-perf] fps=1.0 misse");
	sfvr_perf_add_frame(&p, 2.0, 0, stage, eye, 1.0f);
	CHECK_INT(sfvr_perf_poll(&p, 3.0, tiny, sizeof(tiny)), 1);
	CHECK_STR(tiny, "");
}

static void test_reset(void)
{
	sfvr_perf p;
	char line[256];
	float stage[2] = { 1.0f, 1.0f }, eye[2] = { 1.0f, 1.0f };

	sfvr_perf_init(&p, 1.0, stages2, 2);
	sfvr_perf_add_frame(&p, 0.0, 1, stage, eye, 1.0f);
	sfvr_perf_reset(&p);
	CHECK_INT(p.frames, 0);
	CHECK_INT(sfvr_perf_poll(&p, 5.0, line, sizeof(line)), 0); /* window restarted at the poll */
	sfvr_perf_add_frame(NULL, 0.0, 0, NULL, NULL, 0.0f);
	sfvr_perf_init(NULL, 1.0, NULL, 0);
	sfvr_perf_reset(NULL);
	CHECK_INT(sfvr_perf_poll(NULL, 0.0, line, sizeof(line)), 0);
}

int main(void)
{
	RUN(test_golden_line);
	RUN(test_gpu_unknown);
	RUN(test_gpu_partial_mean);
	RUN(test_window_resets);
	RUN(test_poll_starts_window);
	RUN(test_no_stages_and_nulls);
	RUN(test_stage_names);
	RUN(test_truncation);
	RUN(test_reset);
	return SFVR_TEST_RESULT("test_perf");
}
