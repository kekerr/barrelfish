/*
 * Copyright (c) 2014 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetsstrasse 6, CH-8092 Zurich. Attn: Systems Group.
 */
#include <bomp_internal.h>

/* This function should return true for just the first thread */
bool GOMP_single_start(void)
{
    struct bomp_thread_local_data *local = g_bomp_state->backend.get_tls();

    if (local == NULL || local->work->thread_id == 0) {
        return true;
    }
    return false;
}

void *GOMP_single_copy_start (void)
{
    assert(!"NYI");
    return NULL;
}

void GOMP_single_copy_end (void *data)
{
    assert(!"NYI");
}
