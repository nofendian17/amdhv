/*
 * hv_kext_abi_test.cpp - host-side check of the hv_callbacks_t layout the
 * kext registers with xnu.
 *
 * Sources (both declare the same eight fields in the same order):
 *   https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/hv_support_kext.h
 *   https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/kern/hv_support_kext.h
 * The newer tag changes only hv_io_notifier.c (IKOT_UNKNOWN -> IOT_ANY), which
 * this kext does not call; the provider ABI is unchanged.
 */
#include <stddef.h>
#include <stdio.h>

#include "../kext/Source/core/HvKextAbi.hpp"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

constexpr size_t kPtr = sizeof(void (*)(void *));

void testCallbackOrderMatchesXnu()
{
    CHECK(__builtin_offsetof(hv_callbacks_t, dispatch) == 0 * kPtr);
    CHECK(__builtin_offsetof(hv_callbacks_t, preempt) == 1 * kPtr);
    CHECK(__builtin_offsetof(hv_callbacks_t, suspend) == 2 * kPtr);
    CHECK(__builtin_offsetof(hv_callbacks_t, thread_destroy) == 3 * kPtr);
    CHECK(__builtin_offsetof(hv_callbacks_t, task_destroy) == 4 * kPtr);
    CHECK(__builtin_offsetof(hv_callbacks_t, volatile_state) == 5 * kPtr);
    CHECK(__builtin_offsetof(hv_callbacks_t, resume) == 6 * kPtr);
    CHECK(__builtin_offsetof(hv_callbacks_t, memory_pressure) == 7 * kPtr);
    CHECK(sizeof(hv_callbacks_t) == 8 * kPtr);
}

} // namespace

int main()
{
    testCallbackOrderMatchesXnu();
    if (fails) {
        printf("hv_kext_abi_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("hv_kext_abi_test: OK\n");
    return 0;
}
