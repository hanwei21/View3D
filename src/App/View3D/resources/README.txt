globe_basemap.jpg —— 球面模式默认底图（等距圆柱投影，2048×1024，2:1）

素材来源：three.js 示例贴图 examples/textures/planets/earth_atmos_2048.jpg
（https://github.com/mrdoob/three.js，MIT 仓库；贴图本身为 NASA Blue Marble
可见地球影像的衍生作品，NASA 素材属公有领域）。

用途：plugin.view3d.renderer 的球面模式把该纹理贴到 WGS84 椭球底图上。
运行时路径：<应用目录>/resources/globe_basemap.jpg；可用环境变量
VIEW3D_GLOBE_BASEMAP 覆盖。文件缺失或尺寸不是 2:1 时回退为纯色球面。

替换要求：等距圆柱投影（左边界西经 180°，右边界东经 180°，顶部北极），
宽高比严格 2:1，JPEG/PNG 均可（按扩展名由 Qt 解码）。
