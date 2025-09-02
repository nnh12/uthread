# Uthread

This is my own threading library that I implemeneted from scratch. Just like Java threads or Linux Pthreads,
my library supports API's to create, join, detach, and yield threads concurrently. This also includes a thread scheduler that manages the context switching between the calling thread + the target threads.

The heart of all uthread APIs lies in `uthread.c`, which also includes the scheduler.
`spin.c`, `test_uthread.c`, and `tiny.c` are separate scripts to test my uthread library.

To compile run: `make`

To run test cases: `./test_uthread`

## TODO
- Add preemption using SIGPROF timer
- Complete test cases for `spin.c` and `tiny.c`
- Complete wrappers in `unix.c` to handle blocking operations for wrappers in Unix I/O functions
