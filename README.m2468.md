# Meizu 21 Note (M2468) · LineageOS 23.2

配套 QCOM 外置驱动，已纳入 M2468 显示、Goodix、WLAN 和 CS35L43 音频适配。

本仓库由 [kernel_manifest](https://github.com/meizu-sm8550-kernel/kernel_manifest) 一并拉取，分支 `lineage-23.2`。请使用其中的固定 manifest 和构建说明；不需要另套本地接入补丁。

上游：[来源](https://github.com/LineageOS/android_kernel_qcom_sm8550-modules.git)，基线提交 `d00477fbff4a83babbee37b2ea56fb0ded793eec`。保留原有许可证及版权声明。这里的 ROM 基准是 LineageOS 23.2；SoC内核基准仍是 Android 13 / Linux 5.15，二者不是同一版本号。

本地已完成核心、386个模块及六份M2468 DT的构建/静态验证，基础模块CRC配套。已有设备运行源码内核至开机动画的证据；本次音频候选尚未加载验证，尚无正常进入桌面与音频功能证据。不得混用stock ko、伪造CRC/vermagic或关闭模块检查。

音频组新增 `cs35l43_dlkm`；M2468 声卡使用 TX3/TX4 和 secondary MI2S 双功放，避免等待 DT 已禁用的 RX macro。来源与验证边界见 [CS35L43 说明](qcom/opensource/audio-kernel/asoc/codecs/cs35l43/README.m2468.md)。

M2468局部HBM目前限部分亮屏模式；AOD、完整ready、手势/指纹、充电扩展与其它设备运行行为仍待验证。源码存在或编译通过不代表这些功能可用。
