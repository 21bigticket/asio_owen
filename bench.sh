#!/bin/bash
set -e

if [ "$1" = "health" ]; then
  DURATION=30s THREADS=30 bash bench/bench_full.sh health
  exit
fi

if [ "$1" = "redis" ]; then
  DURATION=30s THREADS=30 bash bench/bench_full.sh redis
  exit
fi

if [ "$1" = "mysql" ]; then
  DURATION=30s THREADS=30 bash bench/bench_full.sh mysql
  exit
fi

if [ "$1" = "config" ]; then
  DURATION=30s THREADS=30 bash bench/bench_full.sh config
  exit
fi

# 默认，运行所有
if [ -z "$1" ] || [ "$1" = "all" ]; then
  DURATION=30s THREADS=30 bash bench/bench_full.sh health
  echo "sleep 10s"
  sleep 10
  DURATION=30s THREADS=30 bash bench/bench_full.sh redis
  echo "sleep 10s"
  sleep 10
  DURATION=30s THREADS=30 bash bench/bench_full.sh mysql
  echo "sleep 10s"
  sleep 10
  DURATION=30s THREADS=30 bash bench/bench_full.sh config
  exit
fi