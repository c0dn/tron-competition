#ifdef TAVRN_FULL_H
#error "benchmark core must not pre-include FULL_TAVRN"
#endif

#include "routed_benchmark.h"

#ifdef TAVRN_FULL_H
#error "benchmark core must remain AODV_ONLY source-isolated"
#endif

int main(void)
{
    routed_benchmark_state_t state;

    return routed_benchmark_init(&state, 0u, 1u, 1u, 1u) ? 0 : 1;
}
