# tom-ps5-usbnet

让 PS5 通过 USB 万兆网卡接入局域网，用独立 IP 提供网页文件管理，在电脑或手机浏览器里管理主机上的文件。没有插网卡时，改用主机自带的网络提供同样的页面。

## 功能

- 管理内置存储、M.2 扩展硬盘和外接 USB 硬盘
- 上传、下载（文件夹打包成 tar）、复制、移动、重命名、删除、新建文件夹
- 拖入文件或文件夹即可上传，显示进度条、百分比和实时速度
- 电脑和主机上打开的页面同步显示传输状态
- 主屏幕图标：点开后用主机浏览器打开页面
- 手动硬盘测速
- 连接 NAS（SMB）：浏览共享文件夹，把文件或文件夹直接复制到内置存储、M.2 或外接硬盘
- 没有可用的 USB 网卡时，走主机自带网络，文件管理和 NAS 功能照常可用
- 退出时恢复网卡并关闭端口

## 已适配的网卡

| 芯片 | USB ID | 速率 | 状态 |
| --- | --- | --- | --- |
| Realtek RTL8159 | `0bda:815a` | 1G/2.5G/5G/10Gbps | 已实测 |
| Realtek RTL8156B / RTL8156BG | `0bda:8156` | 1G/2.5Gbps | 按 Linux `r8152` 驱动等公开资料适配，未实测 |

初代 RTL8156（不带 B）和改过 USB ID 的贴牌网卡暂未适配。没有检测到可用的网卡时，插件改用主机自带网络（见下文「不插网卡使用」）。

测试环境：PS5 Slim CFI-2200 B01，系统版本 12.40。其他机型和系统版本未测试。

## 实测速度

| 项目 | 速度 |
| --- | --- |
| TCP 上传 | 约 3.2 Gbit/s |
| TCP 下载 | 约 3.0 Gbit/s |
| 实际上传到 M.2 硬盘 | 约 370–440 MB/s |

链路协商到 10 Gbps，但实际速度到不了 10 Gbit/s：网卡驱动跑在用户态，受 USB 传输方式限制。

大文件连续写入时，测试用的 M.2 和 USB 硬盘都出现过中途降速，当时硬盘占满而网络空闲，瓶颈在硬盘写入一侧，具体原因尚未查明。

## 使用

1. 把网卡插到 PS5 的 USB 口，接好网线。
2. 用你的 ELF 加载器发送 `tom-ps5-usbnet.elf`。
3. 等主机弹出通知 `http://<IP>/`，在局域网设备的浏览器里打开。
4. 需要图标时，点页面顶部的「图标」添加到主屏幕。主机重启后要重新运行插件。
5. 用完点「退出」。再次使用前拔插一次网卡。

### 不插网卡使用

没有插网卡，或者插的网卡不受支持时，插件会弹出通知「未找到可用的 USB 网卡，使用主机网络」，然后在主机自带网络上开放 9190 端口。在浏览器里打开 `http://<主机 IP>:9190/` 即可，IP 在主机的网络设置里能看到。速度受主机自带网口或 Wi-Fi 限制。主机进入待机再唤醒后，插件会自动重新开放端口。

### 从 NAS 复制

1. 打开页面上的「NAS」，点「添加 NAS」，填地址（IP 或主机名）、用户名和密码。用户名留空就用来宾登录。
2. 点进共享和文件夹，勾选要拷的文件或文件夹，点「复制到主机」，选目标位置。
3. 进度显示在页面底部，和其他传输一样可以取消。中途失败或取消时，已拷的部分会被删掉。

支持 SMB 2/3（Windows 共享、群晖、威联通、Samba 等）。目前只支持从 NAS 复制到主机，同名项目已存在时不会覆盖。插着 USB 网卡时先经网卡连接 NAS，连不上再走主机自带网络，页面底部会显示走的是哪条线路。NAS 拷贝的速度还没有实测。

运行日志在 `/data/tom-ps5-usbnet/log.txt`，上一次的在 `log.old.txt`。日志不记录序列号、MAC 地址、IP 地址和文件名。

## 注意事项

- **页面没有登录。** 能访问这个端口的人都能读取和删除主机上的文件，也能用已保存的 NAS 账号浏览 NAS、把文件拷到主机，只在可信的局域网里使用，不要映射到公网。
- NAS 的密码保存在主机的 `/data/tom-ps5-usbnet/nas.txt`，没有加密，页面不会再显示它。不想保存密码，可以在 NAS 上开一个只读的来宾共享。
- 网卡运行时不要拔出。
- 不要和其他占用同一网卡的程序同时运行。

## 风险提醒

折腾有风险。

本插件通过非官方方式运行在 PS5 上，直接操作 USB 设备和存储。开发测试中出现过主机断电重启、外接硬盘盒和网卡掉线，原因没有完全查明。使用时可能出现数据丢失、主机或硬件异常。重要数据请先备份。

**本插件按「现状」提供，不提供任何担保。作者和贡献者不承担因使用本插件造成的硬件故障、数据丢失或其他损失的责任，使用风险由你自行承担。**

## 构建

需要 Python 3。`scripts/prepare.py` 默认下载 Apple Silicon macOS 版的 Zig；其他系统请改 `SPECS` 里的地址和校验值。

```bash
make prepare
```

```bash
make test
```

```bash
make ps5
```

输出在 `build-ps5/tom-ps5-usbnet.elf`。

## 开源协议

[GPL-3.0-or-later](LICENSE)。

主屏幕图标和权限部分的代码来自 ps5upload，同样采用 GPL-3.0。第三方代码：

- [lwIP](https://github.com/lwip-tcpip/lwip)：BSD 风格协议，见 `third_party/lwip/COPYING`
- [libsmb2](https://github.com/sahlberg/libsmb2)：LGPL-2.1-or-later，见 `third_party/libsmb2/README.md`
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) 的启动代码：GPL-3.0，见 `third_party/sdk-crt/LICENSE`

## 致谢

- [ps5upload](https://github.com/phantomptr/ps5upload)
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)
- [lwIP](https://savannah.nongnu.org/projects/lwip/)
- [libsmb2](https://github.com/sahlberg/libsmb2)
- Linux `r8152` 驱动：网卡初始化流程的参考

## 特别鸣谢

UP主：爱折腾的老高
频道：
B站https://space.bilibili.com/455976991
YouTube：https://www.youtube.com/@diyfun
博客：https://diyforfun.cn/

感谢他提供PS5设备和万兆网卡和万兆局域网的测试环境并全程测试与问题汇总反馈。
