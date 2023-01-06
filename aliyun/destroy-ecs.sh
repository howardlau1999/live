#!/usr/bin/env bash
echo -n "Destroy test ECS stack! Confirm: "
read REPLY
if [ "$REPLY" != "y" ] && [ "$REPLY" != "Y" ]; then
    exit
fi
set -xe
ros-cdk destroy AliyunStack --sync=true --quiet=true
rm stack.outputs.json