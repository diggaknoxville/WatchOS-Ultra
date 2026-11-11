#include <unity.h>
#include "GeoMath.h"

void setUp() {}
void tearDown() {}

static void test_haversine() {
    // one degree of latitude is about 111.19 km
    TEST_ASSERT_DOUBLE_WITHIN(50.0, 111195.0, geo::haversineM(48.0, 9.0, 49.0, 9.0));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 0.0, geo::haversineM(48.7, 9.1, 48.7, 9.1));
}

static void test_advance_matches_haversine() {
    double lat = 48.7758, lon = 9.1829;
    geo::advance(lat, lon, 90.0, 200.0);                       // east
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 200.0, geo::haversineM(48.7758, 9.1829, lat, lon));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 48.7758, lat);
    TEST_ASSERT_TRUE(lon > 9.1829);
    geo::advance(lat, lon, 180.0, 100.0);                      // south
    TEST_ASSERT_TRUE(lat < 48.7758);
}

static void test_smooth_angle_wraps() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 355.0f, geo::smoothAngle(350.0f, 10.0f, 0.25f));   // 350 -> 10 goes through 0
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, geo::smoothAngle(10.0f, 350.0f, 0.25f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, geo::smoothAngle(90.0f, 90.0f, 0.5f));
}

static void test_filter_standing_jitter_shrinks() {
    geo::PosFilter f;
    f.update(48.7758, 9.1829, 1.0f, 0.0f, 0);
    double lat = 48.7758, lon = 9.1829;
    geo::advance(lat, lon, 45.0, 6.0);                         // 6 m of noise
    TEST_ASSERT_TRUE(f.update(lat, lon, 1.0f, 0.0f, 1000));
    TEST_ASSERT_TRUE(geo::haversineM(48.7758, 9.1829, f.lat, f.lon) < 1.5);
}

static void test_filter_rejects_jump_then_follows() {
    geo::PosFilter f;
    f.update(48.7758, 9.1829, 1.0f, 4.0f, 0);
    double lat = 48.7758, lon = 9.1829;
    geo::advance(lat, lon, 0.0, 400.0);                        // 400 m in one second at walking pace
    TEST_ASSERT_FALSE(f.update(lat, lon, 1.0f, 4.0f, 1000));
    TEST_ASSERT_FALSE(f.update(lat, lon, 1.0f, 4.0f, 2000));
    TEST_ASSERT_FALSE(f.update(lat, lon, 1.0f, 4.0f, 3000));
    TEST_ASSERT_TRUE(f.update(lat, lon, 1.0f, 4.0f, 4000));    // persistent: accepted
    TEST_ASSERT_TRUE(geo::haversineM(lat, lon, f.lat, f.lon) < 1.0);
}

static void test_filter_follows_walking() {
    geo::PosFilter f;
    double lat = 48.7758, lon = 9.1829;
    f.update(lat, lon, 1.0f, 5.0f, 0);
    for (int i = 1; i <= 20; i++) {
        geo::advance(lat, lon, 90.0, 1.4);                     // 1.4 m/s
        TEST_ASSERT_TRUE(f.update(lat, lon, 1.0f, 5.0f, i * 1000));
    }
    TEST_ASSERT_TRUE(geo::haversineM(lat, lon, f.lat, f.lon) < 2.0);   // lag below two metres
}

static void test_climb_ignores_noise() {
    geo::ClimbTracker c;
    const float noisy[] = {300, 302, 299, 303, 301, 298, 302};
    for (float a : noisy) c.update(a);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, c.ascent);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, c.descent);
    const float climb[] = {306, 312, 318, 310, 300};
    for (float a : climb) c.update(a);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f, c.ascent);          // 300 -> 318
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.0f, c.descent);         // 318 -> 300
}

static void test_stride() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.72f, geo::strideFromSegment(72.0f, 100, false));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, geo::strideFromSegment(20.0f, 100, false));    // 0.2 m: GPS stood still
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, geo::strideFromSegment(200.0f, 100, false));   // 2 m: tram, not walking
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.3f, geo::strideFromSegment(130.0f, 100, true));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, geo::strideFromSegment(30.0f, 40, false));     // too few steps
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_haversine);
    RUN_TEST(test_advance_matches_haversine);
    RUN_TEST(test_smooth_angle_wraps);
    RUN_TEST(test_filter_standing_jitter_shrinks);
    RUN_TEST(test_filter_rejects_jump_then_follows);
    RUN_TEST(test_filter_follows_walking);
    RUN_TEST(test_climb_ignores_noise);
    RUN_TEST(test_stride);
    return UNITY_END();
}
