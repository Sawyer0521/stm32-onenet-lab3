# STM32L475 OneNET 物联网实验

本工程用于 VSCode 开发 STM32 与 Git 版本管理实验，基于 RT-Thread IoT SDK 的 OneNET 例程。

## 硬件与功能

- 主控：STM32L475VETx，RT-Thread 4.0.1。
- AP3216C 经 I2C 采集真实光照，WiFi 与 Paho MQTT 完成 OneNET 物模型通信。
- 约每 5 秒上报 Led、Motor、humi、lux、temp。
- 温度、湿度由软件模拟；Motor 是驱动控制百分比，不是实测转速。
- 云端指令控制 LED 亮度、电机驱动占空比；支持 MSH 本地命令。

## 目录

`examples/26_iot_cloud_onenet` 为应用，`drivers`、`libraries`、`rt-thread` 为必需公共依赖，不能只下载应用子目录。

## 配置与编译

1. 安装 Keil MDK 5、ARM Compiler 5.06 update 6 和 STM32L4xx_DFP 2.6.1。
2. 将 `examples/26_iot_cloud_onenet/applications/cloud_config.example.h` 复制为同目录 `cloud_config.h`，填写自己的 WiFi 和 OneNET 设备信息。本地配置已被 Git 忽略。
3. VSCode 安装 C/C++、Keil Assistant 和 Git Graph。设置 `KeilAssistant.MDK.Uv4Path` 为本机 `UV4.exe` 的完整路径。
4. 用 VSCode 打开例程目录，Keil Assistant 加载 `project.uvprojx`，在 `rt-thread_stm32l4xx` 目标右侧执行 Rebuild。
5. 构建生成 `build/rtthread-stm32l4xx.axf` 与 `rt-thread.bin`。连接 ST-Link 后才能验证下载和硬件运行。

## 本地命令

`onenet_data_status` 查看属性；`onenet_led_set 0..100` 设置亮度；`onenet_motor_set 0..100` 设置电机控制值。

## 实验验证范围

代码提交和构建不等于硬件验证。烧录、联网、云端控制结果应以实际开发板日志与截图记录为准。

## 来源与许可

保留 RT-Thread、STM32 HAL 和第三方软件包的原始版权及许可证，使用各组件时遵守其对应许可。仓库不包含本机 WiFi 密码或 OneNET 鉴权令牌。

## 版本记录

v1.1.0 增加开机固件版本输出。功能分支扩展状态查询以显示版本、MQTT 在线状态和上报间隔。
