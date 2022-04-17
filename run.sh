#!/bin/bash

set -e  # exit on error
set -x  # trace (print commands)

(
  gcc main.c -o main
  gcc child.c -o child

  rm file.map
  printf 'result.txt\n1 2 3\n4 5 6\n' | ./main file.map
  ls -l file.map

  od --address-radix=d -f file.map
  cat result.txt
) |& tee run.log
