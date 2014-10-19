#!/bin/sh
set -m
trap "kill %1" 0
trap "kill -CONT %1 ; exit 0" QUIT TERM INT HUP
./lima-memtester "$@" &

while true ; do kill -STOP %1; sleep 1 ; kill -CONT %1 ; sleep 3 ; done
