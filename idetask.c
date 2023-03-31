#include <devices/scsidisk.h>
#include <exec/errors.h>
#include <proto/alib.h>
#include <proto/exec.h>
#include <string.h>

#include "ata.h"
#include "debug.h"
#include "device.h"
#include "idetask.h"
#include "newstyle.h"
#include "scsi.h"
#include "td64.h"

/**
 * handle_scsi_command
 *
 * Handle SCSI Direct commands
 * @param ioreq IO Request
*/
static void handle_scsi_command(struct IOStdReq *ioreq) {
    struct SCSICmd *scsi_command = ioreq->io_Data;
    struct IDEUnit *unit = (struct IDEUnit *)ioreq->io_Unit;

    UBYTE *data    = (APTR)scsi_command->scsi_Data;
    UBYTE *command = (APTR)scsi_command->scsi_Command;

    ULONG lba;
    ULONG count;
    UBYTE error = 0;

    enum xfer_dir direction = WRITE;

    Trace("Command %ld\n",*scsi_command->scsi_Command);

    switch (scsi_command->scsi_Command[0]) {
        case SCSI_CMD_TEST_UNIT_READY:
            scsi_command->scsi_Actual = 0;
            error = 0;
            break;

        case SCSI_CMD_INQUIRY:
            ((struct SCSI_Inquiry *)data)->peripheral_type = unit->device_type;
            ((struct SCSI_Inquiry *)data)->removable_media = 0;
            ((struct SCSI_Inquiry *)data)->version         = 2;
            ((struct SCSI_Inquiry *)data)->response_format = 2;
            ((struct SCSI_Inquiry *)data)->additional_length = (sizeof(struct SCSI_Inquiry) - 4);

            UWORD *identity = AllocMem(512,MEMF_CLEAR|MEMF_ANY);
            if (!identity) {
                error = HFERR_BadStatus;
                break;
            }
            if (!(ata_identify(unit,identity))) {
                error = HFERR_BadStatus;
                break;
            }
            CopyMem(&identity[ata_identify_model],&((struct SCSI_Inquiry *)data)->vendor,24);
            CopyMem(&identity[ata_identify_fw_rev],&((struct SCSI_Inquiry *)data)->revision,4);
            CopyMem(&identity[ata_identify_serial],&((struct SCSI_Inquiry *)data)->serial,8);
            FreeMem(identity,512);
            scsi_command->scsi_Actual = scsi_command->scsi_Length;
            error = 0;
            break;

        case SCSI_CMD_MODE_SENSE_6:
            if (data == NULL) {
                error = IOERR_BADADDRESS;
                break;
            }

            UBYTE page    = command[2] & 0x3F;

            UBYTE subpage = command[3];

            if (subpage != 0) {
                error = HFERR_BadStatus;
                break;
            }

            UBYTE *data_length = data;   // Mode data length
            data[1] = unit->device_type; // Mode parameter: Media type
            data[2] = 0;                 // DPOFUA
            data[3] = 0;                 // Block descriptor length
            
            *data_length = 3;

            UBYTE idx = 4;
            if (page == 0x3F || page == 0x03) {
                data[idx++] = 0x03; // Page Code: Format Parameters
                data[idx++] = 0x16; // Page length
                for (int i=0; i <8; i++) {
                    data[idx++] = 0;
                }
                data[idx++] = (unit->sectorsPerTrack >> 8);
                data[idx++] = unit->sectorsPerTrack;
                data[idx++] = (unit->blockSize >> 8);
                data[idx++] = unit->blockSize;
                for (int i=0; i<12; i++) {
                    data[idx++] = 0;
                }
            }

            if (page == 0x3F || page == 0x04) {
                data[idx++] = 0x04; // Page code: Rigid Drive Geometry Parameters
                data[idx++] = 0x16; // Page length
                data[idx++] = 0;
                data[idx++] = (unit->cylinders >> 8);
                data[idx++] = unit->cylinders;
                data[idx++] = unit->heads;
                for (int i=0; i<19; i++) {
                    data[idx++] = 0;
                }
            }

            *data_length += (idx + 1);
            error = 0;

            scsi_command->scsi_Actual = *data_length + 1;
            break;

        case SCSI_CMD_READ_CAPACITY_10:
            if (data == NULL) {
                error = IOERR_BADADDRESS;
                break;
            }

            ((struct SCSI_CAPACITY_10 *)data)->lba = (unit->logicalSectors) - 1;
            ((struct SCSI_CAPACITY_10 *)data)->block_size = unit->blockSize;
            scsi_command->scsi_Actual = 8;
            error = 0;
            break;

        case SCSI_CMD_READ_6:
            direction = READ;
        case SCSI_CMD_WRITE_6:
            lba   = (((((struct SCSI_CDB_6 *)command)->lba_high & 0x1F) << 16) |
                       ((struct SCSI_CDB_6 *)command)->lba_mid << 8 |
                       ((struct SCSI_CDB_6 *)command)->lba_low);

            count = ((struct SCSI_CDB_6 *)command)->length;
            goto do_scsi_transfer;

        case SCSI_CMD_READ_10:
            direction = READ;
        case SCSI_CMD_WRITE_10:
            lba    = ((struct SCSI_CDB_10 *)command)->lba;
            count  = ((struct SCSI_CDB_10 *)command)->length;

do_scsi_transfer:
            Info("LBA: %ld\n",lba);
            if (data == NULL || (lba + count) > (unit->logicalSectors - 1)) {
                error = IOERR_BADADDRESS;
                break;
            }

            error = ata_transfer(data,lba,count,&scsi_command->scsi_Actual,unit,direction);
            Info("Returns: %ld\n",error);
            break;

        default:
            error = HFERR_BadStatus;
            break;
    }

    ioreq->io_Error = error;
    scsi_command->scsi_CmdActual = scsi_command->scsi_CmdLength;

    if (error != 0) {
        scsi_command->scsi_Status = SCSI_CHECK_CONDITION;
    } else {
        scsi_command->scsi_Status = 0;
    }
    scsi_command->scsi_SenseActual = 0; // TODO: Add Sense data
}

/**
 * ide_task
 *
 * This is a task to complete IO Requests for all units
 * Requests are sent here from begin_io via the dev->TaskMP Message port
*/
void __attribute__((noreturn)) ide_task () {
    struct ExecBase *SysBase = *(struct ExecBase **)4UL;
    struct Task volatile *task = FindTask(NULL);
    struct MsgPort *mp;
    struct IOStdReq *ioreq;
    struct IDEUnit *unit;
    UWORD blockShift;
    ULONG lba;
    ULONG count;
    enum xfer_dir direction = WRITE;


    Info("Task: waiting for init\n");
    while (task->tc_UserData == NULL); // Wait for Task Data to be populated
    struct DeviceBase *dev = (struct DeviceBase *)task->tc_UserData;
    Trace("Task: CreatePort()\n");
    // Create the MessagePort used to send us requests
    if ((mp = CreatePort(NULL,0)) == NULL) {
        dev->Task = NULL; // Failed to create MP, let the device know
        RemTask(NULL);
        Wait(0);
    }

    dev->TaskMP = mp;
    dev->TaskActive = true;

    while (1) {
        // Main loop, handle IO Requests as they comee in.
        Trace("WaitPort()\n");
        Wait(1 << mp->mp_SigBit); // Wait for an IORequest to show up

        while ((ioreq = (struct IOStdReq *)GetMsg(mp))) {
            unit = (struct IDEUnit *)ioreq->io_Unit;
            direction = WRITE;

            switch (ioreq->io_Command) {
                case CMD_READ:
                case TD_READ64:
                case NSCMD_TD_READ64:
                    direction = READ;

                case CMD_WRITE:
                case TD_WRITE64:
                case TD_FORMAT64:
                case NSCMD_TD_WRITE64:
                case NSCMD_TD_FORMAT64:
                    blockShift = ((struct IDEUnit *)ioreq->io_Unit)->blockShift;
                    lba = (((long long)ioreq->io_Actual << 32 | ioreq->io_Offset) >> blockShift);
                    count = (ioreq->io_Length >> blockShift);

                    if ((lba + count) > (unit->logicalSectors - 1)) {
                        ioreq->io_Error = IOERR_BADADDRESS;
                        break;
                    }

                    ioreq->io_Error = ata_transfer(ioreq->io_Data, lba, count, &ioreq->io_Actual, unit, direction);
                    break;

                /* SCSI Direct */
                case HD_SCSICMD:
                    handle_scsi_command(ioreq);
                    break;

                /* CMD_DIE: Shut down this task and clean up */
                case CMD_DIE:
                    Info("CMD_DIE: Shutting down IDE Task\n");
                    DeletePort(mp);
                    dev->TaskMP = NULL;
                    dev->Task = NULL;
                    dev->TaskActive = false;
                    ReplyMsg(&ioreq->io_Message);
                    RemTask(NULL);
                    Wait(0);

                default:
                    // Unknown commands.
                    ioreq->io_Error = IOERR_NOCMD;
                    ioreq->io_Actual = 0;
                    break;
            }

            ReplyMsg(&ioreq->io_Message);
        }
    }

}

