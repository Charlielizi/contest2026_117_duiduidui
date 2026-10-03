/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* Returns 0 for a reserved start, 1 for help, or a negative error. */
int agent_start_gate_acquire(int argc, char *argv[]);
void agent_start_gate_release(void);
