# XiaoMiao Desktop OS — OTA 更新指南

## 架构

硬件 Flash 为 16MB（QIO 80MHz），分区表见 `partitions.csv`。

| 分区 | 类型/子类型 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24KB | 非易失存储 |
| phy_init | data/phy | 0xF000 | 4KB | 射频校准 |
| factory | app/factory | 0x10000 | 1MB | Loader/Recovery，OTA 永不覆盖（label 不可改） |
| otadata | data/ota | 0x110000 | 8KB | OTA 启动选择 |
| launcher | app/ota_0 | 0x120000 | 4MB | OTA 槽 A：桌面系统固件（label 不可改，app_ota.c 按此查找） |
| launcher_b | app/ota_1 | 0x520000 | 4MB | OTA 槽 B：预留给未来 A/B 安全升级 |
| retro-core | data/fat | 0x920000 | 5MB | 挂载到 /flash，存放字库/图标/内置音乐（label 不可改） |

注：`launcher` 与 `retro-core` 的 label 被现有代码硬编码引用，禁止修改。

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
3. **固件大小**: ota_0 分区 4MB，固件不得超过此大小
4. **测试**: 大面积推送前先在测试设备上验证