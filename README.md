# m2468 外置驱动

配套 QCOM 外置驱动，包含 M2468 的显示与背光、Goodix 触控、CS35L43 音频、JIIOV 指纹、相机闪光灯和 WLAN 适配。

本仓库属于 **meizu-sm8550-kernel**，**当前仅支持 m2468（魅族 21 Note）**。组织与内核仓库名称中的 `sm8550` 表示平台，不表示支持其它魅族 SM8550 设备；M2481（魅族 21 Pro）也不在本适配范围内。

发布分支为 `lineage-23.2`。使用 [kernel_manifest](https://github.com/meizu-sm8550-kernel/kernel_manifest) 同步四个配套源码仓库，并按其中的构建说明编译。清单跟随该分支，`revisions.lock.json` 只记录发布版本。

设备路径、配置和自有代码标识使用 `m2468` / `M2468`。提交采用“子系统前缀 + 首字母大写的动作描述”，每条提交聚焦一项修改，见 [提交约定](https://github.com/meizu-sm8550-kernel/kernel_manifest/blob/lineage-23.2/CONTRIBUTING.md)。原厂 DT 属性、固件名和运行时接口保持兼容。

上游基线为 `d00477fbff4a83babbee37b2ea56fb0ded793eec`，来源和许可信息见 [m2468-source-provenance.json](m2468-source-provenance.json)。保留上游许可证和版权声明。

本次整理只调整提交历史、内部命名和文档。此前编译与设备反馈的范围见 [历史适配记录](docs/m2468-bringup-history.md)；未因本次整理新增整 ROM 编译或实机验证结论。各项主机回归不能代替外设运行验证。

驱动说明与测试入口：

- [背光](qcom/opensource/display-drivers/Documentation/m2468-backlight.rst)：`qcom/opensource/display-drivers/tests/m2468_backlight/run.py`。
- [触控坐标](qcom/opensource/touch-drivers/README.m2468-coordinates.md)与[默认上报率](qcom/opensource/touch-drivers/README.m2468-report-rate.md)：`qcom/opensource/touch-drivers/tests/`。
- [CS35L43 音频](qcom/opensource/audio-kernel/asoc/codecs/cs35l43/README.m2468.md)。
- [JIIOV 指纹](qcom/opensource/jiiov-fingerprint/README.md)：`qcom/opensource/jiiov-fingerprint/tests/run.py`。

使用驱动中的坐标缩放时，ROM 必须移除旧 inputflinger 除以 10 的补丁，避免重复缩放。
