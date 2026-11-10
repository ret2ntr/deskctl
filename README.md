# deskctl

给 agent 用的 Linux 桌面"眼睛和手"：截图看屏幕、枚举/聚焦窗口、注入键盘和鼠标。
**不需要 root，不加载内核模块，不需要 `/dev/uinput`，也不依赖 `xdotool`。**

```console
$ ./deskctl info
{"ok": true, "version": "0.1.0", "session": "wayland", "compositor": "hyprland",
 "wayinput": ".../build/wayinput", "capabilities": {"screenshot": true, "windows": true, "input": true},
 "screen": {"width": 2560, "height": 1600}}
```

## 为什么会有这个项目

在 Wayland 桌面上，一个没有 root、装在沙箱里的 agent 想"动一下鼠标"几乎无路可走：

| 常见做法 | 为什么会失败 |
| --- | --- |
| `xdotool` | 只支持 X11，Wayland 下没有 XTEST |
| `ydotool` | 依赖 `/dev/uinput`（内核模块 + 权限），容器/沙箱里通常没有 |
| `wtype` | 只能打字，不能动鼠标；且需要额外的包 |
| 直接调 `hyprctl` | 只能改窗口布局，点不进应用内部 |

但 Wayland 本身留了正规接口：**`zwp_virtual_keyboard_manager_v1`** 和
**`zwlr_virtual_pointer_manager_v1`**。Hyprland、Sway 等 wlroots 系合成器都实现了它们。
它不走内核、不碰 `/dev`，只是一个普通的 Wayland 客户端，因此**普通用户权限就能用**。

本项目把这些接口包装成一个小 C 程序（`wayinput`）加一个 JSON 输出的 CLI（`deskctl`），
让 agent 可以用最朴素的方式驱动：看截图 → 算坐标 → 点击/打字 → 再看截图。

## 架构

```
        agent
          │  （只是执行命令 / 解析 JSON）
          ▼
      deskctl                  Python CLI：窗口查询、焦点保护、后端选择
          │
   ┌──────┴────────┐
   ▼               ▼
 hyprctl        wayinput     C，~450 行，直连 Wayland socket
 grim             │
（截图）      ┌────┴─────┐
              ▼          ▼
   zwp_virtual_keyboard  zwlr_virtual_pointer
   （打字/快捷键）        （移动/点击/滚轮）
```

所有命令都往 stdout 打**一个** JSON 对象，失败时 `ok: false` 且退出码非 0，方便 agent 直接 `json.loads`。

## 构建

依赖：`gcc`、`wayland-client`、`xkbcommon`、`wayland-scanner`、`python3`、`grim`（截图）。

```console
$ make                 # 生成 build/wayinput（协议代码由 wayland-scanner 现场生成）
$ ./deskctl info       # 自检
$ make test            # 冒烟测试：真的动指针、真的往窗口里打字
```

安装到系统：

```console
$ sudo make install    # /usr/local/bin/deskctl + /usr/local/lib/deskctl/wayinput
```

## 命令

| 命令 | 作用 |
| --- | --- |
| `deskctl info` | 会话类型、合成器、屏幕尺寸、可用能力 |
| `deskctl windows` | 所有窗口：class / title / 几何 / 是否聚焦 |
| `deskctl focus <模式>` | 按 class 或标题子串聚焦；多个匹配会拒绝执行 |
| `deskctl shot [-o 文件] [--window 模式]` | 全屏或单个窗口截图，返回尺寸 |
| `deskctl type <文本> [--window 模式]` | 打字，支持 UTF-8、大小写、符号 |
| `deskctl key <组合键> [--window 模式]` | 如 `Return`、`ctrl+shift+t`、`super+space` |
| `deskctl move <x> <y>` | 指针绝对移动 |
| `deskctl click <x> <y> [--button left] [--count 2] [--window 模式]` | 点击 |
| `deskctl scroll <dy> [--dx dx]` | 滚轮，正数向下 |
| `deskctl wait <模式> [--timeout 10]` | 等窗口出现（启动应用后用） |

示例：

```console
$ ./deskctl windows
{"ok": true, "count": 5, "windows": [{"id": "0x55a8…", "class": "QQ", "title": "QQ",
  "x": 1917, "y": 80, "w": 1246, "h": 1498, "workspace": "1", "focused": false, …}]}

$ ./deskctl focus QQ
{"ok": true, "focused": {"class": "QQ", "title": "QQ"}}

$ ./deskctl type --window QQ "hello"
{"ok": true, "action": "type", "target": {"class": "QQ", "title": "QQ"},
 "args": {"text": "hello"}}

$ ./deskctl shot --window QQ -o /tmp/qq.png
{"ok": true, "path": "/tmp/qq.png", "width": 1246, "height": 1498, "bytes": 229075}
```

## 焦点保护（重点）

agent 打错窗口是这类工具最现实的危险：一句 `type` 可能打进用户的编辑器、终端或聊天框。
所以：

- `--window <模式>` 会先聚焦、再**复核当前活动窗口的 address 是否真的是目标**，不一致就中止，一个字符都不发；
- `focus` 遇到多个匹配（比如两个叫"心雨"的窗口）直接报错并列出候选，绝不猜；
- 每次输入的结果里都带上 `target`，调用方能确认这一下打给了谁；
- 没有 `--window` 时按键发给当前焦点窗口，但仍然在返回值里报告 `target`。

## 兼容性

| 环境 | 截图 | 窗口 | 键盘 | 鼠标 |
| --- | --- | --- | --- | --- |
| Hyprland 0.4x–0.5x | ✅ grim | ✅ hyprctl | ✅ | ✅ |
| Sway / wlroots | ✅ grim | ✅ swaymsg | ✅ | ✅ |
| GNOME (Mutter) | ✅ grim/gnome-screenshot | ⚠️ 需扩展 | ⚠️ 仅受信任客户端 | ❌ 未实现 virtual pointer |
| X11 | ✅ import/scrot | ✅ wmctrl | ✅ xdotool | ✅ xdotool |

X11 下 `deskctl` 会自动改用 `xdotool`/`wmctrl`，行为保持一致。

非 US 键盘布局用 `DESKCTL_XKB_LAYOUT=de` 之类的环境变量指定（默认 `us`）。

## 输入法与粘贴（实测踩坑）

中文没法靠 keysym 直接打——xkb 里没有汉字的 keysym，只能借输入法：

```console
$ fcitx5-remote -o                     # 打开（切到中文）
$ DESKCTL_TYPE_DELAY_MS=60 ./build/wayinput type "nihao "   # 拼音 + 空格提交首选词 → 你好
$ fcitx5-remote -c                     # 切回英文，再打 DeepSeek 这类拉丁文
```

两个要点：

- **必须加按键间隔**（`DESKCTL_TYPE_DELAY_MS`）。连发时 fcitx5 会丢键或把拼音拆错，一次一个键就没问题。
- **别指望粘贴**。Linux QQ 3.2.34 上 Ctrl+V 无效，甚至连它自己右键菜单里的「粘贴」也只往输入框塞一个占位图标；`wl-copy` 侧是正常的（`wl-paste` 能读回全文）。这类自绘客户端只能老老实实走输入法打字。

## 已知限制

- 坐标是多显示器**全局布局坐标**，与 `hyprctl clients` 一致；窗口如果停在已拔掉的显示器位置上会跑到可见区域外，需要先把它挪回来。
- Wayland 没有"读控件树"的接口，agent 只能靠截图 + 视觉判断，不是可访问性树。
- 合成器可能对虚拟输入设备做额外限制（例如锁屏、某些安全输入框）。
- GNOME 需要额外扩展才能枚举/聚焦窗口，本项目不假装支持。

## 开发

```console
$ make            # 编译
$ make test       # 冒烟测试（无图形会话时自动 skip，退出码 77）
$ ./deskctl --help
```

`wayinput` 的退出码：0 成功，2 用法错误，3 合成器不支持该协议，4 键位查找失败。

协议 XML 放在 `protocol/`，构建时用 `wayland-scanner` 生成客户端代码，不依赖系统里的 wlr-protocols 包。

## 路线图

- [ ] `deskctl serve`：本地 HTTP/JSON 接口，省掉每次 fork 的开销
- [ ] 视觉定位助手：截图 + 模板/文字匹配，直接给出可点坐标
- [ ] `deskctl record`：把一串操作录成可重放的脚本
- [ ] OCR 钩子（tesseract）让 agent 不必依赖多模态模型也能读屏幕
- [ ] X11 下的 XTEST 直连实现，去掉 `xdotool` 依赖

## 目录结构

```
deskctl
├── deskctl                     CLI（Python，无第三方依赖）
├── src/wayinput.c              虚拟键鼠（C，直连 Wayland）
├── protocol/*.xml              两份协议定义，构建时生成代码
├── tests/smoke.sh              真机冒烟测试
├── Makefile
└── README.md
```

## License

MIT
