#pragma once
#include <stdbool.h>

// ============================================================================
// Fault injection: proves on the real board that the recovery layers work
//
// "The bridge never needs its plug pulled" is a claim about failures that do not
// happen on a bench by themselves. This makes them happen on purpose, over the USB
// console only (Improv command 0x46 with the word CRASH-TEST), so each recovery path
// can be exercised and timed:
//
//   panic     abort()                         -> panic handler -> reboot
//   taskwdt   busy loop at high priority      -> task watchdog (idle task starved) -> reboot
//   intwdt    interrupts off, endless loop    -> interrupt watchdog -> reboot
//   deadlock  a supervised task stops beating -> supervisor -> reboot
//   leak      eats the heap and keeps it      -> supervisor (low heap) -> reboot
//
// scripts/reboot_soak.py --mode fault --fault <kind> runs one and checks that the board
// answers again without anybody touching it. Three crashes in a row end in SAFE MODE
// (supervisor.h), which that script checks as well.
//
// It needs a physical USB connection, which already allows reflashing the board, so it
// opens nothing that was not open. It is not reachable over the network.
// ============================================================================

bool fault_known(const char *kind);

// Starts the fault on a task of its own after a short delay, so the reply to the
// command goes out first. Returns at once.
void fault_run(const char *kind);
