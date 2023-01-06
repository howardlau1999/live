#!/usr/bin/env bash
set -xe
ros-cdk deploy AliyunStack --sync=true --outputs-file=true
export ECS_IP=$(jq -r '.AliyunStack | map(select(.OutputKey == "public_ip"))[0].OutputValue?' stack.outputs.json)
echo "ECS_IP=$ECS_IP"
export INSTANCE_ID=$(jq -r '.AliyunStack | map(select(.OutputKey == "instance_id"))[0].OutputValue?' stack.outputs.json)
echo "INSTANCE_ID=$INSTANCE_ID"
aliyun ecs ModifyInstanceVncPasswd --InstanceId=$INSTANCE_ID --VncPassword=Lhh123
ssh-keygen -f "~/.ssh/known_hosts" -R "$ECS_IP" || true