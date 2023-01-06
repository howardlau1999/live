import * as ros from '@alicloud/ros-cdk-core';
import * as ecs from '@alicloud/ros-cdk-ecs';
import * as ROS from '@alicloud/ros-cdk-ros';
import { readFileSync } from 'fs';
import { hostname } from 'os';
const imageAndStartScript = {
  "ubuntu": {
    startScript: `#!/bin/bash
      export DEBIAN_FRONTEND=noninteractive
      apt-get update && apt-get install -y curl
      curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /usr/share/keyrings/docker-archive-keyring.gpg
echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] https://download.docker.com/linux/ubuntu \
  $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
      apt-get update
      apt-get install -y htop iftop docker-ce docker-ce-cli containerd.io curl git g++ gcc make libavformat-dev libavdevice-dev libavcodec-dev libavutil-dev libavfilter-dev libswscale-dev 

      NOTIFY
  `,
    imageId: "ubuntu_22_04_x64_20G_alibase_20220803.vhd",
  },
}

export class AliyunStack extends ros.Stack {
  constructor(scope: ros.Construct, id: string, props?: ros.StackProps) {
    super(scope, id, props);
    new ros.RosInfo(this, ros.RosInfo.description, "This is the simple ros cdk app example.");
    // The code that defines your stack goes here

    const fromWhich = process.env.IMAGE_FROM || "ubuntu";
    const spec = (imageAndStartScript as any)[fromWhich];
    const specImageId = spec.imageId;
    const specStartScript = spec.startScript;

    const zoneId = 'cn-guangzhou-a';

    const ecsVpc = new ecs.Vpc(this, 'hcache-vpc', {
      vpcName: 'hcache-vpc',
      cidrBlock: '10.0.0.0/8',
      description: 'hcache vpc'
    });

    // 构建 VSwitch
    const ecsvSwitch = new ecs.VSwitch(this, 'hcache-vswitch', {
      vpcId: ecsVpc.attrVpcId,
      zoneId: zoneId,
      vSwitchName: 'hcache-vswitch',
      cidrBlock: '10.1.1.0/24',
    });

    // 指定系统镜像、系统密码、实例类型
    const ecsImageId = new ros.RosParameter(this, "ecs_image_id", {
      type: ros.RosParameterType.STRING,
      defaultValue: specImageId,
    });
    const ecsPassword = new ros.RosParameter(this, "ecs_password", {
      type: ros.RosParameterType.STRING,
      noEcho: true, minLength: 8, maxLength: 30,
      allowedPattern: "[0-9A-Za-z\\_\\-\\&:;'<>,=%`~!@#\\(\\)\\$\\^\\*\\+\\|\\{\\}\\[\\]\\.\\?\\/]+$",
      defaultValue: "livelhh@2023",
    });
    const ecsInstanceType = new ros.RosParameter(this, "ecs_instance_type", {
      type: ros.RosParameterType.STRING,
      defaultValue: "ecs.hfc7.2xlarge",
      associationProperty: "ALIYUN::ECS::Instance::InstanceType",
      associationPropertyMetadata: {
        "ZoneId": zoneId,
      },
    });
    const ecsSystemDiskCategory = new ros.RosParameter(this, "ecs_system_disk_category", {
      type: ros.RosParameterType.STRING,
      defaultValue: "cloud_essd",
    });

     // 创建安全组开放端口
    const sg = new ecs.SecurityGroup(this, 'hcache-sg', { vpcId: ecsVpc.attrVpcId });

    let ports = ['22', '8080', '80', '443', '1935', '8081', '1985'];
    for (const port of ports) {
      new ecs.SecurityGroupIngress(this, `hcache-sg-ingress-${port}`, {
        portRange: `${port}/${port}`,
        nicType: 'intranet',
        sourceCidrIp: '0.0.0.0/0',
        ipProtocol: 'tcp',
        securityGroupId: sg.attrSecurityGroupId
      }, true);
    }

    // 密钥导入，默认读取本地的公钥
    const pubKey = readFileSync(`${process.env.HOME}/.ssh/id_rsa.pub`).toString();
    const ecsKeyPair = new ecs.SSHKeyPair(this, 'hcache-key-pair', {
      keyPairName: `hcache-key-pair-${hostname()}`,
      publicKeyBody: pubKey,
      tags: [{ key: 'hcache', value: hostname() }],
    });

    // 等待逻辑，用于等待 ECS 中应用安装完成
    const ecsWaitConditionHandle = new ROS.WaitConditionHandle(this, 'RosWaitConditionHandle', {
      count: 1
    });

    const ecsWaitCondition = new ROS.WaitCondition(this, 'RosWaitCondition', {
      timeout: 600,
      handle: ros.Fn.ref('RosWaitConditionHandle'),
      count: 1
    });

    const ecsInstance = new ecs.Instance(this, 'hcache-test', {
      vpcId: ecsVpc.attrVpcId,
      keyPairName: ecsKeyPair.attrKeyPairName,
      vSwitchId: ecsvSwitch.attrVSwitchId,
      imageId: ecsImageId,
      securityGroupId: sg.attrSecurityGroupId,
      instanceType: ecsInstanceType,
      instanceName: 'hcache-test-ecs',
      hostName: 'hcache-build',
      systemDiskCategory: ecsSystemDiskCategory,
      password: ecsPassword,
      spotStrategy: 'SpotAsPriceGo',
      spotDuration: 0,
      allocatePublicIp: true,
      internetMaxBandwidthOut: 50,
      internetChargeType: 'PayByBandwidth',
      userData: ros.Fn.replace({ NOTIFY: ecsWaitConditionHandle.attrCurlCli }, specStartScript),
    });

    new ros.RosOutput(this, 'instance_id', { value: ecsInstance.attrInstanceId });
    new ros.RosOutput(this, 'private_ip', { value: ecsInstance.attrPrivateIp });
    new ros.RosOutput(this, 'public_ip', { value: ecsInstance.attrPublicIp });
  }
}
