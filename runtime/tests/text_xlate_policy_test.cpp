#include "text_xlate_policy.h"

#include <cstdlib>

static void require(bool condition)
{
    if (!condition) std::abort();
}

int main()
{
    require(text_xlate_capture_requested(nullptr) == 0);
    require(text_xlate_capture_requested("") == 0);
    require(text_xlate_capture_requested("0") == 0);
    require(text_xlate_capture_requested("true") == 0);
    require(text_xlate_capture_requested("1") == 1);
    require(text_xlate_capture_requested("1-extra") == 1);
    return 0;
}
