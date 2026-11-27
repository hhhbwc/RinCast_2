# RinCast 动态注视点串流三阶段深度重构交接文档
**Handoff Documentation: RinCast Dynamic Foveated Streaming (3-Stage Overhaul)**
*生成时间: 2026-11-28 | 仓库版本: RinCast 2.0.0 | 目标受众: 后续接手 AI / 资深图形与串流工程师*

---

## 1. 仓库与版本关键元数据

- **GitHub 远端仓库**：`https://github.com/hhhbwc/RinCast_2.git`（Git Remote 名为 `new_origin`，**严禁向旧 `origin` 推送**）
- **基线分支**：`main`
- **核心 Commit 记录**：
  - `bccf82b` - `feat(vr): 落地动态注视点三阶段优化（One-Euro+施密特滞回防抖、NVENC动态QP Map双轨、Unwarp+GSR单Pass融合）`
  - `79a4c6f` - `chore(tools): update AI review package generator with deep questions and foveation architecture`
  - `7fca6ed` - `feat(vr): 开放手柄死区上限至100%并支持注视点清晰框尺寸与周边模糊强度动态调节`
- **本地工作区绝对路径**：`C:\Users\wzy\.zcode\workspace\draft-f4c11c48-5ad3-4301-ac42-79f775847197\RinCast`
- **构建环境基准**：
  - PC 端：Windows 11 x64, Visual Studio 2022 (MSVC 19.4x / C++20), CMake 3.28+, DirectX 11 / NVENC SDK
  - 头显端：Android NDK r25+, Gradle 8.13, JDK 17 (Adoptium Hotspot), OpenGL ES 3.2
  - 测试套件：Python 3.10+, Node.js 18+ (ESM 模块规范)

---

## 2. 核心架构与 3 阶段演进实现详解

本次重构全面解决了加装式外置眼动（如 PaperTracker 红外摄像头）在高频抖动、编码宏块横切、双轨码率分配与头显端超分伪影四大维度的深层工程瓶颈。

### 阶段一：PC 端眼动抗噪、量纲校正与跟手度（One-Euro + 22px 施密特滞回 + Saccade Bypass）
- **核心文件**：
  - `pc/streamer/src/stream/vr_stream.h` (`OneEuroFilter2D`, `m_alignedEncPx`)
  - `pc/streamer/src/stream/vr_stream.cpp` (`VrStream::PlaceFovea`)
- **关键问题与修复**：
  1. **归一化 UV 空间滤波量纲校正**：
     - **病根**：全画幅 UV 空间范围为 $[0, 1]$（约对应 100° FOV），快速扫视（Saccade）速度约为 2.5 ~ 3.0 UV/s。原代码参数 `beta = 0.5` 严重偏小，导致快速眼跳时截止频率只能升至 ~2.5 Hz，引入了近 60 ms 的滤波相位滞后，体感严重慢半拍。
     - **重构**：调整为 UV 空间适配参数：基线截止频率 $f_{c\min} = 0.7\text{ Hz}$、动态斜率 $\beta = 12.0$、导数截止频率 $d_{\text{cutoff}} = 1.5\text{ Hz}$。在 2.5 UV/s 扫视下截止频率拉升至 >30 Hz，滤波滞后压缩到 <5 ms。
  2. **22px 施密特触发器滞回（Schmitt Trigger Hysteresis）**：
     - **病根**：原代码按 32px 宏块网格裸做 `std::round(px / 32.0f) * 32.0f`，在 16px 中线处即使 1 像素噪声也会触发网格在相邻 CTU 间来回跳变，导致外围全屏像素持续漂移，破坏 HEVC/AV1 P 帧参考链并导致码率暴增。
     - **重构**：引入 `m_alignedEncPx[eye]` 状态记忆，只有当当前滤波坐标与已对齐像素差值超过 **22px** 时才跨入下一个 32px 网格；在微动与凝视时彻底锁死。
  3. **高速眼跳直通（Saccade Bypass）**：
     - 当单帧原始位移跳变 $\text{rawJump} > 0.08\text{ UV}$ 或眼动速度 $> 1.5\text{ UV/s}$ 时判定为眼跳爆发期，直接跳过滤波器并无视滞回死区，瞬间对齐最新目标格，眼跳落地后再由 $f_{c\min}$ 稳稳接管。
  4. **丢追踪安全复位**：
     - 丢追踪超过 2 秒执行 200ms smoothstep 回中，完成后执行 `m_alignedEncPx[eye][0] = m_alignedEncPx[eye][1] = -1.f;`，防止重新捕获视线时复用陈旧对齐值。

---

### 阶段二：PC 编码器 NVENC 动态 QP Map 真双轨叠加
- **核心文件**：
  - `pc/streamer/src/encode/encoder.h` (`SetDynamicFoveation`)
  - `pc/streamer/src/encode/nvenc_encoder.cpp` (`SetDynamicFoveation`, `qpDeltaMap`)
  - `pc/streamer/src/stream/vr_stream.cpp` (`RunEncoder`)
- **关键问题与修复**：
  1. **打破静态镜头中心与 Foveator 的互斥**：
     - 过去代码中 `ec.foveated = m_config.foveated && !job.foveated;` 将几何压缩与 QP Map 完全对立，因旧 `SetFoveation` 仅支持静态镜片中心。
     - 现已彻底解禁：`ec.foveated = m_config.foveated || job.foveated;`，开启真双轨叠加。
  2. **压缩帧单目坐标系下的精确映射公式**：
     - 设单眼全图注视点起始为 $a$、全密度窗口占比为 $F$、外围采样密度为 $s$（分母 $D = F + (1 - F)s$），压缩帧单眼 UV 坐标下的高清框起止范围为：
       $$e_{\text{start}} = \frac{a \cdot s}{D}, \quad e_{\text{end}} = \frac{a \cdot s + F}{D}$$
     - NVENC 的 `qpDeltaMap` 按照 32×32 宏块划分（如 3072×1536 画面仅 96×48 = 4,608 字节），**直接在 CPU 遍历计算**，单次耗时 $< 15\ \mu\text{s}$，零 GPU→CPU 回读同步开销。
  3. **2.5 CTU smoothstep 过渡带与码率自适应下发**：
     - 外围到中心使用 2.5 个 CTU 宽度的 smoothstep 进行平滑过渡，消除了高清框边缘明显的宏块接缝。
     - 码率自适应阶梯（根据实时 `appliedKbps`）：
       - $\le 100\text{ Mbps}$：中心 $-2$，外围 $+5$（AV1 同步乘以 4 为 $-8 / +20$），在码率受限时将比特全力倾斜至视线中心；
       - $100 \sim 160\text{ Mbps}$：中心 $-1$，外围 $+3$（AV1 为 $-4 / +12$），温和优化；
       - $> 160\text{ Mbps}$：自动将 Delta-QP 归零，防止暗部渐变产生色彩断层（banding）。

---

### 阶段三：头显端 Unwarp + GSR 超分单 Pass 融合
- **核心文件**：
  - `headset/app/src/main/cpp/gfx/scene.h` (`FusedGsrReady`, `BlitExternalUpscaled`)
  - `headset/app/src/main/cpp/gfx/scene.cpp` (`kBlitExtGsrFs`, `BlitExternalUpscaled`, `DrawKeptUpscaled`)
  - `headset/app/src/main/cpp/app/app.cpp` (`App::LatchVideo`)
- **关键问题与修复**：
  1. **消除双线性拉伸伪影的二级锐化与节省 FBO Pass**：
     - 旧管线为：`KeepVideo`（Fovea Unwarp 反拉伸至中间 FBO） $\rightarrow$ `DrawKeptUpscaled`（GSR 全图超分放大至 Swapchain）。不仅白白浪费 1.5~3ms GPU 时间和大量 Tile Memory 读写，更会把外围 2x~4x 降采样拉伸产生的阶梯模糊误判为边缘进行锐化，引发严重振铃与噪点。
  2. **单 Pass 着色器融合设计（`kBlitExtGsrFs`）**：
     - 在未开启插帧（`!m_settings.frameGeneration && m_scene.FusedGsrReady()`）时，直接调用 `BlitExternalUpscaled` 从 `samplerExternalOES` 直出 Swapchain FBO。
     - 片元着色器内根据当前片元 UV 计算距注视点高清盒 $[a, a + F]$ 的距离，羽化因子 `foveaWeight = smoothstep(0.0, 0.015, min(dIn.x, dIn.y))`：
       - 高清区内：严格 1:1 像素映射，执行 12-tap 类 Lanczos 自适应定向锐化，采样步长使用压缩纹理的 texel 尺寸并严格 clamp 在 $[e_{\text{start}}, e_{\text{end}}]$ 边界内；
       - 外围压缩区：跳过超分逻辑，直接 1-tap 双线性平滑输出；
       - 结合 `ApplyFoveaDebug` 调试绿框与 Y 轴反转（`1.0 - vUv.y`）完全对齐。

---

## 3. 验证与门禁测试清单（100% 全部通过）

在每次代码修改后，务必按以下顺序执行回归测试：

1. **Python / Node 契约测试集**：
   ```powershell
   python tools/test-pc-video-settings.py       # 5/5 OK: 验证视频设置序列化、字段声明与热重启关联
   python tools/test-i18n-integrity.py          # 100% SUCCESS: 7国语言357键完整对齐，字库charset 100%覆盖
   node tools/test-setting-intents.mjs          # 26/26 PASS: 验证前端与Native Bridge设置下发防抖与状态机
   python tools/test-thumbstick-contracts.py    # 17/17 PASS: 手柄死区与校准逻辑验证
   ```
2. **PC 端原生 C++ 单元测试**：
   ```powershell
   .\build\bin\Release\rincast-streamer-tests.exe   # 验证 D3D11 Foveator, NVENC rate control, QP Map, FEC 等
   .\build\bin\Release\rincast-core-tests.exe       # 验证基础加密、数据包与协议
   .\build\bin\Release\rincast-headset-tests.exe    # 验证头显端解码与投影数学
   ```
3. **Android 端 Gradle 编译**：
   ```powershell
   cd headset
   .\gradlew.bat assembleStandardDebug assembleQuest1Debug
   ```

---

## 4. 打包与真机交付规则（核心红线）

必须严格遵守以下规范：

1. **PC 端（EXE）绝对禁止自动运行或安装**：
   - 无论何时，**严禁自动静默执行 Setup.exe / install.ps1，严禁自动覆写本机安装目录，严禁拉起 RinCast Streamer.exe 图形界面**。
   - 编译打包完成后，必须使用纯文本代码块（` ```text `）输出打包出的安装程序完整绝对路径，供用户手动双击安装。
2. **头显端（APK）覆盖安装规则**：
   - 运行 `adb devices -l` 与 `adb mdns services`；
   - 探测到 PICO 4（`A8110` / `PICOA8110`）或 Quest 2 设备后，直接使用 `adb -s <serial> install -r dist\RinCast.apk` 覆盖安装；
   - 未探测到在线设备时，仅输出 APK 绝对路径，不得报错中断。
3. **最新成品输出格式示例**：
   ```text
   C:\Users\wzy\.zcode\workspace\draft-f4c11c48-5ad3-4301-ac42-79f775847197\RinCast\dist\RinCast-2.0.0-Setup.exe
   C:\Users\wzy\.zcode\workspace\draft-f4c11c48-5ad3-4301-ac42-79f775847197\RinCast\dist\RinCast.apk
   ```

---

## 5. 关键工程踩坑点与排障备忘

1. **`EncodeJob` 结构体成员命名**：
   - 在 `pc/streamer/src/stream/vr_stream.h:128` 中，压缩后尺寸的字段名为 `job.width` 和 `job.height`，**不是** `job.encWidth` / `job.encHeight`。引用时请勿写错。
2. **`Settings::ToJson` 中的 0 自动档陷阱**：
   - `foveaSize = 0.0f` 和 `foveaPeriphery = 0.0f` 代表“自适应/自动”。`ToJson()` 中必须原样持久化 `0.0f`，切忌使用三元运算符将其写死为 `0.40f`，否则会破坏无眼动用户的固定注视点大视野。
3. **4-Tap 降采样极限与摩尔纹**：
   - `foveator.cpp` 采用 4-tap 双线性降采样，在 $0.25 \sim 0.80$ 区间抗锯齿效果极佳；当低于 $0.25$（如 $0.10$ 对应 10:1 压缩）时采样点覆盖不足，边缘会产生严重的摩尔纹闪烁。因此下限已被 clamp 在 $0.25$。
4. **`m_alignedEncPx` 的状态重置**：
   - 在 `vr_stream.cpp:176`，丢追踪超过 2 秒插值回中后，已经将 `m_alignedEncPx[eye][0] = m_alignedEncPx[eye][1] = -1.f;` 纳入重置，确保重新捕获视线时即刻以最新点为基准对齐。

---

## 6. 后续演进建议（Backlog）

1. **AMD (AMF) 显卡动态 QP Map 支持**：
   - 目前 `SetDynamicFoveation` 在 `nvenc_encoder.cpp` 中完整生效；若后续需支持 AMD 显卡，可在 `amf_encoder.cpp` 中参照相同的 4.5KB CTU 映射填入 AMF 的 ROI surface。
2. **开启插帧（Frame Generation）时的 GSR 融合探索**：
   - 当用户开启机内插帧时，由于插帧需要引用历史帧，目前管线走的是 `KeepVideo` $\rightarrow$ `DrawKeptUpscaled` 路径。后续可评估是否将历史帧 FBO 与 GSR 融合为专用 MRT（多渲染目标）着色器。
3. **机内调试准星（Fovea Debug）前端开关打磨**：
   - 目前可通过设置项或机内快捷键开启注视点绿色轮廓框，后续可在 PC WebUI 上增加更醒目的专属开关或一键微调向导。
