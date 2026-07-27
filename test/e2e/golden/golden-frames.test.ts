import { afterEach, describe, expect, it, vi } from "vitest";
import { SkyEngineE2e, SkyEngineWorkspace, readPpm } from "../engine-e2e.js";

/**
 * P5.3 golden 关键帧回归(见 docs/arm-ext-executor-refactor-plan.md 第八节):
 * 现有用例只抽点像素,渲染管线的"未预期改动"(调色/行距/覆盖区错位)可能
 * 恰好避开采样点。本用例对若干静态等待帧做全图 diff==0 比对。
 *
 * golden 资产生成方法(test/fixtures/golden/):同一画面间隔 2s 双采样,
 * 逐字节一致(排除动画帧)且跨进程重启可复现才入选。渲染行为的有意变更
 * 需要用同一方法重新生成资产并在提交说明中写明原因。
 */
describe("golden 关键帧", () => {
  let engine: SkyEngineE2e | undefined;
  let ws: SkyEngineWorkspace | undefined;

  afterEach(async () => {
    await engine?.close();
    engine = undefined;
    await ws?.dispose();
    ws = undefined;
  });

  const cases = [
    // 系统字体字形不具备跨平台逐像素稳定性，只豁免明确的文本矩形；
    // 对话框布局、背景、图像和其他所有区域继续要求与 golden 完全一致。
    // wbrw-home 曾入选后被剔除:主页含时钟且内容跨运行可变(实测一轮差
    // 2404 像素),不满足"跨进程可复现"资格。
    {
      name: "gzwdzjs-bgm-dialog",
      mrp: "test/fixtures/gzwdzjs.mrp",
      bootMs: 5_000,
      allowedFontRects: [
        { x: 80, y: 152, width: 72, height: 16 },
        { x: 0, y: 296, width: 240, height: 16 },
      ],
    },
    {
      name: "gxdzc-boot",
      mrp: "test/fixtures/gxdzc.mrp",
      bootMs: 5_000,
      allowedFontRects: [],
    },
  ] as const;

  for (const { name, mrp, bootMs, allowedFontRects } of cases) {
    it(`${name} 非字体区域与 golden 一致`, async () => {
      ws = await SkyEngineWorkspace.create();
      engine = await SkyEngineE2e.start(mrp, { workDir: ws.dir });
      await engine.delay(bootMs);
      const golden = await readPpm(`test/fixtures/golden/${name}.ppm`);
      // 画面为无限等待输入的静态帧:到达晚不影响判定,waitFor 只吸收
      // 启动速度抖动,判据始终是全图零差异。
      await vi.waitFor(async () => {
        if (!engine) throw new Error("engine is undefined");
        const screen = await engine.screen(name);
        const totalDiff = screen.diffPixelCount(golden);
        const allowedFontDiff = allowedFontRects.reduce(
          (count, rect) => count + screen.diffPixelCount(golden, rect),
          0,
        );
        expect(totalDiff - allowedFontDiff).toBe(0);
      }, { timeout: 20_000, interval: 1_500 });
    });
  }
});
