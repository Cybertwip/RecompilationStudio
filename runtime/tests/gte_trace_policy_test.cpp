#include "gte_trace_policy.h"

#include <cstdlib>

static void require(bool condition)
{
    if (!condition) std::abort();
}

int main()
{
    require(gte_trace_requested(nullptr) == 0);
    require(gte_trace_requested("") == 0);
    require(gte_trace_requested("0") == 0);
    require(gte_trace_requested("true") == 0);
    require(gte_trace_requested("1") == 1);
    require(gte_trace_requested("1-extra") == 1);
    return 0;
}
