#include "voice/wakeword_labels.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(wakeword_labels_valid(2, "zh2-v1:debug"));
    assert(wakeword_labels_valid(2, "zh2-mf1:debug"));
    assert(wakeword_uses_microfrontend("zh2-mf1:debug"));
    assert(!wakeword_uses_microfrontend("zh2-v1:debug"));
    assert(!wakeword_labels_valid(2, "legacy"));
    assert(!wakeword_labels_valid(4, "zh2-v1:debug"));
    assert(wakeword_labels_valid(4, "legacy"));
    assert(!wakeword_labels_valid(3, "legacy"));
    int8_t two[] = {-128,127};
    assert(wakeword_positive_index(two,2)==1);
    two[0]=127; assert(wakeword_positive_index(two,2)==-1);
    two[1]=-128; assert(wakeword_positive_index(two,2)==-1);
    int8_t four[]={100,20,-50,-100};
    assert(wakeword_positive_index(four,4)==0);
    four[1]=110; assert(wakeword_positive_index(four,4)==1);
    four[2]=110; assert(wakeword_positive_index(four,4)==-1);
    assert(wakeword_positive_index(NULL,2)==-1);
    puts("wakeword_labels_host_test: PASS");
    return 0;
}
