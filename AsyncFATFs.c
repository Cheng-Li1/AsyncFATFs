#include "AsyncFATFs.h"
#include "ff15/source/ff.h"
#include "ff15/source/diskio.h"
#include "libblocksharedqueue/blk_shared_queue.h"
#include "FiberPool/FiberPool.h"
#include "libfssharedqueue/fs_shared_queue.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <microkit.h>

#define RequestPool_Size 128

#define Coroutine_STACKSIZE 0x40000

#define Client_CH 1
#define Server_CH 2

#ifdef FS_DEBUG_PRINT
#include "../../vmm/src/util/printf.h"
#endif

blk_queue_handle_t blk_queue_handle_memory;
blk_queue_handle_t *blk_queue_handle = &blk_queue_handle_memory;

struct sddf_fs_queue *FATfs_command_queue;
struct sddf_fs_queue *FATfs_completion_queue;

blk_req_queue_t *request;
blk_resp_queue_t *response;

// Compromised code here
blk_storage_info_t *config;

void* Coroutine_STACK_ONE;
void* Coroutine_STACK_TWO;
void* Coroutine_STACK_THREE;
void* Coroutine_STACK_FOUR;

bool blk_request_pushed;

typedef enum {
    FREE,
    INUSE
} space_status;

typedef struct FS_request{
    /* Client side cmd info */
    FS_CMD cmd;
    uint64_t args[9];
    uint64_t request_id;
    /* FiberPool metadata */
    co_handle handle;
    /* self metadata */
    space_status stat;
} FSRequest;

/*
 The function array presents the function that 
 mapped by the enum
 typedef enum {
    SDDF_FS_CMD_OPEN,
    SDDF_FS_CMD_CLOSE,
    SDDF_FS_CMD_STAT,
    SDDF_FS_CMD_PREAD,
    SDDF_FS_CMD_PWRITE,
    SDDF_FS_CMD_RENAME,
    SDDF_FS_CMD_UNLINK,
    SDDF_FS_CMD_MKDIR,
    SDDF_FS_CMD_RMDIR,
    SDDF_FS_CMD_OPENDIR,
    SDDF_FS_CMD_CLOSEDIR,
    SDDF_FS_CMD_FSYNC,
    SDDF_FS_CMD_READDIR,
    SDDF_FS_CMD_SEEKDIR,
    SDDF_FS_CMD_TELLDIR,
    SDDF_FS_CMD_REWINDDIR,
} FS_CMD;
*/

void (*operation_functions[])() = {
    f_mount_async,
    f_open_async,
    f_close_async,
    f_stat_async,
    f_pread_async,
    f_pwrite_async,
    f_rename_async,
    f_unlink_async
};

static FSRequest RequestPool[MAX_COROUTINE_NUM];

void Fill_Client_Response(union sddf_fs_message* message, const FSRequest* Finished_Request) {
    message->completion.status = Finished_Request->args[Status_bit];
    message->completion.data[0] = Finished_Request->args[First_data_bit];
    message->completion.data[1] = Finished_Request->args[Second_data_bit];
    return;
}

// Setting up the request in the requestpool and push the request to the FiberPool
void SetUp_request(int32_t index, union sddf_fs_message message) {
    RequestPool[index].request_id = message.command.request_id;
    RequestPool[index].cmd = message.command.cmd_type;
    memcpy(RequestPool[index].args, message.command.args, SDDF_ARGS_SIZE * sizeof(uint64_t));
    FiberPool_push(operation_functions[RequestPool[index].cmd], RequestPool[index].args, 
      2, &(RequestPool[index].handle));
    return;
}

void init(void) {
    // Init the block device queue
    // Have to make sure who initialize this SDDF queue
    blk_queue_init(blk_queue_handle, request, response, true, 
    BLK_REQ_QUEUE_SIZE, BLK_RESP_QUEUE_SIZE);
    struct stack_mem stackmem[4];
    stackmem[0].memory = Coroutine_STACK_ONE;
    stackmem[0].size = Coroutine_STACKSIZE;
    stackmem[1].memory = Coroutine_STACK_TWO;
    stackmem[1].size = Coroutine_STACKSIZE;
    stackmem[2].memory = Coroutine_STACK_THREE;
    stackmem[2].size = Coroutine_STACKSIZE;
    stackmem[3].memory = Coroutine_STACK_FOUR;
    stackmem[3].size = Coroutine_STACKSIZE;
    FiberPool_init(stackmem, 4, 1);
}

// mimic microkit_channel
/*
  The filesystems should be blockwait for new message if and only if all of working
  coroutines are either free(no tasks assigned to them, no pending replies) or blocked in diskio.
  If filesystem is blocked here and any working coroutines are free, then the FATfs_command_queue must
  also be empty.
*/
void notified(microkit_channel ch) {
    //printf("FS IRQ received: %d\n", ch);
    union sddf_fs_message message;
// Compromised code here, polling for server's state until it is ready
    while (!config->ready) {}

    switch (ch) {
    case Client_CH: 
        break;
    case Server_CH: {
        blk_response_status_t status;
        uintptr_t addr;
        uint16_t count;
        uint16_t success_count;
        uint32_t id;
        while (!blk_resp_queue_empty(blk_queue_handle)) {
            blk_dequeue_resp(blk_queue_handle, &status, &addr, &count, &success_count, &id);
            
            #ifdef FS_DEBUG_PRINT
            printf_("blk_dequeue_resp: status: %d addr: 0x%lx count: %d success_count: %d ID: %d\n", status, addr, count, success_count, id);
            #endif

            FiberPool_SetArgs(RequestPool[id].handle, (void* )(status));
            Fiber_wake(RequestPool[id].handle);
        }
        break;
    }
    default:
        return;
    }

    int32_t index;
    int32_t i;
    // This variable track if the fs should send back reply to the file system client
    bool Client_have_replies = false;
    // This variable track if there are new requests being pushed into the couroutine pool or not
    bool New_request_pushed = true;
    /**
      I assume this big while loop is the confusing and critical part for dispatching coroutines and send back the results.
    **/
    while (New_request_pushed) {
        // Performance bug here, should check if the reason being wake up is from notification from the blk device driver
        // Then decide to yield() or not
        // And should only send back notification to blk device driver if at least one coroutine is block waiting
        blk_request_pushed = false;

        Fiber_yield();
        
        /** 
        If the code below get executed, then all the working coroutines are either blocked or finished.
        So the code below would send the result back to client through SDDF and do the cleanup for finished 
        coroutines. After that, the main coroutine coroutine would block wait on new requests or server sending
        responses.
        **/
        if (blk_request_pushed == true) {
            microkit_notify(Server_CH);
        }
        /*
          This for loop check if there are coroutines finished and send the result back
        */
        New_request_pushed = false;
        for (i = 1; i < MAX_COROUTINE_NUM; i++) {
            if (RequestPool[i].handle == INVALID_COHANDLE && RequestPool[i].stat == INUSE) {
                message.completion.request_id = RequestPool[i].request_id;
                Fill_Client_Response(&message, &(RequestPool[i]));
                sddf_fs_queue_push(FATfs_completion_queue, message);
                RequestPool[i].stat= FREE;
                Client_have_replies = true;
            }
        }
        /*
          This should pop the request from the command_queue to the FiberPool to execute, if no new request is 
          popped, we should exit the whole while loop.
        */
        while (true) {
            index = FiberPool_FindFree();
            if (index == INVALID_COHANDLE || sddf_fs_queue_isEmpty(FATfs_command_queue) 
                  || sddf_fs_queue_isFull(FATfs_completion_queue)) {
               break;
            }
            sddf_fs_queue_pop(FATfs_command_queue, &message);
            SetUp_request(index, message);
            RequestPool[index].stat = INUSE;
            New_request_pushed = true;
        }
    }
    if (Client_have_replies == true) {
        microkit_notify(Client_CH);
    }
}