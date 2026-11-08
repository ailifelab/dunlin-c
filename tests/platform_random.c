#include "../dunlin-compact/platform/random.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    unsigned char first[32] = {0}, second[32] = {0}, zero[32] = {0};
    assert(!ah_os_random(NULL, sizeof first));
    assert(ah_os_random(first, sizeof first));
    assert(ah_os_random(second, sizeof second));
    assert(memcmp(first, zero, sizeof first));
    assert(memcmp(second, zero, sizeof second));
    assert(memcmp(first, second, sizeof first));
    puts("platform random tests passed");
    return 0;
}
