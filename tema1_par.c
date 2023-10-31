/* Copyright 2023, Robert-Ioan Constantinescu */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "helpers.h"

#define CONTOUR_CONFIG_COUNT 16
#define STEP                 8
#define SIGMA                200
#define RESCALE              2048

#define CLAMP(v, min, max) if (v < min) { v = min; } else if (v > max) { v = max; }
#define MIN(a, b)          ((a) < (b) ? (a) : (b))

enum {
    LOCK_CMAP_ALLOC,
    LOCK_IMAGE_READ,
    LOCK_GRID_ALLOC,
    LOCK_WRITE,
    NLOCKS
};

enum {
    BARRIER_CMAP_AND_IMAGE_ALLOC,
    BARRIER_CMAP_INIT_AND_GRID_ALLOC,
    BARRIER_SAMPLE_GRID,
    BARRIER_RESCALE_IMAGE,
    BARRIER_MARCH,
    NBARRIERS
};

typedef struct {
    ppm_image        *image;
    ppm_image        *scaled;
    ppm_image       **cmap;
    unsigned char   **grid;

    long              nthreads;
    pthread_mutex_t   locks[NLOCKS];
    pthread_barrier_t barriers[NBARRIERS];

    char filename_in[FILENAME_MAX_SIZE];
    char filename_out[FILENAME_MAX_SIZE];

    long              finished;
} thread_data_shared;

typedef struct {
    thread_data_shared *shared;
    long                tid;
} thread_data;

typedef struct {
    long start;
    long end;
} thread_slice;

static inline thread_slice thread_get_slice(const long tid,
                                     const long nthreads,
                                     const long range) {
    const double ratio = (double) range / (double) nthreads;
    return (thread_slice) {
        .start = tid * ratio,
        .end   = MIN((tid + 1) * ratio, range)
    };
}

void init_cmap(ppm_image **const cmap,
               const long tid,
               const long nthreads) {
    const thread_slice slice = thread_get_slice(tid, nthreads, CONTOUR_CONFIG_COUNT);

    for (long i = slice.start; i < slice.end; ++i) {
        char filename[FILENAME_MAX_SIZE];
        sprintf(filename, "./contours/%ld.ppm", i);
        cmap[i] = read_ppm(filename);
    }
}

void rescale_image(ppm_image *const image,
                   ppm_image *const scaled,
                   const long tid,
                   const long nthreads) {
    if (image->x <= RESCALE_X && image->y <= RESCALE_Y) {
        return;
    }

    uint8_t sample[3];

    scaled->x = RESCALE_X;
    scaled->y = RESCALE_Y;

    const thread_slice slice = thread_get_slice(tid, nthreads, RESCALE_X * RESCALE_Y);

    for (long i = slice.start; i < slice.end; ++i) {
        sample_bicubic(image,
                      (float)(i / RESCALE_Y) / (RESCALE_X - 1),
                      (float)(i % RESCALE_Y) / (RESCALE_Y - 1),
                      sample);

        scaled->data[i] = *((ppm_pixel *) sample);
    }
}

void sample_grid(unsigned char  **const grid,
                 const ppm_image *const image,
                 const long tid,
                 const long nthreads) {
    const long   p           = image->x / STEP;
    const long   q           = image->y / STEP;
    const thread_slice slice = thread_get_slice(tid, nthreads, p);

    ppm_pixel     curr_pix;
    unsigned char curr_col;

    for (long i = slice.start; i < slice.end; ++i) {
        grid[i] = malloc((q + 1) * sizeof(unsigned char));

        curr_pix = image->data[i * STEP * image->y + image->x - 1];
        curr_col = (curr_pix.red + curr_pix.green + curr_pix.blue) / 3;
        grid[i][q] = curr_col <= SIGMA;

        for (long j = 0; j < q; ++j) {
            curr_pix = image->data[i * STEP * image->y + j * STEP];
            curr_col = (curr_pix.red + curr_pix.green + curr_pix.blue) / 3;
            grid[i][j] = curr_col <= SIGMA;
        }
    }

    // Task reserved for the thread having the last slice of the range
    if (tid == nthreads - 1) {
        grid[p] = malloc((q + 1) * sizeof(unsigned char));

        for (long j = 0; j < q; ++j) {
            curr_pix = image->data[(image->x - 1) * image->y + j * STEP];
            curr_col = (curr_pix.red + curr_pix.green + curr_pix.blue) / 3;
            grid[p][j] = curr_col <= SIGMA;
        }
    }
}

static inline void march_update(ppm_image *const image,
                         const ppm_image *const c,
                         const long x,
                         const long y) {
    int idx_i, idx_c;

    for (int i = 0; i < c->x; ++i) {
        for (int j = 0; j < c->y; ++j) {
            idx_c = c->x * i + j;
            idx_i = (x + i) * image->y + y + j;

            image->data[idx_i].red   = c->data[idx_c].red;
            image->data[idx_i].green = c->data[idx_c].green;
            image->data[idx_i].blue  = c->data[idx_c].blue;
        }
    }
}

void march(ppm_image     *const image,
           unsigned char *const *const grid,
           ppm_image     *const *const cmap,
           const long     tid,
           const long     nthreads) {
    const long p = image->x / STEP;
    const long q = image->y / STEP;

    const thread_slice slice = thread_get_slice(tid, nthreads, p);

    unsigned char k;

    for (long i = slice.start; i < slice.end; ++i) {
        for (long j = 0; j < q; ++j) {
            k = 8 * grid[i][j]
              + 4 * grid[i][j + 1]
              + 2 * grid[i + 1][j + 1]
              +     grid[i + 1][j];
            march_update(image, cmap[k], i * STEP, j * STEP);
        }
    }
}

void *worker(void *args) {
    thread_data_shared *const shared = ((thread_data *) args)->shared;
    const long                tid    = ((thread_data *) args)->tid;

    pthread_mutex_lock(&shared->locks[LOCK_IMAGE_READ]);
    if (!shared->image) {
        shared->image = read_ppm(shared->filename_in);
        if (shared->image->x <= RESCALE_X && shared->image->y <= RESCALE_Y) {
            shared->scaled = shared->image;
        } else {
            shared->scaled       = malloc(sizeof(ppm_image));
            shared->scaled->data = malloc(RESCALE_X * RESCALE_Y * sizeof(ppm_pixel));
        }
    }
    pthread_mutex_unlock(&shared->locks[LOCK_IMAGE_READ]);
    pthread_mutex_lock(&shared->locks[LOCK_CMAP_ALLOC]);
    if (!shared->cmap) {
        shared->cmap = malloc(CONTOUR_CONFIG_COUNT * sizeof(ppm_image *));
    }
    pthread_mutex_unlock(&shared->locks[LOCK_CMAP_ALLOC]);
    pthread_barrier_wait(&shared->barriers[BARRIER_CMAP_AND_IMAGE_ALLOC]);
    
    rescale_image(shared->image, shared->scaled, tid, shared->nthreads);
    pthread_barrier_wait(&shared->barriers[BARRIER_RESCALE_IMAGE]);

    pthread_mutex_lock(&shared->locks[LOCK_GRID_ALLOC]);
    if (!shared->grid) {
        shared->grid = malloc((shared->scaled->x / STEP + 1) * sizeof(unsigned char *));
    }
    pthread_mutex_unlock(&shared->locks[LOCK_GRID_ALLOC]);
    init_cmap(shared->cmap, tid, shared->nthreads);
    pthread_barrier_wait(&shared->barriers[BARRIER_CMAP_INIT_AND_GRID_ALLOC]);

    sample_grid(shared->grid, shared->scaled, tid, shared->nthreads);
    pthread_barrier_wait(&shared->barriers[BARRIER_SAMPLE_GRID]);

    march(shared->scaled, shared->grid, shared->cmap, tid, shared->nthreads);
    pthread_barrier_wait(&shared->barriers[BARRIER_MARCH]);

    pthread_mutex_lock(&shared->locks[LOCK_WRITE]);
    if (!shared->finished) {
        shared->finished = 1;
        write_ppm(shared->scaled, shared->filename_out);
    }
    pthread_mutex_unlock(&shared->locks[LOCK_WRITE]);

    return NULL;
}

int main(int argc, char *argv[]) {
    (void) argc;

    thread_data_shared *shared = calloc(1, sizeof(*shared));

    strcpy(shared->filename_in,  argv[1]);
    strcpy(shared->filename_out, argv[2]);
    shared->nthreads = atol(argv[3]);

    pthread_t threads[shared->nthreads];

    for (long i = 0; i < NLOCKS; ++i) {
        pthread_mutex_init(&shared->locks[i], NULL);
    }
    for (long i = 0; i < NBARRIERS; ++i) {
        pthread_barrier_init(&shared->barriers[i],
                            NULL,
                            shared->nthreads);
    }

    thread_data args[shared->nthreads];
    int         rc;

    for (long i = 0; i < shared->nthreads; ++i) {
        args[i] = (thread_data) { .shared = shared, .tid = i };

        if ((rc = pthread_create(&threads[i], NULL, worker, &args[i]))) {
            return rc;
        }
    }
    for (long i = 0; i < shared->nthreads; ++i) {
        if ((rc = pthread_join(threads[i], NULL))) {
            return rc;
        }
    }

    for (long i = 0; i < NLOCKS; ++i) {
        pthread_mutex_destroy(&shared->locks[i]);
    }
    for (long i = 0; i < NBARRIERS; ++i) {
        pthread_barrier_destroy(&shared->barriers[i]);
    }

    return 0;
}
