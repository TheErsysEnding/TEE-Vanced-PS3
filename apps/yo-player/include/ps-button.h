#pragma once

// ps-button - press the PS button on the user's behalf.
//
// A DualShock 4 / DualSense pairs with a PS3 and works as a controller, but its PS button never reaches the
// console: cellPadGetData only ever reports the 16 buttons in DIGITAL1/DIGITAL2, and there is no PS bit
// among them (see pad.h). So on those pads there is no way to open the XMB out of a running app at all.
//
// The way out is the one Sony's own tutorial takes (samples/tutorial/Controller/CustomController, whose
// stated job is "operate the system dialog with a custom controller"): register a virtual controller - a
// "logical debug device" - with libpad and insert one frame that has the PS bit set.
//
// Despite the "debug" in the name this is ordinary game API. cell/pad/libpad.h declares all four functions
// used here, and libio_stub.a exports them from the same "sys_io" PRX library the app already imports for
// cellPadInit/cellPadGetData - verified by building a probe ELF and reading its import table. Nothing here
// needs CFW, a VSH plugin, webMAN or a syscall.
//
// What is NOT established: whether the XMB acts on an injected PS press while a GAME holds the foreground.
// Every real-world user of this API found in the wild is a VSH plugin, so the console is the proof. A
// failure is harmless and reported - the press simply does nothing.

// Register a virtual pad, press PS, release it, unregister. Returns 0 if the press was delivered, negative
// if the virtual controller could not be registered (out of ports, or the firmware refused).
//
// Blocks the caller for PS_HOLD_MS. That is deliberate: the press has to be visible for a moment to count,
// and this only ever runs from a button the user pressed on purpose.
int pressPsButton(void);
