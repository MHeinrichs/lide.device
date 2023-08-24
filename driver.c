// SPDX-License-Identifier: GPL-2.0-only
/* This file is part of lide.device
 * Copyright (C) 2023 Matthew Harlum <matt@harlum.net>
 */
#include <devices/scsidisk.h>
#include <devices/timer.h>
#include <devices/trackdisk.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <proto/alib.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <resources/filesysres.h>

#include <string.h>
#include <stdio.h>

#include "ata.h"
#include "atapi.h"
#include "device.h"
#include "idetask.h"
#include "newstyle.h"
#include "td64.h"
#include "mounter.h"
#include "debug.h"

struct ExecBase *SysBase;

/*-----------------------------------------------------------
A library or device with a romtag should start with moveq #-1,d0 (to
safely return an error if a user tries to execute the file), followed by a
Resident structure.
------------------------------------------------------------*/
int __attribute__((no_reorder)) _start()
{
    return -1;
}

asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    _endskip                \n"
    "       dc.b    "XSTR(RTF_COLDSTART)"   \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name+4          \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _init                   \n");

char device_name[] = DEVICE_NAME;
char const device_id_string[] = DEVICE_ID_STRING;

/**
 * set_dev_name
 *
 * Try to set a unique drive name
 * will prepend 2nd/3rd/4th. etc to the beginning of device_name
*/
char * set_dev_name(struct DeviceBase *dev) {
    struct ExecBase *SysBase = dev->SysBase;

    ULONG device_prefix[] = {' nd.', ' rd.', ' th.'};
    char * devName = (device_name + 4); // Start with just the base device name, no prefix

    /* Prefix the device name if a device with the same name already exists */
    for (int i=0; i<8; i++) {
        if (FindName(&SysBase->DeviceList,devName)) {
            if (i==0) devName = device_name;
            switch (i) {
                case 0:
                    *(ULONG *)devName = device_prefix[0];
                    break;
                case 1:
                    *(ULONG *)devName = device_prefix[1];
                    break;
                default:
                    *(ULONG *)devName = device_prefix[2];
                    break;
            }
            *(char *)devName = '2' + i;
        } else {
            Info("Device name: %s\n",devName);
            return devName;
        }
    }
    Info("Couldn't set device name.\n");
    return NULL;
}

/**
 * L_CreateTask
 * 
 * Create a task with tc_UserData populated before it starts
 * @param taskName Pointer to a null-terminated string
 * @param priority Task Priority between -128 and 127
 * @param funcEntry Pointer to the first executable instruction of the Task code
 * @param stackSize Size in bytes of stack for the task
 * @param userData Pointer to User Data
*/
struct Task *L_CreateTask(char * taskName, LONG priority, APTR funcEntry, ULONG stackSize, APTR userData) {
        stackSize = (stackSize + 3UL) & ~3UL;
        
        struct Task *task;
        
        struct {
            struct Node ml_Node;
            UWORD ml_NumEntries;
            struct MemEntry ml_ME[2];
        } alloc_ml = {
            .ml_NumEntries = 2,
            .ml_ME[0].me_Un.meu_Reqs = MEMF_PUBLIC|MEMF_CLEAR,
            .ml_ME[1].me_Un.meu_Reqs = MEMF_ANY|MEMF_CLEAR,
            .ml_ME[0].me_Length = sizeof(struct Task),
            .ml_ME[1].me_Length = stackSize
        };

        memset(&alloc_ml.ml_Node,0,sizeof(struct Node));

        struct MemList *ml = AllocEntry((struct MemList *)&alloc_ml);
        if ((ULONG)ml & 1<<31) {
            Info("Couldn't allocate memory for task\n");
            return NULL;
        }

        task                  = ml->ml_ME[0].me_Un.meu_Addr;
        task->tc_SPLower      = ml->ml_ME[1].me_Un.meu_Addr;
        task->tc_SPUpper      = ml->ml_ME[1].me_Un.meu_Addr + stackSize;
        task->tc_SPReg        = task->tc_SPUpper;
        task->tc_UserData     = userData;
        task->tc_Node.ln_Name = taskName;
        task->tc_Node.ln_Type = NT_TASK;
        task->tc_Node.ln_Pri  = priority;
        NewList(&task->tc_MemEntry);
        AddHead(&task->tc_MemEntry,(struct Node *)ml);

        AddTask(task,funcEntry,NULL);

        return task;
}

#if CDBOOT
/**
 * FindCDFS
 * 
 * Look for a CD Filesystem in FileSystem.resource
 * 
 * @return BOOL True if CDFS found
*/
static BOOL FindCDFS() {
    struct FileSysResource *fsr = OpenResource(FSRNAME);
    struct FileSysEntry *fse;

    if (fsr == NULL) return false;

    for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head; fse->fse_Node.ln_Succ != NULL; fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
        if (fse->fse_DosType == 'CD01') return true;
    }

    return false;
}
#endif

/**
 * Cleanup
 *
 * Free used resources back to the system
*/
static void Cleanup(struct DeviceBase *dev) {
    Info("Cleaning up...\n");
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
    for (int i=0; i < MAX_UNITS; i++) {
        // Un-claim the boards
        if (dev->units[i].cd != NULL) {
            dev->units[i].cd->cd_Flags |= CDF_CONFIGME;
        }
    }

    if (dev->TimeReq->tr_node.io_Device) CloseDevice((struct IORequest *)dev->TimeReq);
    if (dev->TimeReq) DeleteExtIO((struct IORequest *)dev->TimeReq);

    // If we still own the timer reply port we need to delete it now
    if (dev->IDETimerMP->mp_SigTask == FindTask(NULL)) {
        if (dev->IDETimerMP)  DeletePort(dev->IDETimerMP);
    }

    if (dev->ExpansionBase) CloseLibrary((struct Library *)dev->ExpansionBase);
    if (dev->units) FreeMem(dev->units,sizeof(struct IDEUnit) * MAX_UNITS);
}

/**
 * init_device
 *
 * Scan for drives and initialize the driver if any are found
*/
struct Library __attribute__((used, saveds)) * init_device(struct ExecBase *SysBase asm("a6"), BPTR seg_list asm("a0"), struct DeviceBase *dev asm("d0"))
//struct Library __attribute__((used)) * init_device(struct ExecBase *SysBase, BPTR seg_list, struct DeviceBase *dev)
{
    dev->SysBase = SysBase;
    Trace("Init dev, base: %08lx\n",dev);
    struct Library *ExpansionBase = NULL;

    char *devName;

    if (!(devName = set_dev_name(dev))) return NULL;
    /* save pointer to our loaded code (the SegList) */
    dev->saved_seg_list       = seg_list;
    dev->lib.lib_Node.ln_Type = NT_DEVICE;
    dev->lib.lib_Node.ln_Name = devName;
    dev->lib.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    dev->lib.lib_Version      = DEVICE_VERSION;
    dev->lib.lib_Revision     = DEVICE_REVISION;
    dev->lib.lib_IdString     = (APTR)device_id_string;

    dev->is_open    = FALSE;
    dev->num_boards = 0;
    dev->num_units  = 0;
    dev->IDETaskMP     = NULL;
    dev->IDETask       = NULL;
    dev->IDETaskActive = false;


    if ((dev->units = AllocMem(sizeof(struct IDEUnit)*MAX_UNITS, (MEMF_ANY|MEMF_CLEAR))) == NULL)
        return NULL;

    Trace("Dev->Units: %08lx\n",(ULONG)dev->units);

    if (!(ExpansionBase = (struct Library *)OpenLibrary("expansion.library",0))) {
        Cleanup(dev);
        return NULL;
    } else {
        dev->ExpansionBase = ExpansionBase;
    }

    if ((dev->IDETimerMP = CreatePort(NULL,0)) != NULL && (dev->TimeReq = (struct timerequest *)CreateExtIO(dev->IDETimerMP, sizeof(struct timerequest))) != NULL) {
        if (OpenDevice("timer.device",UNIT_MICROHZ,(struct IORequest *)dev->TimeReq,0)) {
            Info("Failed to open timer.device\n");
            Cleanup(dev);
            return NULL;
        }
    } else {
        Info("Failed to create Timer MP or Request.\n");
        Cleanup(dev);
        return NULL;
    }

    struct CurrentBinding cb;

    GetCurrentBinding(&cb,sizeof(struct CurrentBinding));

    struct ConfigDev *cd = cb.cb_ConfigDev;

    if (cd->cd_Rom.er_Manufacturer != 5194 && cd->cd_Rom.er_Manufacturer != 2092) {
        Cleanup(dev);
        return 0;
    }

    Trace("Claiming board %08lx\n",(ULONG)cd->cd_BoardAddr);
    cd->cd_Flags &= ~(CDF_CONFIGME); // Claim the board

    dev->num_boards++;

    // Detect if there are 1 or 2 IDE channels on this board
    // 2 channel boards use the CS2 decode for the second channel
    UBYTE channels = 2;

    UBYTE *status     = cd->cd_BoardAddr + CHANNEL_0 + ata_reg_status;
    UBYTE *alt_status = cd->cd_BoardAddr + CHANNEL_0 + ata_reg_altStatus;

    UBYTE *drvsel     = cd->cd_BoardAddr + CHANNEL_0 + ata_reg_devHead;

    *drvsel = 0xE0;

    // On the AT-Bus 2008 (Clone) the ROM is selected on the lower byte when IDE_CS1 is asserted
    // Not a problem in single channel mode - the drive registers there only use the upper byte
    // If Status == Alt Status or it's an AT-Bus card then only one channel is supported.
    if (*status == *alt_status || cd->cd_Rom.er_Manufacturer == BSC_MANUF_ID) {
        channels = 1;
    }

    Info("Channels: %ld\n",channels);

    for (BYTE i=0; i < (2 * channels); i++) {
        // Setup each unit structure
        dev->units[i].SysBase        = SysBase;
        dev->units[i].TimeReq        = dev->TimeReq;
        dev->units[i].cd             = cd;
        dev->units[i].primary        = ((i%2) == 1) ? false : true;
        dev->units[i].channel        = ((i%4) < 2) ? 0 : 1;
        dev->units[i].change_count   = 1;
        dev->units[i].device_type    = DG_DIRECT_ACCESS;
        dev->units[i].unitOpened     = false;
        dev->units[i].mediumPresent  = false;
        dev->units[i].present        = false;
        dev->units[i].atapi          = false;
        dev->units[i].xfer_multiple  = false;
        dev->units[i].multiple_count = 0;
        dev->units[i].shadowDevHead  = &dev->shadowDevHeads[i>>1];
        *dev->units[i].shadowDevHead = 0;

        // Initialize the change int list
        dev->units[i].changeints.mlh_Tail     = NULL;
        dev->units[i].changeints.mlh_Head     = (struct MinNode *)&dev->units[i].changeints.mlh_Tail;
        dev->units[i].changeints.mlh_TailPred = (struct MinNode *)&dev->units[i].changeints;

        Warn("testing unit %08lx\n",i);

        if (ata_init_unit(&dev->units[i])) {
            dev->num_units++;
        }
    }

    Info("Detected %ld drives, %ld boards\n",dev->num_units, dev->num_boards);

    if (dev->num_units > 0) {
        Trace("Start the Task\n");

        // The IDE Task will take over the Timer reply port when it starts
        dev->IDETimerMP->mp_Flags = PA_IGNORE;
        FreeSignal(dev->IDETimerMP->mp_SigBit);

        // Start the IDE Task
        dev->IDETask = L_CreateTask(dev->lib.lib_Node.ln_Name,TASK_PRIORITY,ide_task,TASK_STACK_SIZE,dev);
        if (!dev->IDETask) {
            Info("IDE Task failed\n");
            Cleanup(dev);
            return NULL;
        } else {
            Trace("Task created!, waiting for init\n");
        }

        // Wait for task to init
        while (dev->IDETaskActive == false) {
            // If dev->IDETask has been set to NULL it means the task failed to start
             if (dev->IDETask == NULL) {
                Info("IDE Task failed.\n");
                Cleanup(dev);
                return NULL;
            }
        }

        dev->ChangeTask = L_CreateTask(dev->lib.lib_Node.ln_Name,0,diskchange_task,TASK_STACK_SIZE,dev);

        Info("Startup finished.\n");
        return (struct Library *)dev;
    } else {
        Cleanup(dev);
        return NULL;
    }
}

/* device dependent expunge function
!!! CAUTION: This function runs in a forbidden state !!!
This call is guaranteed to be single-threaded; only one task
will execute your Expunge at a time. */
static BPTR __attribute__((used, saveds)) expunge(struct DeviceBase *dev asm("a6"))
{
    Trace((CONST_STRPTR) "running expunge()\n");
 
    /**
     * Don't expunge
     * 
     * If expunged the driver will be gone until reboot
     * 
     * Also need to figure out how to kill the disk change int task cleanly before this can be enabled
    */

    dev->lib.lib_Flags |= LIBF_DELEXP;
    return 0;

    // if (dev->lib.lib_OpenCnt != 0)
    // {
    //     dev->lib.lib_Flags |= LIBF_DELEXP;
    //     return 0;
    // }

    // if (dev->IDETask != NULL && dev->IDETaskActive == true) {
    //     // Shut down ide_task

    //     struct MsgPort *mp = NULL;
    //     struct IOStdReq *ioreq = NULL;

    //     if ((mp = CreatePort(NULL,0)) == NULL)
    //         return 0;
    //     if ((ioreq = CreateStdIO(mp)) == NULL) {
    //         DeletePort(mp);
    //         return 0;
    //     }

    //     ioreq->io_Command = CMD_DIE; // Tell ide_task to shut down

    //     PutMsg(dev->IDETaskMP,(struct Message *)ioreq);
    //     WaitPort(mp);                // Wait for ide_task to signal that it is shutting down

    //     if (ioreq) DeleteStdIO(ioreq);
    //     if (mp) DeletePort(mp);
    // }

    // Cleanup(dev);
    // BPTR seg_list = dev->saved_seg_list;
    // Remove(&dev->lib.lib_Node);
    // FreeMem((char *)dev - dev->lib.lib_NegSize, dev->lib.lib_NegSize + dev->lib.lib_PosSize);
    // return seg_list;
}

/* device dependent open function
!!! CAUTION: This function runs in a forbidden state !!!
This call is guaranteed to be single-threaded; only one task
will execute your Open at a time. */
static void __attribute__((used, saveds)) open(struct DeviceBase *dev asm("a6"), struct IORequest *ioreq asm("a1"), ULONG unitnum asm("d0"), ULONG flags asm("d1"))
{
    UBYTE lun = unitnum / 10;
    unitnum = (unitnum % 10);
    struct IDEUnit *unit = NULL;

    if (lun != 0) {
        // No LUNs for IDE drives
        ioreq->io_Error = TDERR_BadUnitNum;
        return;
    }

    if (unitnum >= MAX_UNITS) {
        ioreq->io_Error = IOERR_OPENFAIL;
        return;
    }

    unit = &dev->units[unitnum];

    if (unit->present == false) {
        ioreq->io_Error = TDERR_BadUnitNum;
        return;
    }
    

    Trace((CONST_STRPTR) "running open() for unitnum %ld\n",unitnum);
    ioreq->io_Error = IOERR_OPENFAIL;

    if (dev->IDETask == NULL || dev->IDETaskActive == false) {
        ioreq->io_Error = IOERR_OPENFAIL;
        return;
    }


    ioreq->io_Unit = (struct Unit *)unit;

    // Send a TD_CHANGESTATE ioreq for the unit if it is ATAPI and not already open
    // This will update the media presence & geometry
    if (unit->atapi && unit->unitOpened == false) direct_changestate(unit,dev);

    unit->unitOpened = true;

    if (!dev->is_open)
    {
        dev->is_open = TRUE;
    }

    dev->lib.lib_OpenCnt++;
    ioreq->io_Error = 0; //Success
}


static void td_get_geometry(struct IOStdReq *ioreq) {
    struct DriveGeometry *geometry = (struct DriveGeometry *)ioreq->io_Data;
    struct IDEUnit *unit = (struct IDEUnit *)ioreq->io_Unit;

    if (unit->atapi && unit->mediumPresent == false) {
        ioreq->io_Error = TDERR_DiskChanged;
        return;
    }

    geometry->dg_SectorSize   = unit->blockSize;
    geometry->dg_TotalSectors = unit->logicalSectors;
    geometry->dg_Cylinders    = unit->cylinders;
    geometry->dg_CylSectors   = (unit->sectorsPerTrack * unit->heads);
    geometry->dg_Heads        = unit->heads;
    geometry->dg_TrackSectors = unit->sectorsPerTrack;
    geometry->dg_BufMemType   = MEMF_PUBLIC;
    geometry->dg_DeviceType   = unit->device_type;
    geometry->dg_Flags        = (unit->atapi) ? DGF_REMOVABLE : 0;

    ioreq->io_Error = 0;
    ioreq->io_Actual = sizeof(struct DriveGeometry);
}


/* device dependent close function
!!! CAUTION: This function runs in a forbidden state !!!
This call is guaranteed to be single-threaded; only one task
will execute your Close at a time. */
static BPTR __attribute__((used, saveds)) close(struct DeviceBase *dev asm("a6"), struct IORequest *ioreq asm("a1"))
{
    Trace((CONST_STRPTR) "running close()\n");
    dev->lib.lib_OpenCnt--;

    if (dev->lib.lib_OpenCnt == 0 && (dev->lib.lib_Flags & LIBF_DELEXP))
        return expunge(dev);

    return 0;
}

static UWORD supported_commands[] =
{
    CMD_CLEAR,
    CMD_UPDATE,
    CMD_READ,
    CMD_WRITE,
    TD_ADDCHANGEINT,
    TD_REMCHANGEINT,
    TD_PROTSTATUS,
    TD_CHANGENUM,
    TD_CHANGESTATE,
    TD_EJECT,
    TD_GETDRIVETYPE,
    TD_GETGEOMETRY,
    TD_MOTOR,
    TD_PROTSTATUS,
    TD_READ64,
    TD_WRITE64,
    TD_FORMAT64,
    NSCMD_DEVICEQUERY,
    NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,
    NSCMD_TD_FORMAT64,
    HD_SCSICMD,
    0
};

/**
 * begin_io
 *
 * Handle immediate requests and send any others to ide_task
*/
static void __attribute__((used, saveds)) begin_io(struct DeviceBase *dev asm("a6"), struct IOStdReq *ioreq asm("a1"))
{
    struct IDEUnit *unit = (struct IDEUnit *)ioreq->io_Unit;

    Trace((CONST_STRPTR) "running begin_io()\n");
    ioreq->io_Error = TDERR_NotSpecified;

    if (dev->IDETask == NULL || dev->IDETaskActive == false) {
        ioreq->io_Error = IOERR_OPENFAIL;
    }

    if (ioreq == NULL || ioreq->io_Unit == 0) return;
    Trace("Command %lx\n",ioreq->io_Command);
    switch (ioreq->io_Command) {
        case TD_MOTOR:
        case CMD_CLEAR:
        case CMD_UPDATE:
            ioreq->io_Actual = 0;
            ioreq->io_Error  = 0;
            break;

        case TD_CHANGENUM:
            ioreq->io_Actual = unit->change_count;
            ioreq->io_Error  = 0;
            break;

        case TD_GETDRIVETYPE:
            ioreq->io_Actual = unit->device_type;
            ioreq->io_Error  = 0;
            break;

        case TD_GETGEOMETRY:
            td_get_geometry(ioreq);
            break;

        case TD_CHANGESTATE:
        case CMD_READ:
        case CMD_WRITE:
            ioreq->io_Actual = 0; // Clear high offset for 32-bit commands
        case TD_PROTSTATUS:
        case TD_ADDCHANGEINT:
        case TD_REMCHANGEINT:
        case TD_EJECT:
        case TD_FORMAT:
        case TD_READ64:
        case TD_WRITE64:
        case TD_FORMAT64:
        case NSCMD_TD_READ64:
        case NSCMD_TD_WRITE64:
        case NSCMD_TD_FORMAT64:
        case HD_SCSICMD:
            // Send all of these to ide_task
            ioreq->io_Flags &= ~IOF_QUICK;
            PutMsg(dev->IDETaskMP,&ioreq->io_Message);
            Trace((CONST_STRPTR) "IO queued\n");
            return;

        case NSCMD_DEVICEQUERY:
            if (ioreq->io_Length >= sizeof(struct NSDeviceQueryResult))
            {
                struct NSDeviceQueryResult *result = ioreq->io_Data;

                result->DevQueryFormat    = 0;
                result->SizeAvailable     = sizeof(struct NSDeviceQueryResult);
                result->DeviceType        = NSDEVTYPE_TRACKDISK;
                result->DeviceSubType     = 0;
                result->SupportedCommands = supported_commands;

                ioreq->io_Actual = sizeof(struct NSDeviceQueryResult);
                ioreq->io_Error = 0;
            }
            else {
                ioreq->io_Error = IOERR_BADLENGTH;
            }
            break;

        default:
            Warn("Unknown command %d\n", ioreq->io_Command);
            ioreq->io_Error = IOERR_NOCMD;
    }
    if (ioreq && !(ioreq->io_Flags & IOF_QUICK)) {
        ReplyMsg(&ioreq->io_Message);
    }
}

/**
 * abort_io
 *
 * Abort io request
*/
static ULONG __attribute__((used, saveds)) abort_io(struct Library *dev asm("a6"), struct IOStdReq *ioreq asm("a1"))
{
    Trace((CONST_STRPTR) "running abort_io()\n");
    return IOERR_NOCMD;
}


static const ULONG device_vectors[] =
    {
        (ULONG)open,
        (ULONG)close,
        (ULONG)expunge,
        0, //extFunc not used here
        (ULONG)begin_io,
        (ULONG)abort_io,
        -1 //function table end marker
    };

/**
 * init
 *
 * Create the device and add it to the system if init_device succeeds
*/
static struct Library __attribute__((used)) * init(BPTR seg_list asm("a0"))
{
    SysBase = *(struct ExecBase **)4UL;
    Info("Init driver.\n");
    struct MountStruct *ms = NULL;
    struct DeviceBase *mydev = (struct DeviceBase *)MakeLibrary((ULONG *)&device_vectors,  // Vectors
                                                                NULL,                      // InitStruct data
                                                                (APTR)init_device,         // Init function
                                                                sizeof(struct DeviceBase), // Library data size
                                                                seg_list);                 // Segment list

    if (mydev != NULL) {
        ULONG ms_size = (sizeof(struct MountStruct) + (MAX_UNITS * sizeof(struct UnitStruct)));
        Info("Add Device.\n");
        AddDevice((struct Device *)mydev);

        if ((ms = AllocMem(ms_size,MEMF_ANY|MEMF_PUBLIC)) == NULL) goto done;

        ms->deviceName  = mydev->lib.lib_Node.ln_Name;
        ms->creatorName = NULL;
        ms->numUnits    = 0;
        ms->SysBase     = SysBase;

        UWORD *idx = &ms->numUnits;
#if CDBOOT
        BOOL CDBoot = FindCDFS();
#endif
        for (int i=0; i<MAX_UNITS; i++) {
            if (mydev->units[i].present == true) {
#if CDBOOT
                // If CDFS not resident don't bother adding the CDROM to the mountlist
                if (mydev->units[i].device_type == DG_CDROM && !CDBoot) continue;
#endif
                ms->Units[*idx].unitNum    = i;
                ms->Units[*idx].deviceType = mydev->units[i].device_type;
                ms->Units[*idx].configDev  = mydev->units[i].cd;
                *idx += 1;
            }
        }
        if (ms->numUnits > 0) {
            MountDrive(ms);
        }

        FreeMem(ms,ms_size);
    }
done:
    return (struct Library *)mydev;
}
