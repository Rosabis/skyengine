import { afterEach, describe, it } from "vitest";
import { SkyEngineE2e, SkyEngineWorkspace } from "../engine-e2e.js";

describe("gsht", () => {
  let engine: SkyEngineE2e | undefined;
  let ws: SkyEngineWorkspace | undefined;

  afterEach(async () => {
    await engine?.close();
    engine = undefined;
    await ws?.dispose();
    ws = undefined;
  });

  it("应用正常启动", async () => {
    // 每个用例使用独立的 mythroad 数据副本,避免并发执行时互相覆盖插件/缓存/存档。
    ws = await SkyEngineWorkspace.create();
    
    // gsht
    engine = await SkyEngineE2e.start("test/fixtures/gsht_v1015.mrp", { workDir: ws.dir, memory: '2M' });

    await engine.waitForColorInRect(
      [248, 252, 248],
      { x: 0, y: 294, width: 240, height: 26 },
      { name: "bgm-select", timeoutMs: 30_000, intervalMs: 1_000, minCount: 5 },
    );
  });
  
});
