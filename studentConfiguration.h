#ifndef STUDENTCONFIGURATION_H_
#define STUDENTCONFIGURATION_H_
/****************************************************************************
    StudentConfiguration.h
    Of all the files given to you at the start of this project, this
    is the ONLY one you should ever modify.

      4.30 Jan 2016           StudentConfiguration.h created
      4.50 Jan 2018           Automatically configure for underlying OS
****************************************************************************/
/****************************************************************************
    Choose one of the operating systems below
****************************************************************************/

#ifdef __unix
#define LINUX
#endif
#ifdef _WIN32
#define WINDOWS
#endif
#ifdef __APPLE__
#define MAC
#endif


//some constant about file management
#define file_type 0
#define dir_type 1
#define root_type 2
#define BIT_MAP_SIZE    4
#define ROOT_DIR_SIZE   2
#define SWAP_SIZE       240
#define BIT_MAP_LOC     1
#define ROOT_DIR_LOC    BIT_MAP_LOC + BIT_MAP_SIZE * 4
#define SWAP_LOC  ROOT_DIR_LOC+ROOT_DIR_SIZE
#define NORMAL_SEC_LOC SWAP_LOC+SWAP_SIZE*4
#define BLOCK_BIT  128
#define BYTE_BIT  8
#define MASK_LSF        255
#define MASK_MSF        65280
#define MASK_SSF        1671180
#define SWAP_NUM 960
#define MAX_CUR_FILE 8

typedef struct
{
    char bytes[16];
} Block;



//a Process Control Block
typedef struct p_c_b
{
    char P_Name[16];
    long P_Id;
    long P_Pri;
    long P_Status;
    long P_Time;
    long P_Disk;
    long P_Disk_Wait;
    void* P_Buffer;
    long P_Context;
    short *P_PageTable;
    long cur_dir;
    long cur_file;
    int bit_map_change_sector[BIT_MAP_SIZE*4];
    long P_Sector;
}PCB;





typedef struct
{
    Block data[BIT_MAP_SIZE * 4];
}bitmap;


// The number of bytes in a page
#define    PGSIZE       (short)16
//routines
    
    

void start_timer(long Time);
void Sleep(long TimeToSleep);
void create_process(char* ProcessName,void* StartingAdress,long InitialPriority,long * ProcessID, long* ErrorReturned);
void switch_context(PCB* pcb);
void resume_process(long ProcessID, long* ErrorReturned);
void physical_disk_read(long DiskID, long Sector, char ReadBuffer[PGSIZE]);
void physical_disk_write(long DiskID, long Sector, char WirteBuffer[PGSIZE]);
void dispatcher(void);
void dispatcher_M(void);
void change_priority(long ProcessID,long NewPriority,long* ErrorReturned);
void suspend_process(long ProcessID, long* ErrorReturned);
void terminate_process(long ProcessID,long * ErrorReturned);
void create_context(PCB* pcb,void* Code);
void get_process_id(char* ProcessName, long* ProcessID,long* ErrorReturned);
void scheduler_printer(long current_pid,long target_pid,char  action[10]);
void memory_printer(void);
void svc_handler_add(short call_type,long const *const Err);
void check_disk(long DiskID,long* ErrorReturned);
void format(long DiskID,long*ErrorReturned);
void initial_bit_map(void);
void update_bit_map(long sector_num);
long initial_header(char* name,int type);
void put_bit_map_in_disk(void);
int get_next_free_sec(void);
void initial_index(long index_loc);
int page_replace(void);
void initial_block0();
void update_index(long index_loc,long updated_loc,long updated_content);
long create_dir_or_file(char* Name, long* ErrorReturned,long type);
void close_file(long Inode, long* ErrorReturned);
void read_file(long Inode, long FileLogicalBlock, char* ReadBuffer, long* ErrorReturned);
void write_file(long Inode, long FileLogicalBlock, char* ReadBuffer, long* ErrorReturned);
void frame_fault_handler(long fault_virtual_frame_id);
void open_file(char* FileName,long *Inode,long* ErrorReturned);
void open_dir(long DiskID_OR_Minus1,char* DirectoryName,long* ErrorReturned);
void dir_contents(long* ErrorReturned);
long check_in_index(long disk_num,long cur_dir_sector,char* name,int logical_loc);
int get_next_free_swap(void);
PCB* get_current_pcb(void);
void start_context(PCB* pcb);
void suspend_context();














#define READY_LOCK MEMORY_INTERLOCK_BASE
#define TIMER_LOCK MEMORY_INTERLOCK_BASE+1
#define RUNNING_LOCK MEMORY_INTERLOCK_BASE+2
#define DISK_LOCK_BASE MEMORY_INTERLOCK_BASE+3
#define SUSPEND_LOCK MEMORY_INTERLOCK_BASE+11
#define BITMAP_LOCK MEMORY_INTERLOCK_BASE+12
#define FRAME_LOCK MEMORY_INTERLOCK_BASE+13

typedef  struct
{
    long head_loc;
    long index_loc;
}directory;

#define wait_for_read 1
#define wait_for_write 2


#define MAX_NUMBER_OF_PROCESS 26

typedef struct
{
   char data[PGSIZE];
   int status;
}SwapArea;

typedef struct
{
    struct p_c_b* pcb;
    long virtual_id;
}FrameInfo;

//scheduler printer mode
#define None 1L
#define Full 2L
#define Limited 3L
#define Initial 4L

//process state
#define running 0
#define ready 1
#define suspended 2
#define waiting_for_timer 3
#define waiting_for_disk 4
#define waiting_for_timer_and_suspended 5
#define waiting_for_disk_and_suspended 6
#define terminated 7
#define original 8

//ErrorType
#define ERR_PID_NO_EXIST 1L
#define ERR_PNAME_DUPLICATE 2L
#define ERR_PRIORITY_ILLEGAL 3L
#define ERR_START_ADRESS_ILLEGAL -4
#define ERR_PROCESS_NUMBER_REACH_MAX 4L
#define ERR_NOT_SUSPENDED 5L
#define ERR_ALREADY_SUSPENDED 6L
#define ERR_IN_DISKQUEUE 7L
#define ERR_IN_TIMERQUEUE 8L
#define ERR_PNAME_NO_EXIST 9L
#define ERR_DISKID_ILLEGAL 10L
#define ERR_DISK_NO_FORMAT 11L
#define ERR_NAME_WRONG 12L
#define ERR_ALREADY_ROOT 13L
#define ERR_NO_CUR_DIR 14L
#define ERR_CUR_FILE_MAX 15L
#define ERR_INODE_WRONG 16L
#define ERR_FILE_LOGIC_NUM_ILLEGAL 17L
#define ERR_NO_CONTENT 18L
#define ERR_DISK_NO_ROOM 19L
#define ERR_ALREADY_EXIST 20L
#define  ERR_CUR_DIR_FULL 21L
#define  ERR_NO_EXIST 22L







/*****************************************************************
    The next five defines have special meaning.  They allow the
    Z502 processor to report information about its state.  From
    this, you can find what the hardware thinks is going on.
    The information produced when this debugging is on is NOT
    something that should be handed in with the project.
    Change FALSE to TRUE to enable a feature.
******************************************************************/
#define         DO_DEVICE_DEBUG                 FALSE
#define         DO_MEMORY_DEBUG                 FALSE

//  These three are very useful for my debugging the hardware,
//  but are probably less useful for students.
#define         DEBUG_LOCKS                     FALSE
#define         DEBUG_CONDITION                 FALSE
#define         DEBUG_USER_THREADS              FALSE

#endif /* STUDENTCONFIGURATION_H_ */

