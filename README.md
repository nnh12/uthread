# Implementation of a User-level Multiple Threaded Library

The most common multithreading library in Linux is pthreads, which is implemented by kernel-level threads.
This is a my own custom implementation of pthreads on user level. I wrote my own thread scheduler to handle 
multiple threading and logic to to manage threads.

The heart of all pthread APIs are in uthread.c

To compile run: make
