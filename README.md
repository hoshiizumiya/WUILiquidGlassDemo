# WUILiquidGlassDemo

这个仓库是一个 Liquid Glass 风格材质实验项目，目前包含两条实现路线：

- **WinUI 2**：基于 system XAML Islands + C++/WinRT，自行捕获 backdrop、自行跑 D3D11 blur 和 glass shader，最后画进 composition surface。
- **WinUI 3**：基于 WinUI 3 + composition brush，把自定义 shader 接入 `Compositor::CreateEffectFactory` 路径，重点实验 DWM / WUCEffectsI 的 custom effect 编译与 linking 行为。

> [!WARNING]
>
> Vibe coded with GPT-5.4 High.

## WinUI 2

WinUI 2 版本是最直接、最可控的实现。它把玻璃看成一层“光学透射表面”，整条链路由 app 自己驱动：

```text
实时背景 -> 可选 blur 预处理 -> 折射 / 色散采样 -> 高光 / 边框叠加 -> composition surface
```

### 核心链路

1. 捕获玻璃后面的实时内容。
2. 把捕获到的 frame 转成 D3D11 纹理。
3. 在需要时对这张纹理执行独立的高斯模糊预处理。
4. CPU 侧选择最终 transmission texture。
5. glass shader 根据圆角矩形几何计算折射、色散、高光、边框和遮罩。
6. 输出写入 `CompositionDrawingSurface`，再通过 `CompositionSurfaceBrush` 显示。

### 代码结构

- [WinUI2/BackdropImageGenerator.h](./WinUI2/BackdropImageGenerator.h) / [WinUI2/BackdropImageGenerator.cpp](./WinUI2/BackdropImageGenerator.cpp)  
  负责捕获 live backdrop，输出 `Direct3D11CaptureFrame`，并通过 `LastFrame` 暴露最新 frame。

- [WinUI2/AnimationFrameScheduler.h](./WinUI2/AnimationFrameScheduler.h) / [WinUI2/AnimationFrameScheduler.cpp](./WinUI2/AnimationFrameScheduler.cpp)  
  提供类似 requestAnimationFrame 的调度能力，用来合并多次重绘请求。

- [WinUI2/MainPage.xaml](./WinUI2/MainPage.xaml)  
  承载演示 UI、参数 slider，以及自定义渲染用的 composition 宿主。

- [WinUI2/MainPage.h](./WinUI2/MainPage.h) / [WinUI2/MainPage.cpp](./WinUI2/MainPage.cpp)  
  持有 D3D11 资源、blur pass、composition surface，以及最终玻璃材质 shader。

### 实现要点

`BackdropImageGenerator` 接收一个 `Visual`，持续产出 `Direct3D11CaptureFrame`。Demo 使用按需触发的重绘模型，主要 invalidation 来源是 backdrop frame 更新、渲染宿主尺寸变化和 slider 参数变化。

> [!WARNING]
>
> 由于 `Windows.Graphics.Capture` 的限制，无法正确捕获透明的 Visual 或元素的 handoff visual。需要为目标元素设置实心背景和 Visual 中转。

blur 预处理是独立的两段 separable Gaussian blur：

```text
horizontal blur pass -> vertical blur pass
```

blur 强度为 0 时直接使用原始 frame；blur 开启时，最终 glass pass 采样预处理后的 blurred frame。

最终 shader 完成这些材质项：

- rounded-rect 遮罩
- 折射偏移
- 色散采样
- 内部提白
- 边框增强
- inner glow 与高光叠加

rounded rect 在这里不只是 clip mask。它同时驱动折射衰减、色散衰减、边框强调和高光强度。实现上刻意把两类场分开：

- 基于真实 SDF 的边缘距离场，用于 edge-local 项。
- 单独构造的 rounded interior field，用于 refraction / dispersion 的中心衰减。

这样做是为了保住边缘项语义，同时避免 SDF medial axis 附近的中心断层。

### WinUI 2 的特点

- 优点：链路完全可控，shader 输入、intermediate texture、blur 尺寸和采样坐标都由 app 管理。
- 缺点：需要自己捕获和调度 backdrop，需要维护 D3D11 surface 生命周期，和系统 composition effect 管线结合较浅。

## WinUI 3

> [!WARNING]
>
> WinUI 3 部分包含大量逆向和 hook 实验，属于研究代码，不是稳定 API 用法。基于 WinAppSDK v2.2.0 x64 测试。

WinUI 3 版本不是 WinUI 2 的简单移植。得益于 WinUI 3 的 Lifted Compositor 架构，此版本可以用于验证基于 hook 的自定义 effect shader 路线：**尽量不在 app 侧自绘 intermediate surface，而是把自定义 effect 伪装成 composition 可接受的 effect graph，并让 Windows App SDK 随附的 WUCEffectsI / DWM 编译和执行它。这里的 DWM 指 Lifted Compositor 中的 `dwmcorei.dll`，不是桌面会话中的系统 `dwm.exe`。**

当前 UI 使用 XAML `Border.Background` 挂载自定义 `XamlCompositionBrushBase`，再把 `CompositionBrush` 设置为普通 solid / gradient / blur / invert / liquid glass effect brush。演示面板、动态 XAML 元素、背景图片和可拖拽 resize 的 glass rect 都用于观察 backdrop sampling 行为。

### 和 WinUI 2 的关键差异

WinUI 2：

```text
app capture -> app blur -> app glass shader -> app-owned CompositionDrawingSurface
```

WinUI 3：

```text
CompositionBackdropBrush / effect source -> WUCEffectsI graph traversal -> DWM shader linking -> CompositionEffectBrush
```

也就是说，WinUI 3 的目标不是“自己画完一张 texture 再交给 XAML”，而是让 `Compositor::CreateEffectFactory` 接受一个自定义 effect description，并把 shader body 编进 composition effect pipeline。

### Composition hook

WinUI 3 的核心实验代码在：

- [WinUI3/CustomEffectRuntime.h](./WinUI3/CustomEffectRuntime.h)
- [WinUI3/CustomEffectRuntime.cpp](./WinUI3/CustomEffectRuntime.cpp)

它做的事情包括：

- 注册自定义 effect 定义，包括 effect GUID、source、property、constant buffer、shader body 和 shader linking 参数。
- 提供可传给 `CreateEffectFactory` 的 `IGraphicsEffect` / `IGraphicsEffectSource` 对象。
- 在运行时参与或绕过 WUCEffectsI / DWM 对 effect type、compiled effect、subgraph、constant buffer 和 shader linking body 的常规假设。
- 让自定义 shader 能以 DWM custom sampler 的形态拿到 `uv`、`samplerDataExt` 和 `samplerData`。

这里的“hook”不是为了把自定义逻辑绕回 app 自绘路径，也不是在 hook 系统 `dwm.exe`。它是为了让 WinUI 3 的 `CompositionEffectBrush` 继续走 Windows App SDK Lifted Compositor 的 brush 路径，同时让未公开的 custom effect 形状被接受。

完整的 WUCEffectsI graph traversal、DWM rendering graph、technique/fragment、shader linking ABI、custom sampler 和纹理输入限制见：

- [docs/dwm-shader-linking.md](./docs/dwm-shader-linking.md)

### WinUI 3 effect 文件

- [WinUI3/CustomInvertEffect.cpp](./WinUI3/CustomInvertEffect.cpp)  
  最小自定义 effect 例子，用于验证自定义 shader 是否确实被执行。

- [WinUI3/CustomBlurEffect.cpp](./WinUI3/CustomBlurEffect.cpp)  
  丐版 blur 实验，用于验证 custom shader 采样、尺寸和 sampler data 行为。

- [WinUI3/GaussianBlurEffect.cpp](./WinUI3/GaussianBlurEffect.cpp)  
  构造系统 GaussianBlur effect description，用于和自定义 effect 串联。

- [WinUI3/CustomLiquidGlassEffect.cpp](./WinUI3/CustomLiquidGlassEffect.cpp)  
  将 WinUI 2 的 glass material 移植到 DWM custom sampler 模型。它不再依赖 app 自己传入宽高，而是使用 DWM 提供的 `samplerData` / `samplerDataExt` 和 shader derivative 推导材质坐标。

- [WinUI3/MainWindow.xaml](./WinUI3/MainWindow.xaml) / [WinUI3/MainWindow.xaml.cpp](./WinUI3/MainWindow.xaml.cpp)  
  演示 UI。支持切换 solid / gradient / blur / invert / liquid glass brush，调整 liquid glass 参数，拖拽 resize glass rect，设置或拖入背景图片。

### Liquid Glass 在 WinUI 3 中的状态

当前 WinUI 3 liquid glass 路线已经能够：

- 通过 `CompositionEffectBrush` 显示自定义 glass shader。
- 串接一个系统 GaussianBlur brush 作为上游 backdrop source。
- 使用 animatable properties 控制 blur radius、refraction、corner radius、border thickness、highlight 和 dispersion。
- 通过 `samplerData` 保持对 DWM effective content rect 的感知，减少上游 blur padding 导致的坐标漂移。

但它仍然不是 WinUI 2 路线的等价替代：

- DWM/WUCEffectsI 的 effect graph 有复杂度和形状限制。
- `CompositionEffectBrush` 的 shader body 不是普通 full-screen pixel shader；可用输入取决于 DWM shader linking ABI。
- 自定义坐标系、intermediate texture 尺寸、GaussianBlur 的 downsample/padding 行为都不能完全由 app 控制。
- 当前 shader 内部 blur 仍是 placeholder，代码里保留了 TODO：后续应实现真正的 Dual Kawase blur 或其它稳定多 pass blur，而不是复用 DWM GaussianBlur 的已缩放 intermediate。

### 为什么 WinUI 3 更复杂

WinUI 2 的 shader 拿到的是 app 明确准备好的 texture 和 rect。WinUI 3 的 shader 运行在 composition effect graph 里，输入是 DWM 链接出来的 sampler body。很多看似普通的事情都会变成 graph/linking 问题：

- 一个 effect 是否能被 `CreateEffectFactory` 接受。
- graph 是否能 flatten 成 DWM 能消费的 subgraph。
- shader body 使用的是颜色输入、custom sampler 输入，还是外部实现的 native graph。
- 上游 blur 是否改变 effective content rect。
- property metadata 和 constant buffer layout 是否匹配。

因此 WinUI 3 代码里有较多“看起来不像正常 app 代码”的 hook 和 runtime metadata。它们是为了让自定义 effect 进入 composition pipeline，而不是为了追求通用框架抽象。

## 构建

解决方案入口：

```powershell
WUILiquidGlassDemo.sln
```

WinUI 3 构建示例：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  'WUILiquidGlassDemo.sln' `
  /t:WUILiquidGlassDemo_WUI3 `
  /p:Configuration=Debug `
  /p:Platform=x64 `
  /m
```

## 当前方向

- WinUI 2 继续作为可控的 reference implementation。
- WinUI 3 继续探索 composition hook、自定义 effect graph、custom sampler、property metadata 和 DWM shader linking 行为。
- liquid glass 目标保持一致：实时背景、稳定 blur、折射/色散、高光/边框和 rounded-rect material mask。
