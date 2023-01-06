#!/usr/bin/env node
import * as ros from '@alicloud/ros-cdk-core';
import { AliyunStack } from '../lib/aliyun-stack';

const app = new ros.App({outdir: './cdk.out'});
new AliyunStack(app, 'AliyunStack');
app.synth();