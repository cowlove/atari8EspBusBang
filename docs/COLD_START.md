# Cold-starting the Atari from main: the sled, the stub and the parks

How main takes over a 6502 it finds in any state (off, powering on, running its OS natively, or parked by a previous main) and cold-starts it onto main's memory map, reliably, on every machine, with no halt or reset line. Worked out on the miner6 pool and in csim, 2026-11-26 to 2026-11-28. Code: `main/main.cpp` (`sledPrepare()`, `sledRun()`, `sledLandingReport()`, the loop test `sledLoop()`), `main/pbirom.asm` (`SAFE_WAIT`), `main/pbi.cpp` (the stack wait's release), `bootloader_components/a8bb_boot_pins/hooks.c` (the reset-time bus sample). AGENTS.md has the short version; this is the long one.

## The problem

When core 1 starts serving the bus, the 6502 suddenly sees main's memory instead of its own: zeroed base RAM, a cartridge, PBI ROM. A 6502 that was running anything at that moment, its OS natively, or code in main's memory from before main rebooted, executes garbage and crashes. Before this work the only answers were timing (boot.sh powered the Atari on only after main's `BOOT COMPLETE`) and the halt line (holding the 6502 while main booted). Neither is enough:

- **No reset.** The PBI/ECI RST pin is buffered and output-only: neither board can reset the Atari (the Sidecar's `atariReset` and the carrier's RESET jumper do nothing).
- **No usable halt on a 130XE.** The ECI pin wired as "halt" is ANTIC's HALT output, not the 6502's RDY; pulling it low under a running 130XE crashed it (the pool, 2026-11-26). The halt line is gone from startup on every machine.
- **Real cases need it:** main rebooting under a running Atari (Boot now, an OTA, the link watchdog, a crash), an Atari powered on before main is up (the real world: both come up together), main reset by its own USB.

## The design

Core 1 starts on the **transparent map** (`mmuStateDisabled`): it drives nothing, so native memory answers every cycle and the 6502 keeps doing whatever it was doing. Core 1 still logs every bus cycle (but refresh) into the bmon rings. Then, at `core0Loop()`'s start, hot (interrupts masked; see "Why it runs hot"):

### No clock: the real map at once

If the bmon heads don't move for 10 ms, phi2 isn't running: the Atari is off. Main puts its real map in (`enableBus()`) and the 6502 boots onto it when the Atari powers on: the ordinary power-on, no sled. The map goes in microseconds after the last look; a 6502 whose power came on in between is still in its reset sequence, reading ROM. (Not in the loop test, which waits for a clock.) Log: `SLED: no bus clock ...`.

### A clock: the $4C flood

Main makes `$0000-$7FFF` read `$4C` everywhere (every L1 block and every ext-bank pointer swapped to the flood, a pointer at a time; every state in between is safe), with writes going to dummy RAM (native RAM keeps what it had). `$4C` is `JMP abs`, so:

- **Any instruction fetched there, at any alignment, is `JMP $4C4C`**, and `$4C4C` jumps to itself: the 6502 is caught in a loop main controls.
- **RTS** pops `$4C4C` (+1: `$4C4D`, still `JMP $4C4C`); **RTI** pops PC `$4C4C` (and P `$4C`: I set, D and V too, which the stub's CLD fixes); every RAM vector the OS's interrupt handlers jump through reads `$4C4C`. Pushes go nowhere, so even code running in ROM lands at its first RTS or RTI, or at the next VBI or IRQ.
- **Page 0 is a scratch page** (read and written, first flooded too): a loop counting in zero page then ends as it would (on a read-only flood it would read `$4C` forever), so code that never returns still reaches its CLI or NMI enable and lands.
- **Page 6 is main's real base RAM**, holding the stub (below); `$8000` up stays transparent (the OS ROM, I/O, whatever was there), with no halting pages.
- **Escapes:** ROM or cartridge code spinning without a return with NMIs and IRQs off. AltirraOS's reset-time init (NMIs off) lands when it ends, ~160 ms after power-on; the XL OS's init is untested.

**The catch:** 24 reads in a row at `$4C4C-$4C4E` in the read ring (three per JMP), within 3 s of clocked time (only time with phi2 running counts: an Atari still powering up isn't charged), 30 s at most.

**A parked 6502 is caught at once** (0 ms): a loop in native RAM below `$8000` (`$0104`, `$3000`, below) reads the flood at its next fetch. So "already parked" needs no case of its own; the report tells it apart from "running" from the reads before the flood: `SLED: before the flood the 6502 was parked at $0104 (the safe reboot's stack wait)` / `parked at $3000 (the loop test's park)` / `running (not in a park loop)`.

### To the stub, a byte at a time

The caught 6502 loops on `JMP $4C4C`. Main rewrites the JMP's operand in the flood page, low byte first: `$4C4C` → `$4C00` (still the flood: back to the loop) → `$0600`. Every intermediate state jumps somewhere safe.

### The stub at `$0600`

```
0600  SEI
0601  CLD
0602  LDA #0 / STA NMIEN      ; no more VBIs or DLIs
0607  LDA #$A5 / STA $06FF    ; the mark: main sees "in the stub"
060C  LDA $06FE / BEQ $060C   ; spin until main sets the go byte
0611  CMP #2 / BEQ park       ; go 2: park (the loop test)
0615  LDA #$38 / STA PBCTL    ; $D301 is DDRB
061A  LDA #$FF / STA DDRB     ; all port B lines outputs
061F  LDA #$3C / STA PBCTL    ; $D301 is PORTB
0624  LDA #$FF / STA PORTB    ; OS ROM in, BASIC and self test out
0629  JMP ($FFFC)             ; the reset handler: a cold start
park: LDA #$B0 / STA $3000 / LDA #$FE / STA $3001 / SEC / JMP $3000   ; BCS * at $3000
```

Once main sees the mark it clears PUPBT1 (`$033D`) and sets COLDST (`$0244`), puts the real map in (page 6 is the same memory in both maps), and `core0Loop()` sets the go byte right after its backlog discard; the stub's PORTB write and the OS's cold start are then the first writes it waits for.

**Why the PIA setup:** a 6502 caught soon after its reset finds the PIA with PBCTL 0, where `$D301` is the data-direction register and the output register is `$00`. A bare `STA PORTB` there made every port B line an output driving `$00`: the OS ROM switched out, `JMP ($FFFC)` read the RAM under it (zeros), a BRK storm at `$0000` (page 1 full of BRK frames). That was the pool's failing "caught after 0 ms" first boots. The stub now sets the PIA up as the OS does. csim never showed it: atari800's PIA resets PBCTL to `$3F`, not the hardware's 0.

**Always a cold start:** the PBI driver's init (`PBI_INIT`) clears PUPBT1 too, so the OS never warm-starts onto state that didn't survive (main's PORTB/`$D1FF` copies, SDX's banks).

## The parks: a 6502 waiting in its own RAM

A main that is about to go away first parks the 6502 in a loop in native RAM below `$8000`, where nothing of main's is needed, interrupts and NMIs off. Main can then reset under it, and the next boot's flood catches it at once.

- **The safe reboot's stack wait, `$0104`** (`SAFE_WAIT`, `pbirom.asm`; every detach-safe PBI call uses it). The PBI handler copies to `$0100`: `STA ESP32_IOCB_REQ,y` (issuing the request), `SEC`, `BCS *` at `$0104`, `RTS`. During a request main detaches the bus and the 6502 spins natively in page 1; for a reboot (`reboot` on the Sidecar's console, Boot now, `update`) main's cold handler restarts main instead of returning (with a Sidecar after a RestartingSelf notice; until 2026-11-28 it sent RebootReady and the Sidecar reset main). Normally main ends the wait by writing an RTS over the BCS (`handlePbiRequestHot()`), after seeing 4 fetches of `$0104` in the read ring (counted from after the request; none in 1 s: `-3 stackprog timeout`, a wedged 6502). The loop polls nothing (it used to read `ESP32_IOCB_REQ`, which with main gone read the math pack ROM at `$D8xx`) and writes nothing.
- **The loop test's park, `$3000`** (the stub's go 2): `BCS *` written at `$3000`, which is transparent in the test's empty-RAM map, so it's native RAM on every machine.

### Why `BCS *`: the D5 glitch

GPIO43 is D5 and also U0TXD. Through main's reset the ESP32's ROM sets up UART0 (clearing that pad's hold) and the idle TX line drives D5 high, for the ROM's part of the boot. On the 800XLs the ESP32 wins against the DRAM: a 6502 parked at `JMP $3000` (`$4C $00 $30`) left its loop within ~10 ms of every main restart, presumably fetching `JMP ($3020)` (`$4C` → `$6C`). The 130XEs and the 600XL never minded (their RAM outdrives the pin, presumably). Found with the Sidecar's REBOOT_TIMELINE (the first write after the restart showed the moment the 6502 left the park) and the bootloader's bus sample ("park loop not seen"). Holding every other pad through the reset (RTC pad hold, `-DRESTART_PAD_HOLD=1`) changed nothing; the ROM clears GPIO43/44's hold itself.

So the parks' bytes all have bit 5 set: `BCS *` is `$B0 $FE`, with carry set first; a taken branch's dummy read of the next byte is discarded. With it the park survived every restart on both 800XLs (17/17, then every soak since). The old stack wait (`STA DUMMY_WRITE` / `LDA ESP32_IOCB_REQ,y` / `BNE`: `$8D`→`$AD`, `$D8`→`$F8`, `$D0`→`$F0` under D5) had the same weakness. **Anything the 6502 runs while main resets must tolerate D5 high.** The real fix is hardware: D5 off GPIO43, or a series resistor or buffer that keeps it off the bus at reset.

## Everything else it took

Each of these was a real failure on the pool:

- **Pins quiet in `setup()`.** The data pins were `pinMode(OUTPUT)`: main drove D0-D7 from setup until core 1's first cycle. Now inputs (core 1 enables them per cycle it serves). Open-drain lines (extSel_, MPD, halt_) were enabled with their level still 0, pulling low for microseconds: now released first (`pinOpenDrainIdle()`). No startup halt. No status LED on board version 1 (GPIO48 is R1 to the Atari's RESET there).
- **The sled runs hot.** It first ran in `threadFunc()`'s start with interrupts on; the 6502 spun in main's page 6 through the banner's flash code and LOGs, and a bus cycle core 1 missed was a native-RAM fetch: a third of the boots that reached the stub died in a BRK storm. Once the flood is live the 6502 depends on main, so `sledRun()` is the first thing in `core0Loop()`, interrupts masked, memory and `mmuState` only (no LOG, flash or peripherals), its results printed cold later. It lives in RTC fast memory (`RTC_IRAM_ATTR`): only core 0 executes there, so its spin (up to 30 s) shares no SRAM bank with core 1 (see AGENTS.md's "The hot bus").
- **The stub is let go from `core0Loop()`**, after its backlog discard, with bmon kept up: not at the end of the landing, ahead of tens of ms of banner LOGs with nothing draining bmon.
- **`esp_restart()` left core 1 running.** In this single-core build it resets CPU 0 only; IDF stalls core 1 only in the new app's startup, after the ROM and the bootloader have loaded the image over IRAM and DRAM. Until then core 1 ran `iloop_pbi` with the bus pads live, on memory being rewritten under it: the parked 6502 was wedged (the standalone machines failed every soak reboot). `rebootRestart()` calls `core1StopForRestart()` first: `disableBus()`, core 1 stalled, held in reset, its clock gated, every bus pad an input. An EN reset (the Sidecar's) resets the whole chip. Any other `esp_restart()` under a running Atari (an OTA's) needs the same.
- **A volatile read.** `NOT IN THE STUB` on every hardware boot was the check, not the stub: `atariRam` isn't volatile and the wait's read was hoisted.
- **The landing wait counts only clocked time**, so an Atari still powering up isn't a timeout.
- **`BOOT COMPLETE` comes early** (in setup(), before core 1), so boot.sh powers the Atari on during main's boot: every order is safe now.

## Diagnostics

- **The sled's report** (`sledLandingReport()`, cold: the exit report and the safe reboot's handler; a `LOGHOT()` line at once with a Sidecar): the case (no clock / parked / running), the last 16 reads and 8 writes core 1 logged before the flood, at the catch and if the stub's mark didn't come, how long the catch and the stub took.
- **The bootloader's bus sample** (`hooks.c`, first thing in the second-stage bootloader, during main's reset): a few ms of the bus with the sampled pads' inputs enabled, decoded for both board pin maps: phi2 ran, a park loop seen (`$3000-$3002` or `$0104-$0106`), any write or GPIO48 low. Main reports it (`the bus then: ...`). It ships in the bootloader, so only USB-flashed boards have the current one.
- **REBOOT_TIMELINE** (the Sidecar's sampler, with a Sidecar that samples the bus: .3): from main's `RestartingSelf` (200 ms before it restarts) or the Sidecar's own reset, 2 s of 5 ms buckets: phi2 falls, writes, extSel_ cycles, and each bucket's first write. A parked `BCS *` writes nothing, so the first write marks the moment the 6502 left the park.
- **Opt-in:** `-DBOOT_BUS_PROBE=1` (core 0's 5 ms bus samples at setup()'s start, through setup and before a restart), `-DSLED_CAPTURE=1` (the first 128 writes after the release and three read-ring snapshots).

## Testing it

- **`-DSLED_LOOP=<sec>`**: main never reboots; over and over: flood, catch, stub, an empty-RAM map (main's `$0000-$07FF` only, zeroed but for page 6: the OS asks main for nothing), the release, `<sec>` s of the OS running (RTCLOK must count ≥100 frames and the 6502 must write), re-arm. 12 cycles, `-7` at the first failure.
- **`-DSLED_LOOP_RESTART=<n>`** as well: one cycle a boot, then the stub (go 2) parks the 6502 at `$3000` and main restarts under it (`-DSLED_RESTART_SYS=1`: a system reset); the count rides in RTC memory; n cycles across restarts. `-DSLED_PARK_JMP=1` brings back the old `JMP $3000` park (fails on the 800XLs).
- **`-DRESET_AFTER=<sec> -DRESET_REPEAT=1`**: the safe-reboot soak through the stack wait, a reboot every `<sec>` s (the pool's `SOAK` uses 60).
- **csim** (`BUS_CYCLE=1`, `CSIM_ATARI_RESET=0` for no Atari RESET line, as on the hardware): power-on caught after 161 ms (AltirraOS's init), safe reboots (`CSIM_SIDECAR_CMD="@25 reboot;@50 reboot"`) parked at `$0104` and caught in 0 ms, the loop test with restarts; `CSIM_MAIN_START_MS=<ms>` brings main up late (a sweep of 0-3000 ms all caught). csim models a taken branch's extra cycles as idle `$FFFF` reads, which the park detector skips (csim only). csim can't test the no-clock case (atari800 always clocks).
- **Results:** the loop test with restarts passed on all six pool machines once the parks were D5-proof (every run 30/30); the one-minute safe-reboot soak ran ~100 reboots without a failure on its first run.

## Open

- The XL OS's reset-time init under the flood (whether it lands) and where it writes the PUPBT signature relative to the PBI scan: unchecked on a real XL; csim runs AltirraOS.
- The no-clock case hasn't been seen on the pool yet (boot.sh usually powers the Atari on during main's setup).
- The hardware fix for D5.
- An OTA's `esp_restart()` under a running Atari needs `core1StopForRestart()`.
- The Atari's RESET while main runs (its key, or a power-on with main up) still leaves main stale; detection needs a RESET pin (AGENTS.md, open work).
