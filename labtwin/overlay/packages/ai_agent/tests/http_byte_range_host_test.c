#include <assert.h>
#include <stdio.h>
#include "infra/http_byte_range.h"
int main(void)
{
    unsigned long long a, b;
    assert(http_byte_range(NULL, 100, &a, &b) == 0 && a == 0 && b == 99);
    assert(http_byte_range("bytes=20-39", 100, &a, &b) == 1 && a == 20 && b == 39);
    assert(http_byte_range("bytes=20-", 100, &a, &b) == 1 && b == 99);
    assert(http_byte_range("bytes=-20", 100, &a, &b) == 1 && a == 80);
    assert(http_byte_range("bytes=-200", 100, &a, &b) == 1 && a == 0);
    assert(http_byte_range("bytes=20-999", 100, &a, &b) == 1 && b == 99);
    assert(http_byte_range("bytes=100-", 100, &a, &b) == -ERANGE);
    assert(http_byte_range("bytes=30-20", 100, &a, &b) == -ERANGE);
    assert(http_byte_range("bytes=-0", 100, &a, &b) == -ERANGE);
    assert(http_byte_range("bytes=-abc", 100, &a, &b) == -EINVAL);
    assert(http_byte_range("bytes=1-2junk", 100, &a, &b) == -EINVAL);
    assert(http_byte_range("bytes=1-2,3-4", 100, &a, &b) == -EINVAL);
    assert(http_byte_range("bytes=18446744073709551616-", 100, &a, &b) == -EINVAL);
    puts("http_byte_range_host_test: PASS");
}
