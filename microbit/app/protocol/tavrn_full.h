#ifndef TAVRN_FULL_H
#define TAVRN_FULL_H

#include "tavrn_gtt.h"
#include "tavrn_router.h"

typedef struct tavrn_full {
    tavrn_gtt_t *gtt;
} tavrn_full_t;

typedef enum tavrn_full_init_status {
    TAVRN_FULL_INIT_OK = 0,
    TAVRN_FULL_INIT_INVALID_ARGUMENT,
} tavrn_full_init_status_t;

tavrn_full_init_status_t tavrn_full_init(tavrn_full_t *full, tavrn_gtt_t *gtt);
tavrn_router_augmentation_hooks_t tavrn_full_router_hooks(tavrn_full_t *full);

#endif /* TAVRN_FULL_H */
