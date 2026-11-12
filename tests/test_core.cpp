// Standalone unit tests for the pure geometry / display matching core.
// Build & run:  c++ -std=c++17 -I src tests/test_core.cpp -o /tmp/om_tests && /tmp/om_tests

#include "core/geometry.hpp"
#include "core/display-match.hpp"

#include <cstdio>
#include <cstdlib>

using namespace om;

static int failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                     \
		if (!(cond)) {                                                   \
			std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                              \
		}                                                                \
	} while (0)
#define CHECK_RECT(r, X, Y, W, H) CHECK((r) == (IRect{X, Y, W, H}))

static void test_two_display_example()
{
	/* 3840x1080 program, draw over each half, two 1920x1080 displays. */
	IRect left = rectFromPoints(0, 0, 1920, 1080);
	IRect right = rectFromPoints(3840, 1080, 1920, 0); /* dragged backwards */
	CHECK_RECT(left, 0, 0, 1920, 1080);
	CHECK_RECT(right, 1920, 0, 1920, 1080);
	CHECK_RECT(defaultDestination(left, 1920, 1080), 0, 0, 1920, 1080);
	CHECK_RECT(defaultDestination(right, 1920, 1080), 0, 0, 1920, 1080);

	/* 1:1 mapping clips nothing and keeps scale exactly 1. */
	ClippedSlice c;
	CHECK(clipSliceToCanvas(right, {0, 0, 1920, 1080}, 3840, 1080, c));
	CHECK_RECT(c.src, 1920, 0, 1920, 1080);
	CHECK(c.dx == 0 && c.dy == 0 && c.dw == 1920 && c.dh == 1080);
}

static void test_defaults_and_fit()
{
	CHECK_RECT(defaultDestination({0, 0, 3840, 1080}, 1920, 1080), 0, 0, 1920, 540);
	CHECK_RECT(defaultDestination({10, 10, 400, 300}, 1920, 1080), 0, 0, 400, 300);
	CHECK_RECT(fitToDisplay({0, 0, 1920, 1080}, 1920, 1080), 0, 0, 1920, 1080);
	CHECK_RECT(fitToDisplay({0, 0, 1000, 1000}, 1920, 1080), 420, 0, 1080, 1080);
	CHECK_RECT(fitToDisplay({0, 0, 3840, 1080}, 1920, 1080), 0, 270, 1920, 540);
}

static void test_clip()
{
	ClippedSlice c;
	/* Source hangs 100px off the right edge of a 1920 canvas: the clipped
	 * part is dropped and the destination shrinks to match (scale 2). */
	CHECK(clipSliceToCanvas({1820, 0, 200, 100}, {0, 0, 400, 200}, 1920, 1080, c));
	CHECK_RECT(c.src, 1820, 0, 100, 100);
	CHECK(c.dx == 0 && c.dw == 200 && c.dh == 200);
	/* Entirely outside. */
	CHECK(!clipSliceToCanvas({5000, 0, 10, 10}, {0, 0, 10, 10}, 1920, 1080, c));
	/* Negative origin. */
	CHECK(clipSliceToCanvas({-50, -50, 100, 100}, {100, 100, 100, 100}, 1920, 1080, c));
	CHECK_RECT(c.src, 0, 0, 50, 50);
	CHECK(c.dx == 150 && c.dy == 150 && c.dw == 50 && c.dh == 50);
}

static void test_hit_and_handles()
{
	IRect r{100, 100, 200, 100};
	CHECK(hitTest(r, 100, 100, 5) == Handle::TopLeft);
	CHECK(hitTest(r, 300, 200, 5) == Handle::BottomRight);
	CHECK(hitTest(r, 200, 100, 5) == Handle::Top);
	CHECK(hitTest(r, 150, 150, 5) == Handle::Move);
	CHECK(hitTest(r, 301, 130, 5) == Handle::Right);
	CHECK(hitTest(r, 50, 50, 5) == Handle::None);
}

static void test_move_and_snap()
{
	IRect r{100, 100, 200, 100};
	DragResult m = applyMove(r, 15.4, -20.6, nullptr, 0);
	CHECK_RECT(m.rect, 115, 79, 200, 100);

	SnapTargets t;
	t.xs = {0, 1920};
	t.ys = {0, 1080};
	/* Left edge within 6px of 0 snaps to 0. */
	m = applyMove(r, -96, 0, &t, 8);
	CHECK(m.snappedX && m.rect.x == 0 && m.guideX == 0);
	/* Right edge near 1920 snaps. */
	m = applyMove({1700, 0, 200, 100}, 15, 0, &t, 8);
	CHECK(m.snappedX && m.rect.right() == 1920);
	/* Far away: no snap. */
	m = applyMove(r, 500, 0, &t, 8);
	CHECK(!m.snappedX && m.rect.x == 600);
}

static void test_resize()
{
	IRect r{100, 100, 200, 100};
	DragResult d = applyResize(r, Handle::BottomRight, 50, 20, false, nullptr, 0);
	CHECK_RECT(d.rect, 100, 100, 250, 120);
	d = applyResize(r, Handle::TopLeft, -10, -10, false, nullptr, 0);
	CHECK_RECT(d.rect, 90, 90, 210, 110);
	/* Never flips past min size. */
	d = applyResize(r, Handle::Right, -500, 0, false, nullptr, 0, 4);
	CHECK_RECT(d.rect, 100, 100, 4, 100);
	/* Aspect lock 2:1 from right edge. */
	d = applyResize(r, Handle::Right, 100, 0, true, nullptr, 0);
	CHECK(d.rect.w == 300 && d.rect.h == 150 && d.rect.x == 100);
	/* Aspect lock from corner. */
	d = applyResize(r, Handle::BottomRight, 200, 0, true, nullptr, 0);
	CHECK_RECT(d.rect, 100, 100, 400, 200);
	/* Snap a resized edge. */
	SnapTargets t;
	t.xs = {1920};
	d = applyResize({1700, 0, 200, 100}, Handle::Right, 17, 0, false, &t, 8);
	CHECK(d.snappedX && d.rect.right() == 1920);
	/* Numeric size edit with lock. */
	CHECK_RECT(applySizeEdit({0, 0, 1920, 1080}, 960, 1080, true, true), 0, 0, 960, 540);
	CHECK_RECT(applySizeEdit({0, 0, 1920, 1080}, 1920, 540, false, true), 0, 0, 960, 540);
	CHECK_RECT(applySizeEdit({0, 0, 1920, 1080}, 960, 1080, true, false), 0, 0, 960, 1080);
}

static void test_matching()
{
	DisplayIdentity a{"UUID-A", 0x10ac, 0xa0, 1111, "DELL"};
	DisplayIdentity b{"UUID-B", 0x10ac, 0xa0, 2222, "DELL"};
	DisplayIdentity tv{"UUID-TV", 0x4c2d, 0x10, 0, "Samsung TV"};
	DisplayIdentity tv2{"UUID-TV2", 0x4c2d, 0x10, 0, "Samsung TV"};
	DisplayIdentity builtin{"UUID-BI", 0x610, 0xa, 0, "Built-in"};

	/* exact */
	MatchResult m = matchDisplay(a, {builtin, b, a});
	CHECK(m.status == MatchStatus::Connected && m.index == 2);

	/* UUID changed (different port) but serial proves same unit */
	DisplayIdentity aMoved = a;
	aMoved.uuid = "UUID-A2";
	m = matchDisplay(a, {builtin, aMoved, b});
	CHECK(m.status == MatchStatus::ConnectedSerial && m.index == 1);

	/* Twin of the same model with different serial is NOT the display */
	m = matchDisplay(a, {builtin, b});
	CHECK(m.status == MatchStatus::Disconnected);

	/* TV without serial, UUID gone, one same-model TV present: ambiguous */
	DisplayIdentity tvMoved = tv;
	tvMoved.uuid = "UUID-TV-NEW";
	m = matchDisplay(tv, {builtin, tvMoved});
	CHECK(m.status == MatchStatus::Ambiguous && m.candidates.size() == 1);

	/* Exact UUID still wins even with identical twins present */
	m = matchDisplay(tv, {tv2, tv});
	CHECK(m.status == MatchStatus::Connected && m.index == 1);

	/* Nothing plausible */
	m = matchDisplay(tv, {builtin});
	CHECK(m.status == MatchStatus::Disconnected && m.index == -1);

	/* Duplicate UUIDs reported by OS: refuse */
	m = matchDisplay(a, {a, a});
	CHECK(m.status == MatchStatus::Ambiguous);

	/* No hardware identity (virtual/indirect display): name match is only a
	 * candidate, never "connected" -- even if it is the only display. */
	DisplayIdentity virt{"", 0, 0, 0, "Virtual Display"};
	m = matchDisplay(virt, {builtin, DisplayIdentity{"", 0, 0, 0, "Virtual Display"}});
	CHECK(m.status == MatchStatus::Ambiguous && m.index == -1 && m.candidates.size() == 1);
	m = matchDisplay(virt, {builtin});
	CHECK(m.status == MatchStatus::Disconnected);

	/* Unassigned */
	m = matchDisplay(DisplayIdentity{}, {a});
	CHECK(m.status == MatchStatus::Unassigned);
}

int main()
{
	test_two_display_example();
	test_defaults_and_fit();
	test_clip();
	test_hit_and_handles();
	test_move_and_snap();
	test_resize();
	test_matching();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("All core tests passed\n");
	return 0;
}
