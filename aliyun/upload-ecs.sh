#!/usr/bin/env bash
set -ex
source ./get-ecs-ip.sh
scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -r $1 root@${ECS_IP}:$2
