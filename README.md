##### Copyright 2023, Robert-Ioan Constantinescu

# Marching Squares Implementation using `<pthread.h>`

## Solving some obvious synchronization issues

### The early `pthread_create` calls and their consequences

As I have to spawn all the threads only once, I have decided to do so as
early as possible in order to maximize the amount of parallel operations, even
though it doesn't necessarily bring about a big performance increase. This does
generate some issues though, as some tasks like the initial memory allocations
needs to happen only ONCE. In order to achieve this, I used a pattern involving
a mutex and a check.

Using this approach would still result in all the threads allocating the map:
```c
pthread_mutex_lock(&shared->locks[LOCK_CMAP_ALLOC]);
shared->cmap = malloc(CONTOUR_CONFIG_COUNT * sizeof(ppm_image *));
pthread_mutex_unlock(&shared->locks[LOCK_CMAP_ALLOC]);
```

The solution is adding an `if` which checks if the task is already done:
```c
pthread_mutex_lock(&shared->locks[LOCK_CMAP_ALLOC]);
if (!shared->cmap) {
    shared->cmap = malloc(CONTOUR_CONFIG_COUNT * sizeof(ppm_image *));
}
pthread_mutex_unlock(&shared->locks[LOCK_CMAP_ALLOC]);
```

Now, when the first thread unlocks the mutex and the second one is about to
execute the code, it will not allocate the memory again, as the pointer to the
cmap is no longer `NULL`.

### Passing the arguments to each thread

In order to pass data to each thread at creation, it needs to be wrapped in
a struct containing the tid (specific to each thread), and a pointer to the
shared data (identical value between all threads). Each thread gets its own
such struct at creation, as passing the same struct would result in bad tid
values.

Doing this will result in a race condition:
```c
thread_data args = {
    .tid    = 0,
    .shared = shared
};

for (long i = 0; i < shared->nthreads; ++i) {
    args.tid = i;

    pthread_create(&threads[i], NULL, worker, &args);
}
```

The solution is providing a different struct to each thread:
```c
thread_data args[shared->nthreads];

for (long i = 0; i <) {
    args[i] = (thread_data) { .shared = shared, .tid = i };

    pthread_create(&threads[i], NULL, worker, &args[i]);
}
```

## Achieving useful parallelism

My approach to this is quite simple. For example, I want to safely parallelize
this code:
```c
shared->image = realloc_memory(); // this needs to happen only once

// this outer for() loop can be parallelized
for (i = 0; i < shared->image->x; i++) {
    for (j = 0; j < shared->image->y; j++) {

        // realloc_memory() must happen before any of this
        shared->image->data[x][y] = dark_magic();
    }
}
```

The easy solution:
```c
lock() // this ensures that realloc_memory() is done once
if (!memory_already_reallocated) {
    shared->image = realloc_memory()
}
unlock()

// this ensures that realloc_memory() is called before the first dark_magic()
barrier(wait_for_memory_reallocation)

// Each thread now processes its own 'slice' of the image
for (i = start; i < end; i++) {
    for (j = 0; j < shared->image->y; j++) {
        shared->image->data[x][y] = dark_magic()
    }
}
```

## Some other optimizations (not related to parallelism)

1) No more `free()` calls, as they are not needed. This is not a program
running in an execution loop, allocating memory once and then terminating.
The kernel already unmaps all the memory after `exit()` is called, so there
is no need for the program to worry about this.

2) Spamming the `const` keyword as often as possible, as it helps the compiler
to generate more efficient code. The compiler sometimes does this automatically
after optimization passes, but optimization flags are not allowed in this
assignment.

3) Removing some of the branching is also a good thing, as it results in less
jumps and branch misses. Take the following example:

```c
if (curr_col > SIGMA) {
    grid[i][j] = 0;
} else {
    grid[i][j] = 1;
}
```

This could be easily reduced to:
```c
grid[i][j] = curr_col <= SIGMA
```

The branching has now been succesfully removed, and instead of a conditional
jmp, the compiler will generate a conditional move (much faster operation).

## Conclusion

Barriers are cool.

