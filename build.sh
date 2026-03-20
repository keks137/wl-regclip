#!/bin/sh
set -xe
cc -Iproto -Wall -Wextra src/*.c proto/*.c -o wl-regclip -lwayland-client  -g
