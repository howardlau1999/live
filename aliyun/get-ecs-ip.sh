#!/usr/bin/env bash
GREEN='\033[0;32m'
NC='\033[0m'
STACK_NAME=${STACK_NAME:-"AliyunStack"}
echo -e "Getting ECS IP address for ${GREEN}${STACK_NAME}${NC}..."
export ECS_IP=$(jq -r ".${STACK_NAME} | map(select(.OutputKey == \"public_ip\"))[0].OutputValue?" stack.outputs.json)
if [ -z $ECS_IP ]; then
  echo -e "Could not find ECS IP address for ${GREEN}${STACK_NAME}${NC}."
  exit 1
fi
echo "Connecting to $STACK_NAME, ECS_IP=$ECS_IP"