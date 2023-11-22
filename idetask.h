// SPDX-License-Identifier: GPL-2.0-only
/* This file is part of lide.device
 * Copyright (C) 2023 Matthew Harlum <matt@harlum.net>
 */
#define TASK_NAME "idetask"
#define TASK_PRIORITY 11
#define TASK_STACK_SIZE 65535

#define CHANGEINT_INTERVAL 2 // Poll units every x seconds for disk change

#define CMD_DIE  0x1000
#define CMD_XFER (CMD_DIE + 1)

void ide_task();
void diskchange_task();
BYTE direct_changestate(struct IDEUnit *unit, struct DeviceBase *dev);