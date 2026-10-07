# Meizu 21 Note (M2468) · lineage-23.2

2026-10-07：用户确认指纹、相机和闪光灯问题已修复。新增 [M2468 触控坐标修复](qcom/opensource/touch-drivers/README.m2468-coordinates.md)，将 X/Y 十倍单位转换下沉 Goodix；本地编译/CRC与手指上报回归通过，实机待验证。配套 ROM 必须移除原 inputflinger 除以 10 补丁，避免双重缩放。最新选择388项，本次仅替换goodix_ts。下方早期未上机叙述保留历史范围。

配套 QCOM 外置驱动，包含 M2468 显示、Goodix、CS35L43 音频和 JIIOV 指纹平台接口适配。

通过 [kernel_manifest](https://github.com/meizu-sm8550-kernel/kernel_manifest) 的分支跟随清单同步；不需要人工套补丁。发布分支为 `lineage-23.2`，不继承 ROM 分支，不固定项目 SHA，也不移除其他清单项目。

[公开上游](https://github.com/LineageOS/android_kernel_qcom_sm8550-modules.git)，基线 `d00477fbff4a83babbee37b2ea56fb0ded793eec`；保留原有许可证及版权声明。内核基线保持 SM8550 / Kalama / Android13 Linux5.15。ROM 构建规则参考官方 LineageOS23.2，用户运行的是24.0 / Android17，不能称为官方23.2整ROM验证。

既有源码基线已进入系统；用户确认ESD黑闪、bark误按键、Wi-Fi基本使用和启动提速。新的 JIIOV 候选仅完成源码接口回归、配套本地编译/CRC检查与加载配置，尚未加载到设备，也未验证probe、HAL初始化、TEE/校准、HBM、录入、匹配或解锁。

本地原386模块基线保留；选择库存增加 `jiiov_fingerprint`，加上既有bark与WLAN替换，共387项。基础内核和M2468 DT不变，不混用stock ko、不伪造CRC/vermagic、不关闭CFI/MODVERSIONS。

JIIOV保持M2468原DT参数及精确ioctl、netlink30/port100接口。配套M2468构建选择 `CONFIG_WLAN_DISABLE_CESIUM_NETLINK=y`，只释放无收发逻辑的Cesium占位socket；其它WLAN诊断通道保留。新增两模块的实机共存和Wi-Fi回归仍需验证。

指纹节点使用专用SELinux类型和受限ioctl规则；离线当前ROM策略对比无新增neverallow冲突，但基线策略存在两条冲突，不代表完整ROM策略编译通过。

完整显示/AOD、音频播放录音、相机、充电、温控及其它OEM行为仍有未验证内容。源码存在和编译成功不是功能恢复。

JIIOV实现、故障清理边界与主机回归见 [驱动说明](qcom/opensource/jiiov-fingerprint/README.md)。
