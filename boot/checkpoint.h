/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PROBE_CHECKPOINT_H
#define PROBE_CHECKPOINT_H
#include <stdint.h>
unsigned checkpoint_delay(unsigned current_el, unsigned target_el,
                          unsigned stage, int32_t psci_version, uint64_t freq);
/* Returns only on a failed check or an unexpected SYSTEM_RESET return. */
int checkpoint_run(unsigned current_el, unsigned target_el, unsigned stage);
#endif
