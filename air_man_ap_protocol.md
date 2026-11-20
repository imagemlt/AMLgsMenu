# air_man_ap TCP 协议文档

## 基本信息

| 项目 | 值 |
|---|---|
| 传输层 | TCP |
| IP | 0.0.0.0（Air Unit 本机） |
| 端口 | **12355** |
| 编码 | ASCII，文本协议 |
| 请求结束符 | `\n` 或 `\r\n` |
| 响应结束符 | `\n`（snprintf 输出，无显式换行，部分命令直接写入 socket） |

连接流程：`accept()` → `read(fd, buf, 1023)` → 去尾 `\r\n` → 命令派发 → `write(fd, resp, strlen(resp))` → `close(fd)`

---

## 命令列表

### 1. `get_all_video_modes`

返回所有视频模式列表，每行一个。

**请求：**
```
get_all_video_modes
```

**响应：**
空行结尾的模式列表。格式推测：
```
1920x1080p60
1920x1080p30
1280x720p60
...
```

> 实现：遍历一个 320 字节/条目的模式表，用 `strncat` 拼接模式名称到响应缓冲区。

---

### 2. `get_current_video_mode`

读取 `/etc/sensors/mode_current` 并返回。

**请求：**
```
get_current_video_mode
```

**响应：**
文件内容的第一行（去掉尾部 `\n`）。例如：
```
1920x1080p30
```

**失败时：**
```
Failed to open /etc/sensors/mode_current
```

---

### 3. `set_video_mode <size> <fps> <exposure> '<crop>'`

设置视频模式，参数可以单引号或双引号包裹 crop。

**请求：**
```
set_video_mode 1920x1080 60 1000 '0,0,1920,1080'
```

或者双引号：
```
set_video_mode 1920x1080 60 1000 "0,0,1920,1080"
```

**解析：**
```
sscanf(buf, "%31s %d %d '%127[^']'", &name, &fps, &exposure, &crop)
```
如果失败则尝试：
```
sscanf(buf, "%31s %d %d \"%127[^\"]\"", &name, &fps, &exposure, &crop)
```

**内部执行（简化）：**
```
cli -s .video0.size 1920x1080
cli -s .video0.fps 60
cli -s .isp.exposure 1000
cli -s .fpv.roiRect '0,0,1920,1080'
cli -s .fpv.roiQp 0
```

然后 `fork()` 子进程执行后续步骤（重试直至成功）：
1. `wget -q -T 1 -O - http://127.0.0.1/api/v1/config.json >/dev/null 2>&1` — 等待 majestic web 就绪（最多 15 秒）
2. `pidof aalink >/dev/null 2>&1` — 等待 aalink 启动（最多 5 秒）
3. `/etc/init.d/S95majestic reload` — 触发 majestic 重载
4. `echo setprecrop <crop> > /proc/mi_modules/mi_vpe/mi_vpe0`
5. `pidof msposd >/dev/null 2>&1 && kill -HUP $(pidof msposd)` — OSD 重载
6. `pidof aalink >/dev/null 2>&1` — 确认 aalink

**成功响应：**
```
Video mode '1920x1080' @ 60fps exp=1000 crop='0,0,1920,1080' roiRect='0,0,1920,1080' applied
```

**失败响应（步骤 1-6 任一失败）：**
无显式响应（子进程直接 `_exit(0)` 或 `stderr` 输出 `"manual video-mode service apply failed\n"`）

**参数格式错误：**
```
Invalid format. Use: set_video_mode <size> <fps> <exposure> '<crop>'
```

---

### 4. `set_simple_video_mode <value>`

简化的视频模式设置，内部调用 `set_video_mode` 处理。

**请求：**
```
set_simple_video_mode <value>
```

**行为：** 解析方式同 `set_video_mode`，但参数格式不同。

---

### 5. `get_region`

获取 WiFi 监管区域（regulatory domain）。

**请求：**
```
get_region
```

**内部执行：**
```
popen("iw reg get | grep country | awk '{print $2}'", "r")
```

**响应：**
国家代码（如 `CN`、`US`、`JP` 等），或空（如果 `iw` 命令失败）。

---

### 6. `set_region <code>`

设置 WiFi 监管区域。

**请求：**
```
set_region CN
```

**内部执行：**
```
iw reg set CN
```

**成功响应：**
```
Region set to CN
```

**失败响应：**
```
Failed to set region
```

---

### 7. `get_all_ap_channels`

返回所有 AP 信道列表。

**请求：**
```
get_all_ap_channels
```

**响应：**
空行结尾的信道列表。遍历 43 个信道条目（304 字节固定宽度），支持的信道见下方信道表。

**信道列表条目格式：**
- 80 MHz VHT / 40 / 20 MHz HT，每个条目 320 字节宽的表中
- 每个信道条目包含：信道号范围、频率、带宽、hostapd_cli 命令字符串

**支持的信道：**

| 信道 | 频率 (MHz) | 20 MHz HT | 40 MHz | 80 MHz VHT |
|---|---|---|---|---|
| 36_40 | 5180 | hostapd_cli -i wlan0 chan_switch 5 5180 ht | +sec_channel | center1=5210 |
| 44_48 | 5220 | hostapd_cli -i wlan0 chan_switch 5 5220 ht | +sec_channel | - |
| 52_56 | 5260 | hostapd_cli -i wlan0 chan_switch 5 5260 ht | +sec_channel | center1=5290 |
| 60_64 | 5300 | hostapd_cli -i wlan0 chan_switch 5 5300 ht | +sec_channel | - |
| 100_104 | 5500 | ... | ... | center1=5530 |
| 108_112 | 5540 | ... | ... | - |
| 116_120 | 5580 | ... | ... | center1=5610 |
| 124_128 | 5620 | ... | ... | - |
| 132_136 | 5660 | ... | ... | center1=5690 |
| 140_144 | 5700 | ... | ... | - |
| 149_153 | 5745 | ... | ... | center1=5775 |
| 157_161 | 5785 | ... | ... | - |
| 165 | 5825 | ... | - | - |

> 共 22 个 20 MHz 信道、12 个 40 MHz 信道、6 个 80 MHz 信道。

**响应格式（推测）：**
```
36
40
44
...
```

---

### 8. `get_current_ap_channel`

获取当前 AP 信道。

**请求：**
```
get_current_ap_channel
```

**内部执行：**
```
popen("iw dev wlan0 info", "r") → 解析 "channel X (Y MHz)" 行
```

**成功响应：**
```
channel 36 (5180 MHz)
```

**失败响应：**
```
Could not determine current channel
```

---

### 9. `set_ap_channel <channel>`

切换 AP 信道，包含 ping 验证和回退机制。

**请求：**
```
set_ap_channel 36
```

`<channel>` 可以是数字信道号或频率（如 `5180`），通过查预置信道表匹配。

**内部流程：**
1. 加锁 `/tmp/air_man_ap.control.lock`（`flock` 排他锁）
2. 查表匹配信道名（最多 43 个条目，每个 304 字节），`strcmp` 匹配
3. 执行对应的 `hostapd_cli -i wlan0 chan_switch ...` 命令（最多重试 3 次，每次间隔 1s）
4. Ping 验证（使用 `ping -c1 -W1 <master_ip> > /dev/null`）：
   - 最多等 20 秒（每次 1s），最多 20 次 ping 尝试
   - 如果 ping 失败，执行回退信道命令
   - 回退后最多等 5 秒
5. 如果信道切换成功：`fw_setenv wlanchan <channel>` 写入 U-Boot 环境变量
6. 解锁

**成功响应：**
```
Channel set to 36
```

**失败响应（信道未识别）：**
```
Channel not recognized: 36
```

**失败响应（Master 失联回退）：**
```
Master ping failed after channel switch, reverted to <previous_channel>
```

**失败响应（其他操作进行中）：**
```
Busy: another control operation is in progress
```

---

### 10. 未知命令

**请求：**
```
some_unknown_command
```

**响应：**
```
Unknown command: some_unknown_command
```

---

## 信道选择命令速查（hostapd_cli 参数）

| 频率 (MHz) | 20 MHz HT | 40 MHz | 80 MHz VHT |
|---|---|---|---|
| 5180 | `chan_switch 5 5180 ht` | `chan_switch 5 5180 sec_channel_offset=1 center_freq1=5190 bandwidth=40 ht` | `chan_switch 5 5180 sec_channel_offset=1 center_freq1=5210 bandwidth=80 vht` |
| 5200 | ... | `chan_switch 5 5200 ... bandwidth=40 ht` | - |
| 5220 | ... | `chan_switch 5 5220 ... bandwidth=40 ht` | - |
| 5240 | ... | `chan_switch 5 5240 ... bandwidth=40 ht` | - |
| 5260 | ... | `chan_switch 5 5260 ... bandwidth=40 ht` | `chan_switch 5 5260 ... bandwidth=80 vht` |
| 5280-5320 | (各频率) | ... | - |
| 5500-5720 | ... | ... | (部分) |
| 5745-5825 | ... | ... | (部分) |

> 每个信道命令在二进制中硬编码为预编译字符串，不是动态拼接。
> 带宽和频率组合有限，不支持的组合返回 `Channel not recognized`。

## 相关文件

| 文件 | 作用 |
|---|---|
| `/etc/sensors/mode_current` | 存储当前视频模式名 |
| `/etc/rc.local` | 存储持久化的 setprecrop 命令（sed 注入） |
| `/proc/mi_modules/mi_vpe/mi_vpe0` | 写入 crop 值 |
| `/tmp/air_man_ap.control.lock` | 信道切换互斥锁 |
| U-Boot env `wlanchan` | 持久化存储信道 |
| U-Boot env `master_ip` | 地面站 IP（默认 192.168.0.10） |

## 依赖的命令行工具

```
cli          → 读写 majestic 运行时参数
hostapd_cli  → 控制 hostapd 信道切换
iw           → 获取/设置监管区域、获取接口信息
fw_printenv  → 读取 U-Boot 环境变量
fw_setenv    → 写入 U-Boot 环境变量
wget         → 探测 majestic HTTP 就绪
pidof/kill   → 进程管理
sed          → 注入/删除 /etc/rc.local 中的 setprecrop
ping         → Master 连通性验证
```
