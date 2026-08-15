---
title: "AQCW plugin ID 490044 filename mapping"
tags: ["aqcw", "plugin", "simpleDownload", "reverse-engineering", "xrecom"]
created: 2026-08-14T13:49:23.157Z
updated: 2026-08-14T13:49:23.157Z
sources: []
links: ["aqcw-payone-local-payment-progress.md", "aqcw-payone-response-contract.md"]
category: debugging
confidence: high
schemaVersion: 1
---

# AQCW plugin ID 490044 filename mapping

## Conclusion

File ID `490044` (`0x00077A3C`) maps to **`xrecom.mrp`**. Its runtime installation/probe path is **`mythroad/plugins/xrecom.mrp`**. The available repository evidence does not establish a human-readable product name; `xrecom` likely denotes a recommendation component, but that expansion is inference only.

## Static package evidence

The current user-supplied `test/fixtures/aqcw_v1101.mrp` is an MRPG package with appid 391046 and version 1101. Structured index parsing and gzip decoding yields `game.ext` length 95400, SHA-256 `9753d2cd7eaa1c0607034d5a2f679e0e4d4035d2d45c8f7c2cedebf325de81ac`.

The only little-endian `490044` occurrence in decoded `game.ext` is at `+0xD5F4`. The eight-byte `plugins.lst` is unrelated: it decodes as big-endian appid 499999/version 1001 and describes embedded `rmdres.mrp`.

## Disassembly evidence

The Thumb function at `game.ext+0xD470` constructs the exact three-argument probe:

- `+0xD478..+0xD480` resolves `r2` to `game.ext+0x12F58`, string `xrecom.mrp`;
- `+0xD480..+0xD488` derives `r1 = game.ext+0x12F64`, string `plugins`;
- `+0xD484` loads `r6 = 0x00077A3C`;
- `+0xD48A..+0xD48C` calls helper `+0xC914` with `r0=490044`, `r1="plugins"`, `r2="xrecom.mrp"`.

Helper `+0xC914` preserves those arguments, concatenates the directory, separator, and filename, queries appid/path version-like state through MPS selectors 201/202/203, and returns the larger observed value. The caller compares the result with 8: `>=8` skips downloading; `<8` enters the download branch. Because the isolated workspace template contains no `plugins/xrecom.mrp`, this run takes the missing/outdated branch.

The same caller then stores a download descriptor containing `{1, 490044, 0, 0}`, formats `plugins/xrecom.mrp`, and selects `plugins/verdload.ext` as the downloader. Nearby `netpay.mrp` is a separate descriptor and is already present in the workspace.

## Runtime evidence

A focused no-Xvfb run with send-only syscall tracing passed:

`VMRP_E2E_KEEP_TMP=1 strace -f -s 4096 -xx -e trace=sendto pnpm vitest run test/e2e/aqcw/temp.test.ts -t 下载插件 --reporter=verbose --retry=0`

The `/simpleDownload` request body contains TLV tag `0x29CE`, length 4, value `00 07 7A 3C`, proving the transmitted resource ID is 490044. Stdout also proves connection to `spd.skymobiapp.com:6009`, HTTP POST `/simpleDownload`, and HTTP 200. The focused Vitest result was 1 passed, 1 skipped; target duration 14.622 s.

Retained artifacts are `/tmp/skyengine-e2e-AdNpdZ` and `/tmp/skyengine-ws-PEMIqk`. P6 PPM hashes: `main.ppm` `af0a717e55687f831394ddca302cfbf9f51ef38cfd8b6c20bb4661bb658e4f80`; `screen.ppm` `cdc41628b3f7c2d4fe90b0e3a709129322159654f1811eac7bd5d0c7fe15d6b3`; `pay.ppm` `44f67dd9d0899ca4f50912c5875d3786e9214c3efb21e89b39e9539c948383f0`. The pay frame is 240x320 and pixel `(106,145)` is `[248,0,0]`, matching the current test. PPM proves the UI path, not the filename mapping.

## Code and test boundary

No program, test, or fixture code was changed during this investigation. Existing user-owned changes remain in `docs/prompt.md`, `test/e2e/aqcw/temp.test.ts`, and untracked `test/fixtures/aqcw_v1101.mrp`.

The comment in the current test calling this a browser plugin is contradicted by the binary mapping. The test also starts a local pay server but does not pass a `dnsMap`, so that process is not used by the v1101 run. Neither issue changes the ID-to-filename conclusion.

Related older payment work: [[aqcw-payone-local-payment-progress]] and [[aqcw-payone-response-contract]].
