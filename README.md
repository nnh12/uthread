# Implementation of a user-level multi-thread library (similliar to Pthreads)

The most common multithreading library in Linux is pthreads, which is implemented by kernel-level threads.
This is a my own custom implementation of pthreads.

The heart of all pthread APIs are in uthread.c
To compile run: make
