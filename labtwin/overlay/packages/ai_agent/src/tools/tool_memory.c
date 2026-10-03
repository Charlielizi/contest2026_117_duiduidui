#include "tools/tool_memory.h"
#include "core/structured_memory.h"

int tool_memory_propose_execute(const char *input, char *output,
                                size_t output_size)
{
    return structured_memory_propose_json(input, output, output_size);
}
