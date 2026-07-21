# Deterministic GhidraGo + GhidraMCP Bring-up

This is the mandatory runbook for local PSX static analysis. Do not rediscover
this sequence interactively.

## Policy

- **Probe and reuse first.** Port `8080` is the Ghidra plugin; port `8081` is
  the Python MCP bridge. A bridge on `8081` may remain alive while Ghidra is
  closed and should be reused when Ghidra returns.
- **One MCP-owning CodeBrowser only.** Opening a second CodeBrowser loads a
  second `GhidraMCPPlugin`, which fails with `Address already in use` on `8080`.
  If another project/program is needed, close/restart Ghidra first.
- **Use `support/launch.sh ... fg`.** `ghidraRun` uses Ghidra's background
  launcher. Under an agent/unified-exec session that child can disappear and
  leave stale project/GhidraGo locks. The bring-up tool uses foreground launch
  semantics in a detached process group.
- **Do not auto-delete locks.** Locks are removed only with
  `--repair-stale-locks`, and the tool refuses while a Ghidra process exists.
- **Beetle is not required.** Do not restore, build, launch, or require Beetle
  for this workflow unless the user explicitly asks for it. Required evidence
  is the BIOS disassembly, Ghidra MCP, and the native runtime TCP harness.

## Installed paths on this machine

```text
Ghidra:       ~/Tools/ghidra_11.3.2_PUBLIC
GhidraMCP:    ~/Tools/GhidraMCP-release-1-4
Plugin HTTP:  http://127.0.0.1:8080/
Python MCP:   http://127.0.0.1:8081/sse
```

## 1. Status: always run this first

```bash
python3 tools/ghidra_mcp_bringup.py status
```

Interpretation:

- `ghidra_http.healthy=true`, `mcp_bridge_port_open=true`: reuse both. Do not
  send another GhidraGo URL.
- `ghidra_http.healthy=false`, `mcp_bridge_port_open=true`: Ghidra is closed but
  the previous Python bridge is still open. Start Ghidra; reuse `8081`.
- both false: start both with the command below.

The generic MCP resource list may be empty because this bridge exposes **tools**,
not MCP resources. The health check must call a tool (`list_methods`), not use
resource enumeration as the reachability test.

## 2. Create/reuse a verified PS-X project

### Game PS-X EXE

```bash
python3 tools/ghidra_psx_import.py \
  '/path/to/GAME.EXE' \
  --kind exe \
  --workspace /private/tmp/psxrecomp-game-ghidra \
  --project-name PSX_GAME
```

The tool validates the `PS-X EXE` header, strips exactly `0x800` bytes, imports
at the header load address as `MIPS:LE:32:default`, seeds the header entry, and
writes `proof/ghidra_analysis.json`.

### SCPH1001 BIOS

```bash
python3 tools/ghidra_psx_import.py \
  branding/SCPH1001.BIN \
  --kind bios \
  --workspace /private/tmp/psxrecomp-bios-ghidra \
  --project-name PSX_SCPH1001 \
  --seed 0xBFC10A40 \
  --seed 0xBFC11B18 \
  --seed 0xBFC11BD4
```

Use `--reuse` only when the existing `.gpr` is the intended project and its
input bytes have not changed.

## 3. Start GhidraGo and MCP

Use the `gpr`, program name, and URL/address printed by the importer:

```bash
python3 tools/ghidra_mcp_bringup.py start \
  --gpr /private/tmp/psxrecomp-game-ghidra/ghidra/PSX_GAME.gpr \
  --program GAME.EXE_no_header.bin \
  --address 0x80101008
```

What the tool does:

1. Probes `8080`; if healthy, it reuses it and refuses to open another
   CodeBrowser.
2. If Ghidra is absent, starts it with:

   ```bash
   ~/Tools/ghidra_11.3.2_PUBLIC/support/launch.sh \
     fg jdk Ghidra '' '' ghidra.GhidraRun /path/project.gpr
   ```

3. Waits for GhidraGo's `listenerReadyLock`.
4. Sends one local-project Ghidra URL through:

   ```bash
   ~/Tools/ghidra_11.3.2_PUBLIC/support/GhidraGo/ghidraGo \
     'ghidra:/path/project?/program#ADDRESS'
   ```

5. Waits for the Ghidra plugin on `8080`.
6. Reuses an existing bridge on `8081`, or starts:

   ```bash
   ~/Tools/GhidraMCP-release-1-4/.venv/bin/python \
     ~/Tools/GhidraMCP-release-1-4/bridge_mcp_ghidra.py \
     --ghidra-server http://127.0.0.1:8080/ \
     --transport sse --mcp-host 127.0.0.1 --mcp-port 8081
   ```

7. Performs a real Python MCP tool call to prove end-to-end health.

If no Ghidra process exists but verified-stale locks remain:

```bash
python3 tools/ghidra_mcp_bringup.py start ... --repair-stale-locks
```

Never use that flag while Ghidra is running.

## 4. Query through Python MCP

```bash
python3 tools/ghidra_mcp_client.py health
python3 tools/ghidra_mcp_client.py current
python3 tools/ghidra_mcp_client.py function 0x80101008
python3 tools/ghidra_mcp_client.py decompile 0x80101008 --out /private/tmp/entry.c
python3 tools/ghidra_mcp_client.py disassemble 0x80101008
python3 tools/ghidra_mcp_client.py call get_xrefs_to address=0x80101008 limit=100
```

Batch decompilation:

```bash
python3 tools/ghidra_mcp_client.py batch \
  --operation decompile \
  --address 0x80101008 \
  --address 0x80102000 \
  --out-dir /private/tmp/ghidra-decompile
```

The client automatically re-executes with the bridge virtualenv when the system
Python lacks the `mcp` package.

## 5. Native runtime evidence tools

Production Release exports have `PSX_DEBUG_TOOLS=OFF`; they do not call
`debug_server_init`. Build an exact TCP-enabled diagnostic copy without
modifying the app:

```bash
python3 tools/build_diagnostic_export.py \
  --app '/path/Game.app' \
  --workspace /private/tmp/psxrecomp-game-debug \
  --debug-port 4470
```

Run the generated command printed by the tool, then use arbitrary TCP commands:

```bash
python3 tools/runtime_batch.py --port 4470 call ping
python3 tools/runtime_batch.py --port 4470 call gpu_state
python3 tools/runtime_batch.py --port 4470 call cdrom_command_history count=64
```

Batch file example:

```json
{
  "requests": [
    {"cmd": "frame"},
    {"cmd": "get_registers"},
    {"cmd": "gpu_state"},
    {"cmd": "irq_state"},
    {"cmd": "cdrom_state"},
    {"cmd": "freeze_check", "window": 1024}
  ]
}
```

```bash
python3 tools/runtime_batch.py --port 4470 batch \
  --file /private/tmp/requests.json \
  --output /private/tmp/runtime-evidence.json
```

For TCB/RFE interrupt-loss failures:

```bash
python3 tools/thread_sr_audit.py --port 4470 \
  --output /private/tmp/thread-sr-audit.json
```

The audit correlates `thread_trace`, `thread_ctx_ring`, `irqctx_ring`, GP1
history, current IRQ state, GPU state, and CD state. It flags TCB saved SR values
that are not RFE-ready (current KU/IE bits still in bits 0-1) and shows whether
interrupt delivery stops at the same transition.

## 6. Manual failure checks

- `8081` open but MCP calls report `Request failed`: bridge is alive, Ghidra
  `8080` is absent. Start Ghidra; do not start another bridge.
- `8080 Address already in use` in Ghidra: more than one CodeBrowser loaded the
  plugin. Close Ghidra completely and restart one project/program.
- GhidraGo waits forever: launch the `.gpr` first with `launch.sh ... fg`; do
  not use the `ghidraRun` background wrapper.
- Project `.lock` with no Ghidra process: review it, then use the explicit repair
  flag.
- Runtime screenshot says `display disabled`: this is evidence, not permission
  to skip visual verification. Inspect GP1 history and fix the upstream guest/
  runtime condition that left display disabled.
