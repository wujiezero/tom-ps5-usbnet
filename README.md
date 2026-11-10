# tom-ps5-usbnet

给 PS5 插上 USB 万兆网卡，插件会用网卡自己的 IP 开一个网页，在电脑或手机浏览器里就能管理主机上的文件。不插网卡也能用，走主机自带网络。

## 功能

- 管理内置存储、M.2 扩展硬盘和外接 USB 硬盘
- 上传、下载（文件夹打包成 tar）、复制、移动、重命名、删除、新建文件夹
- 拖进文件或文件夹就能上传，有进度条、百分比和实时速度
- 电脑和主机上开着的页面会同步显示传输进度
- 主屏幕图标，点开就用主机浏览器打开页面
- 手动硬盘测速
- 连接 NAS（SMB），浏览共享文件夹，把文件或文件夹直接拷到内置存储、M.2 或外接硬盘
- 没有能用的 USB 网卡时走主机自带网络，文件管理和 NAS 照样能用
- 退出时还原网卡、关掉端口

## 已适配的网卡

| 芯片 | USB ID | 速率 | 状态 |
| --- | --- | --- | --- |
| Realtek RTL8159 | `0bda:815a` | 1G/2.5G/5G/10Gbps | 已实测 |
| Realtek RTL8156B / RTL8156BG | `0bda:8156` | 1G/2.5Gbps | 已实测（2.5G） |

初代 RTL8156（不带 B）和改过 USB ID 的贴牌网卡还不支持。检测不到能用的网卡时，插件会改走主机自带网络（见下面「不插网卡使用」）。

测试机器：PS5 Slim CFI-2200 B01，系统 12.40。别的机型和系统版本没测过。

## 实测速度

RTL8159，万兆链路：

| 项目 | 速度 |
| --- | --- |
| TCP 上传 | 约 3.2 Gbit/s |
| TCP 下载 | 约 3.0 Gbit/s |
| 实际上传到 M.2 硬盘 | 约 370–440 MB/s |

链路虽然协商到 10 Gbps，实际跑不到这么快，因为网卡驱动跑在用户态，受 USB 传输方式限制。

RTL8156B 在 2.5G 链路下，上传到扩展存储约 141 MB/s。

连续写大文件时，测试用的 M.2 和 USB 硬盘都出现过中途掉速。掉速时硬盘满载、网络空闲，所以是卡在硬盘写入上，具体原因还没查清。

## 使用

1. 把网卡插到 PS5 的 USB 口，接好网线。
2. 用你的 ELF 加载器发送 `tom-ps5-usbnet.elf`。
3. 主机弹出 `http://<IP>/` 的通知后，在同一局域网的电脑或手机浏览器里打开。
4. 想要主屏幕图标，点页面顶部的「图标」。主机重启后要重新运行插件。
5. 用完点「退出」。下次用之前把网卡拔插一次。

### 不插网卡使用

没插网卡，或插的网卡不支持时，插件会弹出「未找到可用的 USB 网卡，使用主机网络」，然后在主机自带网络上开 9190 端口。浏览器打开 `http://<主机 IP>:9190/` 就行，主机 IP 在网络设置里能看到。速度取决于主机自带网口或 Wi-Fi。主机待机再唤醒后，插件会自动重新开端口。

### 从 NAS 复制

1. 打开页面上的「NAS」，点「添加 NAS」，填地址（IP 或主机名）、用户名和密码。用户名留空就是来宾登录。
2. 点进共享文件夹，勾选要拷的文件或文件夹，点「复制到主机」，选好目标位置。
3. 进度在页面底部，可以随时取消。失败或取消时，拷了一半的内容会被删掉。

支持 SMB 2/3（Windows 共享、群晖、威联通、Samba 等）。现在只能从 NAS 拷到主机，目标位置有同名项目时不会覆盖。插着 USB 网卡时先用网卡连 NAS，连不上再走主机自带网络，页面底部会显示走的哪条线路。NAS 拷贝速度还没实测过。

运行日志在 `/data/tom-ps5-usbnet/log.txt`，上一次的在 `log.old.txt`。日志不记录序列号、MAC 地址、IP 地址和文件名。

## 注意事项

- **页面没有登录。** 能访问这个端口的人都能读、删主机上的文件，也能用保存的 NAS 账号浏览 NAS、往主机拷文件。只在信得过的局域网里用，别映射到公网。
- NAS 密码明文存在主机的 `/data/tom-ps5-usbnet/nas.txt`，页面上不会再显示。不想存密码，可以在 NAS 上开一个只读的来宾共享。
- 插件运行时别拔网卡。
- 别和其他占用同一块网卡的程序一起运行。

## 风险提醒

折腾有风险。

本插件用非官方方式在 PS5 上运行，直接操作 USB 设备和存储。开发测试时出现过主机断电重启、外接硬盘盒和网卡掉线，原因还没完全查清。用的时候可能丢数据，主机或硬件也可能出问题，重要数据先备份。

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

- B站：https://space.bilibili.com/455976991
- YouTube：https://www.youtube.com/@diyfun
- 博客：https://diyforfun.cn/

感谢他提供PS5设备和万兆网卡和万兆局域网的测试环境并全程测试与问题汇总反馈。
