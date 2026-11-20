# View3D

面向 Windows 的桌面三维地理信息预览程序。以插件化架构承载多种三维数据源的加载、渲染与交互：3D Tiles、倾斜摄影（OSGB）、通用三维模型、点云、栅格/DEM 与矢量数据，并提供参照系管理、球面（全球）模式、测量与要素属性查看等能力。

- 版本：`0.1.0`
- 平台：Windows x64（渲染后端 Direct3D 11）
- 语言/标准：C++17
- 构建：CMake ≥ 3.25 + Ninja + MSVC（v143 工具集）

---

## 功能特性

- **多数据源导入**
  - 3D Tiles 1.0 / 1.1（Cesium Native）
  - 倾斜摄影 OSGB 瓦片（OpenSceneGraph），支持按 metadata 的 SRS 逐顶点烘焙
  - 通用三维模型（3DS、OBJ 等，assimp / OSG 读取器）
  - 点云（E57 等）
  - 栅格 / DEM（GeoTIFF）与矢量（SHP / KML / GPKG）（GDAL）
- **球面模式**：按 ECEF 烘焙 + 浮动原点渲染，WGS84 椭球底图，联动参照系、测量与导航（默认关闭，按需开启）。
- **参照系管理**：PROJ 内核，QGIS 式三栏浏览器，内置 EPSG 全库，可挑选任意显示参照系并做源参照系重投影。
- **地理能力**：影像底图、坐标定位 + FlyTo。
- **交互工具**：测量、拾取与要素属性表（跨数据源通用）、场景树图层管理。
- **界面**：Qt Widgets + Ribbon（SARibbonBar）+ 可停靠面板（Qt Advanced Docking），全系统双色 SVG 图标，无边框窗口。

---

## 技术栈与依赖

渲染与界面框架之外，数据读取能力按依赖**可选**：缺哪个预编译树，就不编译对应读取器（参照系/倾斜摄影/3D Tiles 等同理）。

| 组件 | 版本 | 用途 |
| --- | --- | --- |
| Qt | 6.8.4 (msvc2022_64) | 界面 / 核心 / SVG / 网络 |
| Ogre-Next | 3.0.0 | 主渲染（Direct3D 11，PBS 反射） |
| OpenSceneGraph | 3.6.5 | 倾斜摄影 OSGB 加载 |
| Cesium Native | 0.62 | 3D Tiles 加载 |
| assimp | — | 通用模型导入 |
| GDAL | 3.12.4 | 栅格 / 矢量读取 |
| PROJ | 9.8.1 | 参照系 / 坐标变换 |
| 点云库 | —（E57） | 点云读取 |
| SARibbonBar | 2.8 | Ribbon 界面 |
| Qt Advanced Docking | 4.0 | 可停靠面板 |
| NSIS | 3.05（便携版） | 生成 Windows 安装包 |

依赖以**预编译树**形式放在 `third_party/`（见下），CMake 通过 `cmake/*Runtime.cmake` 定位；缺失时给出明确诊断或降级。第三方许可见 `third_party/licenses/`。

---

## 架构

程序采用「框架 + 契约 + 插件 + 宿主外壳」的分层插件化架构：

```
App/View3D (宿主 exe)
   └─ 加载 Framework/ApplicationPlugin（主应用插件，外壳）
         └─ 通过 Framework/PluginEngine 装载各 Views/* 插件
               └─ 插件之间只经 RenderContract 的纯接口通信
```

- **`src/Framework/PluginEngine`** — 插件引擎与 UI 抽象。插件容器、服务注册/发布、命令服务、消息中心、Ribbon/停靠/状态栏等接口（`IPlugin`、`IApplication`、`IRibbonPage`、`IDockWindow`…）、布局配置。
- **`src/Framework/ApplicationPlugin`** — 主应用插件（外壳）：停靠布局、主题、插件管理器、日志面板、崩溃诊断。
- **`src/RenderContract`** — 宿主与渲染/场景插件之间的**纯接口契约**（`IRenderService`、`IRenderScene`、`IViewportCamera`、`IGeoReference`、`ICrsLibrary`、`ITilesetLoader`、`IOsbLoader`、`ModelMesh`、`GlobeMath`…），不含实现。
- **`src/Views/*`** — 能力插件：
  - `Renderer`：Ogre-Next 渲染插件
  - `SceneView`：场景树 / 图层契约 / 视口布局
  - `ToolsPlugin`：测量等工具
  - `ModelPlugin`：模型导入（assimp/OSG/Cesium/点云/GDAL 读取器各自可选）
  - `CrsPlugin`：参照系（PROJ），发布 `IGeoReference`
  - `GeoPlugin`：影像底图、定位 + FlyTo
  - `FeaturePlugin`：拾取监听 + 属性表
  - `OsgPlugin`：倾斜摄影（需本地 OSG 预编译树）
  - `TilesetPlugin`：3D Tiles（需本地 Cesium Native 预编译树）
- **`src/App/View3D`** — 宿主可执行文件（WIN32 子系统）。从 `ui_config.xml` 读取 `mainapp` 插件并调用入口，`LogTee` 落日志。

---

## 目录结构

```
E:\work
├─ CMakeLists.txt            # 顶层：框架/契约/插件/宿主一起构建
├─ CMakePresets.json         # 配置/构建/测试预设
├─ build.cmd                 # 一键：配置 + 构建 + 测试（release）
├─ build_framework.cmd       # 仅构建框架库
├─ package.cmd               # 一键：配置 + 构建 + cpack 安装包
├─ cmake/                    # 依赖定位与打包模块
│   ├─ *Runtime.cmake        # Ogre/OSG/Cesium/assimp/点云/地理 运行库定位
│   ├─ AddView3DPlugin.cmake # 插件目标辅助
│   └─ Packaging.cmake       # CPack + NSIS 安装包规则
├─ src/                      # 源码（见「架构」）
├─ tests/                    # math 纯函数回归 / host 冒烟检查 / config 测试配置
├─ third_party/              # 预编译依赖树 + 许可 + 便携 NSIS
└─ out/                      # 构建输出（gitignore）
    ├─ release/              # release 构建目录（bin 为自包含部署）
    └─ installer/            # 生成的安装包
```

---

## 构建

### 先决条件

- Windows x64 + Visual Studio（含 **MSVC v143 工具集 14.44**）。脚本默认调用
  `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat`，
  若安装路径不同请相应修改 `build*.cmd` / `package.cmd`。
- CMake 与 Ninja 取自 VS 开发者环境（`VsDevCmd`）。
- `third_party/` 下的预编译依赖树需就位（Qt、Ogre-Next、OSG、Cesium Native、assimp、geospatial、pointcloud、ui-deps）。缺失的可选依赖会跳过对应插件。

### 预设（`CMakePresets.json`）

| 预设 | 说明 |
| --- | --- |
| `view3d-release` | 完整构建，Ninja，`BUILD_TESTING=ON`，输出 `out/release` |
| `view3d-framework` | 仅框架库（`View3DFrameworkLibraries`），`BUILD_TESTING=OFF` |
| `view3d-ide` | 生成 VS 解决方案（浏览/调试用） |
| `view3d-framework-vs2015` | VS2015 解决方案（仅浏览；Qt6.8 无法用 v140 编译） |

### 一键脚本

```bat
:: 配置 + 构建 + 测试（release）
build.cmd

:: 仅构建框架库
build_framework.cmd
```

等价的手工命令：

```bat
cmake --preset view3d-release
cmake --build --preset view3d-release
ctest --preset view3d-release --output-on-failure
```

构建产物集中在 **`out/release/bin/`**：宿主 exe、各插件 DLL、Qt/Ogre/OSG/GDAL/PROJ 运行库与数据目录（`Hlms`、`proj-data`、`gdal-data`、`osgPlugins-3.6.5`、`platforms`、`imageformats`、`resources`）。该目录是**自包含部署**，数据目录均按可执行文件相对路径定位。

---

## 运行

```bat
out\release\bin\View3D.exe
```

- 界面布局与主应用插件由 `bin/ui_config.xml` 描述；系统级配置在 `bin/system_config.xml`。
- 运行期用户状态（**不随安装包分发**，首次保存时生成）：
  - `crs_library.json` — 用户参照系列 / 收藏
  - `layout_state.ini` — 界面布局状态
- 球面底图 `bin/resources/globe_basemap.jpg`（可用环境变量 `VIEW3D_GLOBE_BASEMAP` 覆盖）。

---

## 测试

CTest 驱动，分两类：

- **纯函数回归**（`tests/math`）：不依赖渲染上下文，如测量数学（`View3DMeasurementMathTests`）。
- **宿主冒烟检查**（`tests/host`）：以 `View3D.exe --smoke-test` 无头运行，按标记判定（如 `VIEW3D_SMOKE_OK` / `VIEW3D_UI_CHECKS_OK`），覆盖主题、Ribbon、场景树、视口布局、图标、插件管理、测量、模型导入、参照系、属性表、倾斜摄影、3D Tiles、球面往返等。

```bat
ctest --preset view3d-release --output-on-failure
```

> 部分用例依赖本机数据集（如 `E:/data/` 下的倾斜摄影 / 3D Tiles / 大场景 3DS），未安装时**自动跳过**，不影响其余用例。日常改动建议只跑相关用例（参照系 → `Startup`/`UiChecks`，渲染 → `Osb*`）。

---

## 打包 Windows 安装包

使用 **CPack + NSIS**，把已部署的 `out/release/bin` 整树打包（规则见 `cmake/Packaging.cmake`）。

```bat
:: 一键：配置 + 构建 + cpack
package.cmd
```

产物：**`out/installer/View3D-0.1.0-win64.exe`**

安装包特性：
- 整树安装，保留相对布局，装到任意目录可运行；LZMA solid 压缩。
- 附带 **VC++ 运行库**（`vc_redist.x64.exe`，自动在 VS Redist 目录探测），安装末尾静默安装后删除引导器——干净机器必需（工程以 `/MD` 链接，bin 无 app-local 运行库）。
- 开始菜单 + 桌面快捷方式、卸载器、中文显示名「View3D 三维预览」（按构建机 ANSI 代码页编译，目标为中文 Windows）。
- **剔除**：调试/链接产物（`*.pdb`/`*.ilk`/`*.exp`/`*.log`）、测试产物（`stage2_probe`、数学测试 exe、测试配置）、运行期用户状态（`crs_library.json`、`layout_state.ini`）。

> 便携 NSIS 位于 `third_party/nsis/`（gitignore，不入库），不在系统 PATH/注册表；`package.cmd` 负责把 `makensis` 注入 PATH 后再调 cpack。

---

## 许可与第三方素材

- 第三方库许可集中在 `third_party/licenses/`（Qt、Ogre-Next、OSG、Cesium Native、assimp、GDAL/PROJ、点云、SARibbonBar、Qt Advanced Docking 等）。
- 球面底图为 NASA Blue Marble 可见地球影像衍生作品（NASA 素材公有领域），经 three.js 示例贴图引入，说明见 `src/App/View3D/resources/README.txt`。