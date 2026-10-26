/* Host tests for the trace ring: pio test -e native */
#include <unity.h>

#include "trace.h"

#define CAP 4

static trace_ring_t r;
static uint32_t buf[CAP];

void setUp(void)
{
    trace_init(&r, buf, sizeof(buf[0]), CAP);
}

void tearDown(void) {}

static void push_n(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = r.next * 10u; /* record content follows its number */
        trace_push(&r, &v);
    }
}

static void test_empty(void)
{
    uint32_t out[CAP], first = 99;
    TEST_ASSERT_EQUAL_size_t(0, trace_read(&r, 0, out, CAP, &first));
    TEST_ASSERT_EQUAL_UINT32(0, first);
}

static void test_in_order(void)
{
    uint32_t out[CAP], first;
    push_n(3);
    TEST_ASSERT_EQUAL_size_t(3, trace_read(&r, 0, out, CAP, &first));
    TEST_ASSERT_EQUAL_UINT32(0, first);
    TEST_ASSERT_EQUAL_UINT32(0, out[0]);
    TEST_ASSERT_EQUAL_UINT32(20, out[2]);
    TEST_ASSERT_EQUAL_size_t(1, trace_read(&r, 2, out, CAP, &first));
    TEST_ASSERT_EQUAL_UINT32(2, first);
    TEST_ASSERT_EQUAL_UINT32(20, out[0]);
    TEST_ASSERT_EQUAL_size_t(0, trace_read(&r, 3, out, CAP, &first)); /* nothing new */
    TEST_ASSERT_EQUAL_UINT32(3, first);
}

static void test_overwritten(void)
{
    uint32_t out[CAP], first;
    push_n(10);
    TEST_ASSERT_EQUAL_size_t(4, trace_read(&r, 0, out, CAP, &first));
    TEST_ASSERT_EQUAL_UINT32(6, first); /* 0..5 are gone */
    TEST_ASSERT_EQUAL_UINT32(60, out[0]);
    TEST_ASSERT_EQUAL_UINT32(90, out[3]);
    TEST_ASSERT_EQUAL_size_t(3, trace_read(&r, 7, out, CAP, &first));
    TEST_ASSERT_EQUAL_UINT32(7, first);
    TEST_ASSERT_EQUAL_UINT32(70, out[0]);
}

static void test_since_ahead(void)
{
    uint32_t out[CAP], first;
    push_n(10);
    /* a client of an earlier run asks for more than there is: it gets the oldest kept */
    TEST_ASSERT_EQUAL_size_t(4, trace_read(&r, 50, out, CAP, &first));
    TEST_ASSERT_EQUAL_UINT32(6, first);
}

static void test_max(void)
{
    uint32_t out[CAP], first;
    push_n(10);
    TEST_ASSERT_EQUAL_size_t(2, trace_read(&r, 6, out, 2, &first));
    TEST_ASSERT_EQUAL_UINT32(6, first);
    TEST_ASSERT_EQUAL_UINT32(70, out[1]);
}

static void test_no_buffer(void)
{
    uint32_t out[CAP], first;
    trace_init(&r, NULL, sizeof(buf[0]), 0);
    push_n(3);
    TEST_ASSERT_EQUAL_size_t(0, trace_read(&r, 0, out, CAP, &first));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_buffer);
    RUN_TEST(test_empty);
    RUN_TEST(test_in_order);
    RUN_TEST(test_overwritten);
    RUN_TEST(test_since_ahead);
    RUN_TEST(test_max);
    return UNITY_END();
}
