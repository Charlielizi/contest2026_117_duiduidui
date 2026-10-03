#include <assert.h>
#include <string.h>
static int default_calls, list_calls;
static const char *last_list;
int ntpc_start(void) { default_calls++; return 7; }
int ntpc_start_with_list(const char *servers) { list_calls++; last_list = servers; return 8; }
#define main ntpcstart_main
#include "../../system/ntpc/ntpcstart_main.c"
#undef main
int main(void)
{
  char *args[] = { "ntpcstart", "--help", NULL };
  assert(ntpcstart_main(2, args) == 0);
  assert(default_calls == 0 && list_calls == 0);
  assert(ntpcstart_main(1, args) == 0 && default_calls == 1);
  args[1] = "one.example;two.example";
  assert(ntpcstart_main(2, args) == 0 && list_calls == 1);
  assert(!strcmp(last_list, args[1]));
  args[1] = "";
  assert(ntpcstart_main(2, args) != 0 && list_calls == 1);
  return 0;
}
