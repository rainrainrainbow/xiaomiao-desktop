# XiaoMiao Desktop OS — OTA 更新指南

## 架构

系统使用双分区 OTA 机制：
- **ota_0 (launcher)**: 2.17MB — 桌面操作系统固件
- **ota_1 (retro_core)**: 1.25MB — 保留（模拟器核心）

分区表见 `partitions.csv`

## 本地 OTA 更新

### 1. 构建 OTA 固件

```bash
cd xiaomiao-desktop
idf.py build

# OTA 只需要 app 分区（不含 bootloader 和分区表）
# 产物在 build/xiaomiao-desktop.bin
```

### 2. 通过 HTTP 服务器分发

```bash
# 本机启动 HTTP 服务
python3 -m http.server 8000 --bind 0.0.0.0

# 固件 URL: http://your-ip:8000/build/xiaomiao-desktop.bin
```

### 3. 从设备端触发 OTA

设备启动后，通过串口发送命令触发 OTA：

```
# 串口终端输入
ota http://192.168.1.100:8000/build/xiaomiao-desktop.bin
```

> 注意：当前固件需要在设置页面添加 OTA 触发按钮，或通过串口 CLI 触发。

## GitHub Releases OTA

当打 tag 推送时，CI/CD 自动构建并发布 Release：

```bash
git tag v1.0.1
git push origin v1.0.1
```

Release 中的 `xiaomiao-desktop.bin` 可直接用于 OTA 更新。

## OTA 安全注意事项

1. **断电保护**: 升级过程中断电不会变砖（otadata 分区追踪状态）
2. **版本回退**: 如果新固件启动失败，自动回滚到旧版本
3. **固件大小**: ota_0 分区 2.17MB，固件不得超过此大小
4. **测试**: 大面积推送前先在测试设备上验证