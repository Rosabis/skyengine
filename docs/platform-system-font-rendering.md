# 平台系统字体渲染改造

## 目标

普通 `mr_drawText`、ARM EXT table[30]/table[145] 和 `mr_textCreate` 平台文本页统一使用宿主系统字体，不再依赖工作目录中的 `system/gb12.uc2`、`system/gb16.uc2`。

## 实现

- `src/platform_font.c` 提供统一的 UCS-2 单字栅格接口，输出逐行紧凑排列的 1bpp 位图。系统字形栅格进 MRP 固定字格（ASCII 8px，中文 12/16px），继续满足 ARM table[30] 的固定 32 字节缓冲契约和应用布局预期。
- Linux 动态调用 Fontconfig，匹配 `sans-serif:lang=zh-cn` 并使用匹配结果的 TTC face index；Android 只遍历只读系统字体目录并选择同时覆盖拉丁文和中文的 face。两者使用 vendored `stb_truetype` 栅格化系统字体文件。
- Windows 读取 `SPI_GETNONCLIENTMETRICS` 的系统消息字体并通过 GDI 栅格化；Wasm 通过浏览器 Canvas 的 `system-ui, sans-serif` 栅格化。
- `DSM_REQUIRE_FUNCS` 追加 `font_get_glyph` 回调。Mythroad 层只消费平台返回的位图和度量，不包含宿主字体库代码，也不再打开/关闭 `gb12.uc2`、`gb16.uc2`。
- `native_text_widget.c` 复用同一字体模块；CMake 删除桌面端基础点阵字库复制目标，Wasm 文件系统预加载列表删除两份点阵文件。

## 兼容性

- `MR_FONT_SMALL` 仍映射为 12px，`MR_FONT_MEDIUM`/`MR_FONT_BIG` 映射为 16px。
- 具体笔画像素会随平台变化，但 MRP 字格宽度、文字换行和控件布局保持旧平台契约。回归测试按标题、正文和软键区域检查文字存在，不再绑定旧点阵字体的单个像素坐标。
- 应用仍可在 `mythroad/system/` 下载或保存自己的文件；改造只移除宿主渲染对基础 `gb12.uc2`/`gb16.uc2` 的依赖。

## 验证记录

- `cmake -S . -B build && cmake --build build --target skyengine -j2`：通过。
- `cmake -S . -B build-shared-only -DSKYENGINE_BUILD_SHARED_ONLY=ON && cmake --build build-shared-only --target skyengine-shared -j2`：通过。
- `pnpm exec tsc --noEmit` 与 `git diff --check`：通过。
- 使用全新空工作目录启动 `test/fixtures/gtdgdq.mrp`：运行进入 guest，工作目录没有基础点阵字库。`test/e2e/gtdgdq/menu.test.ts` 还会在启动前显式删除两份点阵字库，再验证平台帮助页标题、正文、分隔线、软键和关闭后的 guest 帧恢复。
- 全量 `pnpm vitest run test/e2e --reporter=verbose --retry=0` 共 56 个用例：首轮 51 个通过；三个 `gzwdzjs` 旧字形坐标已改成区域断言并完整复跑通过；`gtlbd` 的严格文字保留用例独立复跑通过；`gghjt` 公网下载用例单独复跑可通过，但同一端点也出现过 20 秒内无下载进度的外部网络波动。
- 当前环境没有 `emcc`，未实际构建 `skyengine-wasm`；已完成 Wasm Canvas 后端源码检查和 `wasm/dist/fs.js` 预加载资源检查。Windows、Android 后端也未在本机交叉构建，仍需各平台 CI/设备验证系统字体选择和栅格结果。
