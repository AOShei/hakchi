#!/bin/sh

g_timeout=5
g_count="$g_timeout"
g_previous_time=0

game_hangup_detect(){
  heartbeat_modification_time="$(stat /tmp/.heartbeat -c %Y)"
  if [ $g_previous_time -eq $heartbeat_modification_time ]; then
    g_count=$((g_count - 1))
  else
    g_count="$g_timeout"
  fi
  g_previous_time="$heartbeat_modification_time"
}

while true; do
  game_hangup_detect
  if [ $g_count -eq 0 ]; then
    touch "/var/startmcp.flag"
    g_count="$g_timeout"
  fi
  sleep 1
done
