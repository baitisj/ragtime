// A small check library for the Ragtime unit tests, with no dependencies, so
// the tests build anywhere the core does (including the gcc:16 image).
//
//   CHECK(cond)       records a failure and carries on
//   CHECK_EQ(a, b)    the same, printing both sides when they differ
//   return ragtime_test::finish();   at the end of main()
#ifndef RAGTIME_TESTS_CHECK_H
#define RAGTIME_TESTS_CHECK_H

#include <cstdio>
#include <sstream>
#include <string>

namespace ragtime_test
{

inline int failures = 0;
inline int checks = 0;

inline void check(bool condition, const char* what, const char* file, int line)
{
    checks++;
    if (!condition)
    {
        failures++;
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
    }
}

template <typename A, typename B>
void checkEqual(const A& a, const B& b, const char* what, const char* file, int line)
{
    checks++;
    if (!(a == b))
    {
        failures++;
        std::ostringstream left, right;
        left << a;
        right << b;
        std::fprintf(stderr, "FAIL %s:%d: %s (%s vs %s)\n", file, line, what,
                     left.str().c_str(), right.str().c_str());
    }
}

inline int finish()
{
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

} // namespace ragtime_test

#define CHECK(cond) ::ragtime_test::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::ragtime_test::checkEqual((a), (b), #a " == " #b, __FILE__, __LINE__)

#endif
