cmake_minimum_required(VERSION 3.28)
project(Ex3 C)

set(CMAKE_C_STANDARD 11)

add_executable(Ex3 main.c
        threadpool.c
        server.c
        threadpool.h)
