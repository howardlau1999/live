#!/usr/bin/env bash
source ./get-ecs-ip.sh
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@$ECS_IP $@