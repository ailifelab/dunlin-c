#include "../dunlin-compact/platform/clock.h"
#include <dunlin/compact/session.h>
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint64_t before = ah_os_now_ms(NULL);
    uint64_t compatible = ah_linux_now_ms(NULL);
    uint64_t after = ah_os_now_ms(NULL);
    assert(before <= compatible && compatible <= after);
    puts("platform clock tests passed");
    return 0;
}
