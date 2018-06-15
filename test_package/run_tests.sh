#!/usr/bin/env bash

set -eu
echo $(pwd)
SERVER_COUNT=3

SERVER_PIDS=()
echo """
[==========] Running 1 test from 1 test case.
[----------] Global test environment set-up.
[----------] 1 test from raft_test
[ RUN      ] Echo_Server.Members${SERVER_COUNT}"""
for i in $(seq 1 ${SERVER_COUNT})
do
  echo "[          ] Starting server"
  ./bin/raft_server $i &
  SERVER_PIDS+=($!)
done

echo "[          ] Settling members"
sleep 5

echo "[          ] Cleaning client"
./bin/raft_client -c
echo "[          ] Writing Message"
./bin/raft_client -m 'test::message'
echo "[          ] Counting Server Processes"
alive_servers="$(ps -ef | grep -E '\./bin/raft_server' | grep -v grep | wc -l)"
echo $alive_servers

for pid in "${SERVER_PIDS[@]}"
do
  kill -HUP ${pid} > /dev/null
done

if test ${alive_servers} -ne ${SERVER_COUNT}
then
  echo "[   Failed ] Echo_Server.Members${SERVER_COUNT}"
  echo "[----------] 1 test from EchoServer"
  echo "[==========] 1 test from 1 test case ran."
  echo "[  FAILED  ] 1 test."
  exit -1
fi

wait > /dev/null
echo "[       OK ] Echo_Server.Members${SERVER_COUNT}"
echo "[----------] 1 test from EchoServer"
echo "[==========] 1 test from 1 test case ran."
echo "[  PASSED  ] 1 test."
