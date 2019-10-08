/************************************************************************

 This code forms the base of the operating system you will
 build.  It has only the barest rudiments of what you will
 eventually construct; yet it contains the interfaces that
 allow test.c and z502.c to be successfully built together.

 Revision History:
 1.0 August 1990
 1.1 December 1990: Portability attempted.
 1.3 July     1992: More Portability enhancements.
 Add call to SampleCode.
 1.4 December 1992: Limit (temporarily) printout in
 interrupt handler.  More portability.
 2.0 January  2000: A number of small changes.
 2.1 May      2001: Bug fixes and clear STAT_VECTOR
 2.2 July     2002: Make code appropriate for undergrads.
 Default program start is in test0.
 3.0 August   2004: Modified to support memory mapped IO
 3.1 August   2004: hardware interrupt runs on separate thread
 3.11 August  2004: Support for OS level locking
 4.0  July    2013: Major portions rewritten to support multiple threads
 4.20 Jan     2015: Thread safe code - prepare for multiprocessors
 ************************************************************************/

#include             "global.h"
#include             "syscalls.h"
#include             "protos.h"
#include             "string.h"
#include             <stdlib.h>
#include             <ctype.h>

#include  <unistd.h>

PCB* Process_Table[MAX_NUMBER_OF_PROCESS];
int readyqueue;
int timerqueue;
int suspendqueue;
int runningqueue;
int diskqueue0;
int diskqueue1;
int diskqueue2;
int diskqueue3;
int diskqueue4;
int diskqueue5;
int diskqueue6;
int diskqueue7;
int diskqueue_name[8];

directory* inode_table[MAX_NUMBER_OF_DISKS][MAX_NUMBER_INODES];
BOOL hasformated=-1;
bitmap bit_map[8];
int initialthread=TRUE;

SwapArea swaparea[SWAP_NUM];
FrameInfo FrameTable[NUMBER_PHYSICAL_PAGES];
BOOL IsM=FALSE;

long my_clock=0;
long ava_frame_count=0;
long sp_mode=-1;
long handler=-1;
long mp_mode=-1;
char* hardware_err_names[]={"success","bad_param","no_previous_write","illegal_address","disk_in_use","bad_device_id",
                            "no_device_found","device_in_use","device_free"};

char* err_names[]={"success","pid_no_exist","pname_duplicate","pri_illegal","pro_num_max","pro_not_suspended",
                   "pro_already_suspended,","pro_in_diskqueue","pro_in_timerqueue","pname_no_exist","diskid_illegal","disk_no_format",
                   "name_error","already_in_root","no_cur_dir","cur_file_max","inode_num_wrong","file_logic_num_wrong","no_content",
                   "disk_no_room","already_exist","cur_dir_full","dir_or_file_no_exist"};



//  Allows the OS and the hardware to agree on where faults occur
extern void *TO_VECTOR[];

char *call_names[] = {       "MemRead  ", "MemWrite ", "ReadMod  ", "GetTime  ",
		"Sleep    ", "GetPid   ", "Create   ", "TermProc ", "Suspend  ",
		"Resume   ", "ChPrior  ", "Send     ", "Receive  ", "PhyDskRd ",
		"PhyDskWrt", "DefShArea", "Format   ", "CheckDisk", "OpenDir  ",
		"OpenFile ", "CreaDir  ", "CreaFile ", "ReadFile ", "WriteFile",
		"CloseFile", "DirContnt", "DelDirect", "DelFile  " };
char *device_name[]={"","","","","Timer","Disk0","Disk1","Disk2","Disk3","Disk4","Disk5","Disk6","Disk7"};
/************************************************************************
 INTERRUPT_HANDLER
 When the Z502 gets a hardware interrupt, it transfers control to
 this routine in the Operating System.
 NOTE WELL:  Just because the timer or the disk has interrupted, and
			 therefore this code is executing, it does NOT mean the
		 action you requested was successful.
		 For instance, if you give the timer a NEGATIVE time - it
		 doesn't know what to do.  It can only cause an interrupt
		 here with an error.
		 If you try to read a sector from a disk but that sector
		 hasn't been written to, that disk will interrupt - the
		 data isn't valid and it's telling you it was confused.
		 YOU MUST READ THE ERROR STATUS ON THE INTERRUPT
 ************************************************************************/
void InterruptHandler(void)
{
	INT32 DeviceID;
	INT32 Status;
    long current_time;
	MEMORY_MAPPED_IO mmio;       // Enables communication with hardware
    PCB* temp=NULL;
    PCB* temp_next=NULL;
	long disk_num;
	INT32 lock_result;
	static BOOL  remove_this_from_your_interrupt_code = TRUE; /** TEMP **/
	static INT32 how_many_interrupt_entries = 0;              /** TEMP **/
	// Get cause of interrupt
	mmio.Mode = Z502GetInterruptInfo;
	mmio.Field1 = mmio.Field2 = mmio.Field3 = mmio.Field4 = 0;
	MEM_READ(Z502InterruptDevice, &mmio);
	DeviceID = mmio.Field1;
	Status = mmio.Field2;
 
	
	if (mmio.Field4 != ERR_SUCCESS) {
		aprintf( "The InterruptDevice call in the InterruptHandler has failed.\n");
		aprintf("The DeviceId and Status that were returned are not valid.\n");
		return;
	}
	//Timer Interrupt
	if(DeviceID==TIMER_INTERRUPT)
    {
	    mmio.Mode=Z502ReturnValue;
        mmio.Field1=mmio.Field2=mmio.Field3=0;
        MEM_READ(Z502Clock,&mmio);
        current_time=mmio.Field1;
	   
        //take out all the process that should be awaken according to the time
        //if there is still process, start new timer
        while(TRUE)
        {
            READ_MODIFY(TIMER_LOCK,1,TRUE,&lock_result);
            if(QNextItemInfo(timerqueue)!=(void*)-1 && ((PCB*)QNextItemInfo(timerqueue))->P_Time<=current_time)
            {
                temp=(PCB*)QRemoveHead(timerqueue);
                temp->P_Status=ready;
                READ_MODIFY(TIMER_LOCK,0,TRUE,&lock_result);
                scheduler_printer(0,temp->P_Id,"remove_timer");
                READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
                QInsert(readyqueue,(unsigned int)temp->P_Pri,temp);
                READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
            }
            else
                break;
        }
        
        //if there are still other pcb in the timerqueue, start new timer
        temp_next=(PCB*)QNextItemInfo(timerqueue);
        READ_MODIFY(TIMER_LOCK,0,TRUE,&lock_result);
        if(temp_next!=(void*)-1)
            start_timer(temp_next->P_Time-current_time);
    }
    else
    {
        //disk interrupt
        //get the disk id
        disk_num=DeviceID-DISK_INTERRUPT;
        
        //take out the first pcb in the corresponding disk queue
        READ_MODIFY(DISK_LOCK_BASE+(INT32)disk_num,1,TRUE,&lock_result);
        temp=(PCB*)QRemoveHead(diskqueue_name[disk_num]);
        temp->P_Disk_Wait=FALSE;
        temp->P_Buffer=NULL;
        temp_next=QNextItemInfo(diskqueue_name[disk_num]);
        temp->P_Status=ready;
        READ_MODIFY(DISK_LOCK_BASE+(INT32)disk_num,0,TRUE,&lock_result);
        scheduler_printer(0,temp->P_Id,"remove_disk");
        
        //get the head of the disk queue
        READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
        QInsert(readyqueue,(unsigned int)temp->P_Pri,temp);
        READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
        
        //if there is still other pcb, do a new disk
        if(temp_next!=(void*)-1)
        {
            mmio.Mode =(temp_next->P_Disk_Wait==wait_for_read? Z502DiskRead:Z502DiskWrite);
            mmio.Field1 = disk_num;
            mmio.Field2 = temp_next->P_Sector;
            mmio.Field3 = (long) temp_next->P_Buffer;
            MEM_WRITE(Z502Disk, &mmio);
        }
        
    }
	//whether print handler info
    if(handler==None)
        return;
    else
    {
        if(handler==Initial)
            how_many_interrupt_entries++;
        if (remove_this_from_your_interrupt_code && (how_many_interrupt_entries < 10))
        {
            aprintf("InterruptHandler: Found device %s with status %s\n",
                    device_name[DeviceID], hardware_err_names[Status]);
        }
	}
}           // End of InterruptHandler

/************************************************************************
 FAULT_HANDLER
 The beginning of the OS502.  Used to receive hardware faults.
 ************************************************************************/

void FaultHandler(void)
{
	INT32 DeviceID;
	MEMORY_MAPPED_IO mmio;       // Enables communication with hardware
	static BOOL remove_this_from_your_fault_code = TRUE; /** TEMP **/
	static INT32 how_many_fault_entries = 0; /** TEMP **/
	// Get cause of fault
	mmio.Field1 = mmio.Field2 = mmio.Field3 = 0;
	mmio.Mode = Z502GetInterruptInfo;
	MEM_READ(Z502InterruptDevice, &mmio);
	DeviceID = mmio.Field1;
 
	INT32 Status;
	Status = mmio.Field2;
    
    if (mmio.Field4 != ERR_SUCCESS)
    {
        aprintf( "The InterruptDevice call in the InterruptHandler has failed.\n");
        aprintf("The DeviceId and Status that were returned are not valid.\n");
        return;
    }
	// This causes a print of the first few faults - and then stops printing!
	how_many_fault_entries++;
	
	if (remove_this_from_your_fault_code && (how_many_fault_entries < 10)) {
		aprintf("\nFaultHandler: Found device ID %d with status %d\n",
				(int) mmio.Field1, (int) mmio.Field2);
	}
	
    if (Status >= NUMBER_VIRTUAL_PAGES){
        aprintf("\nTry to visit illegal adress, simulation halt\n");
        mmio.Mode = Z502Action;
        mmio.Field1 = mmio.Field2 = mmio.Field3 = 0;
        MEM_WRITE(Z502Halt, &mmio);
        return;
    }
    //call frame_fault_handler() to fix the frame problem
	frame_fault_handler(Status);
    memory_printer();
} // End of FaultHandler

/************************************************************************
 get_current_pcb
 This function is used to get current pcb in multi-processor mode.
 
 Input: NULL
 Output: current pcb
************************************************************************/
PCB* get_current_pcb(void)
{
    //using memorymappedio to get current context
    MEMORY_MAPPED_IO mmio;
    mmio.Mode=Z502GetCurrentContext;
    mmio.Field1=mmio.Field2=mmio.Field3=0;
    MEM_READ(Z502Context,&mmio);
    
    //search context in process_table to get current pcn
    for(int i=1;i<MAX_NUMBER_OF_PROCESS;i++)
        if(Process_Table[i] && Process_Table[i]->P_Context==mmio.Field1)
            return Process_Table[i];
    return NULL;
}


/************************************************************************
 create_context
 This function is used to initialize context, we can get context address
 and associate page table with corresponding thread.
 
 Input: pcb:  the address of the pcb to be initialized
        Code: the address of the code to be execuated
 Output: NULL
************************************************************************/
void create_context(PCB* pcb,void* Code)
{
    //create context and attached it in pcb
    MEMORY_MAPPED_IO mmio;
    void *PageTable = calloc(2, NUMBER_VIRTUAL_PAGES);
    mmio.Mode = Z502InitializeContext;
    mmio.Field1 = 0;
    mmio.Field2 = (long) Code;
    mmio.Field3 = (long) PageTable;
    MEM_WRITE(Z502Context, &mmio);
    
    //associate the pagetable and context with targeted PCB
    pcb->P_PageTable = PageTable;
    pcb->P_Context = mmio.Field1;
}

/************************************************************************
 swtich_context
 This function is used to switch context. It uses the mode
 "START_NEW_CONTEXT_AND_SUSPEND", it is only used in single processor mode
 
 
 Input: pcb: the address of the pcb to be start
 Output: NULL
************************************************************************/
void switch_context(PCB* pcb)
{
    //start new context and suspend old one
    MEMORY_MAPPED_IO mmio;
    mmio.Mode=Z502StartContext;
    mmio.Field1=pcb->P_Context;
    mmio.Field2=START_NEW_CONTEXT_AND_SUSPEND;
    mmio.Field3=mmio.Field4=0;
    MEM_WRITE(Z502Context,&mmio);
}


/************************************************************************
 suspend_context
 This function is used to suspend context. It uses the mode
 "SUSPEND_CURRENT_CONTEXT_ONLY", it is only used in multi-processor mode
 to make a process suspend itself when it is doing timer or disk
 
 Input: NULL
 Output: NULL
************************************************************************/
void suspend_context()
{
    //just suspend current context, this is used in multi-processor system
    MEMORY_MAPPED_IO mmio;
    mmio.Mode=Z502StartContext;
    mmio.Field1=NULL;
    mmio.Field2=SUSPEND_CURRENT_CONTEXT_ONLY;
    mmio.Field3=mmio.Field4=0;
    MEM_WRITE(Z502Context,&mmio);
}


/************************************************************************
 start_context
 This function is used to suspend context. It uses the mode
 "START_NEW__CONTEXT_ONLY", it is only used in multi-processor mode
 to start a new process, cause we have enough processor, we don't need to
 suspend other process.
 
 Input: pcb:  the pcb to be started
 Output: NULL
************************************************************************/

void start_context(PCB* pcb)
{
    //just start new context, this is used in multi-processor system
    MEMORY_MAPPED_IO mmio;
    mmio.Mode=Z502StartContext;
    mmio.Field1=pcb->P_Context;
    mmio.Field2=START_NEW_CONTEXT_ONLY;
    mmio.Field3=mmio.Field4=0;
    MEM_WRITE(Z502Context,&mmio);
}

/************************************************************************
 memory_printer
 
 
 Input: NULL
 Output: NULL
************************************************************************/

void memory_printer(void)
{
    INT32 lock_result;
    MP_INPUT_DATA* M_P=(MP_INPUT_DATA*)calloc(1,sizeof(MP_INPUT_DATA));
    static short mp_counter=50;
    if(mp_mode==None)
        return;
    if(mp_counter>=0)
    {
        READ_MODIFY(FRAME_LOCK, 1, TRUE, &lock_result);
        for (int i = 0; i < NUMBER_PHYSICAL_PAGES; i++) {
            if (FrameTable[i].pcb == NULL) {
                M_P->frames[i].InUse = FALSE;
            }
            else {
                M_P->frames[i].InUse = TRUE;
                M_P->frames[i].Pid = FrameTable[i].pcb->P_Id;
                M_P->frames[i].LogicalPage = FrameTable[i].virtual_id;
                M_P->frames[i].State = 7;
            }
        }
        READ_MODIFY(FRAME_LOCK, 0, TRUE, &lock_result);
    
        MPPrintLine(M_P);
    }
    if(mp_mode==Limited)
        mp_counter--;
    free(M_P);
}
/************************************************************************
 scheduler_printer
 This function is used to print scheduler information, it is usually called
 when there is some change in readyqueue or some process is terminate
 
 Input: current_pid: the action is taken by which process
        target_pid: the action is taken on which process
        action: what action to take
 Output: NULL
************************************************************************/

void scheduler_printer(long current_pid,long target_pid,char action[SP_LENGTH_OF_ACTION+2])
{
    INT32 lock_result;
    PCB* temp;
    static short sp_counter=50;
    
    //whether do print
    if(sp_mode==None)
        return;
    
    //do some initialize
    SP_INPUT_DATA* SP_Print=(SP_INPUT_DATA*)calloc(1,sizeof(SP_INPUT_DATA));
    SP_Print->NumberOfProcSuspendedProcesses=SP_Print->NumberOfDiskSuspendedProcesses=0;
    SP_Print->NumberOfReadyProcesses=SP_Print->NumberOfTimerSuspendedProcesses=0;
    SP_Print->NumberOfTerminatedProcesses=SP_Print->NumberOfMessageSuspendedProcesses= SP_Print->NumberOfRunningProcesses=0;
    
    //check whether need to continue printing
    if(sp_counter>0)
    {
        
        //deliver the current_pid target_pid action to sp_print
        SP_Print->CurrentlyRunningPID=(INT16)current_pid;
        SP_Print->TargetPID=(INT16)target_pid;
        memcpy(SP_Print->TargetAction,action,SP_LENGTH_OF_ACTION+2);
        
        //get information from readyqueue
        READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
        while(TRUE)
        {
            temp=(PCB*)QWalk(readyqueue,SP_Print->NumberOfReadyProcesses);
            if(temp==(void*)-1)
                break;
            SP_Print->ReadyProcessPIDs[SP_Print->NumberOfReadyProcesses++]=(INT16)temp->P_Id;
        }
        READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
    
        //get information from timerqueue
        READ_MODIFY(TIMER_LOCK,1,TRUE,&lock_result);
        while(TRUE)
        {
            temp=(PCB*)QWalk(timerqueue,SP_Print->NumberOfTimerSuspendedProcesses);
            if(temp==(PCB*)-1)
                break;
            SP_Print->TimerSuspendedProcessPIDs[SP_Print->NumberOfTimerSuspendedProcesses++]=(INT16)temp->P_Id;
        }
        READ_MODIFY(TIMER_LOCK,0,TRUE,&lock_result);
    
        //get information from suspendqueue
        READ_MODIFY(SUSPEND_LOCK,1,TRUE,&lock_result);
        while(TRUE)
        {
            temp=(PCB*)QWalk(suspendqueue,SP_Print->NumberOfProcSuspendedProcesses);
            if(temp==(PCB*)-1)
                break;
            SP_Print->ProcSuspendedProcessPIDs[SP_Print->NumberOfProcSuspendedProcesses++]=(INT16)temp->P_Id;
        }
        READ_MODIFY(SUSPEND_LOCK,0,TRUE,&lock_result);
    
    
        //get information from diskqueue
        READ_MODIFY(DISK_LOCK_BASE,1,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+1,1,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+2,1,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+3,1,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+4,1,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+5,1,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+6,1,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+7,1,TRUE,&lock_result);
        for(int i=0;i<7;i++)
        {
            while(TRUE)
            {
                temp=(PCB*)QWalk(diskqueue_name[i],SP_Print->NumberOfDiskSuspendedProcesses);
                if(temp==(PCB*)-1)
                    break;
                SP_Print->DiskSuspendedProcessPIDs[SP_Print->NumberOfDiskSuspendedProcesses++]=(INT16)temp->P_Id;
            }
        }
        READ_MODIFY(DISK_LOCK_BASE,0,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+1,0,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+2,0,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+3,0,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+4,0,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+5,0,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+6,0,TRUE,&lock_result);
        READ_MODIFY(DISK_LOCK_BASE+7,0,TRUE,&lock_result);
        
        
        //do the print
        SPPrintLine(SP_Print);
        
        if(sp_mode==Limited)
            sp_counter--;
        
        //free SP_Print, avoid memory loss.
        free(SP_Print);
    }
}

/************************************************************************
 create_process
 This function is used to create process. The moves to take when creating
 process are different depending on whether in multi-processor mode or
 single-processor mode, depending on whether this is the first user thread
 
 Input: ProcessName: the name of the process to be created
        StartingAddress: address of the code to be execuated by the new process
        InitialPriority: the priority of the new process
        ProcessID: store the process id assigned to the process
        ErrorReturned: handle error
 Output: NULL
************************************************************************/

void create_process(char* ProcessName,void* StartingAdress,long InitialPriority,long * ProcessID, long* ErrorReturned)
{
    int p_id=-1;
    INT32 lock_result;
    
    //priority illegal
    if(InitialPriority<=0)
    {
        *ErrorReturned=ERR_PRIORITY_ILLEGAL;
        return;
    }
    else
    {
        //check whether name already existed or no thread for new process
        for(int i=1;i<MAX_NUMBER_OF_PROCESS;i++)
        {
            if(Process_Table[i]==NULL) {
                if (p_id == -1)
                    p_id = i;
            }
            else if(strcmp(Process_Table[i]->P_Name,ProcessName)==0)
            {
                *ErrorReturned = ERR_PNAME_DUPLICATE;
                return;
            }
        }
        
        //too many process already
        if(p_id==-1)
        {
            *ErrorReturned=ERR_PROCESS_NUMBER_REACH_MAX;
            return;
        }
        
        
        *ProcessID=p_id;
        
        //initialize some values for pcb
        PCB* NEW_PCB=(PCB*)(calloc(1,sizeof(PCB)));
        NEW_PCB->P_Id=p_id;
        memcpy(NEW_PCB->P_Name,ProcessName,16);
        NEW_PCB->P_Pri=InitialPriority;
        NEW_PCB->P_Disk=-1;
        NEW_PCB->cur_dir=-1;
        Process_Table[p_id]=NEW_PCB;
        
        *ErrorReturned=ERR_SUCCESS;
        
        //create context for new pcb
        create_context(NEW_PCB,StartingAdress);
        
        //if the process is the initial process
        if(initialthread==TRUE)
        {
            initialthread=FALSE;
            
            //in multi-processor mode, only start new process
            if(IsM==TRUE)
            {
                start_context(NEW_PCB);
            }
            
            //in single-processor mode, start the new process and stop the initial one
            else
            {
                NEW_PCB->P_Status = running;
                READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
                QInsert(runningqueue, FALSE, NEW_PCB);
                READ_MODIFY(RUNNING_LOCK, 0, TRUE, &lock_result);
                switch_context(NEW_PCB);
            }
            
        }
        //the current thread is not the initial thread
        else
        {
            //in multi-processor mode, just start the new process
            if(IsM==TRUE)
            {
                NEW_PCB->P_Status=running;
                start_context(NEW_PCB);
                return;
            }
            
            //in single-processor mode, put the pcb in readyqueue
            READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
            NEW_PCB->P_Status=ready;
            QInsert(readyqueue,(unsigned int)InitialPriority,NEW_PCB);
            READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
            
            //scheduler_printer
            scheduler_printer(((PCB*)QNextItemInfo(runningqueue))->P_Id,NEW_PCB->P_Id,"Create");
        }
    }
}

/************************************************************************
 Sleep
 The function is used to sleep the current process
 
 Input: TimeToSleep: the time to sleep
 Output: NULL
************************************************************************/



void Sleep(long TimeToSleep)
{
    MEMORY_MAPPED_IO mmio;
    PCB* temp;
    INT32 lock_result;
    long current_time;
    
    //time illegal
    if(TimeToSleep<=0)
    {
        return;
    }
    else
    {
        //get the current time
        mmio.Mode=Z502ReturnValue;
        mmio.Field1=mmio.Field2=mmio.Field3=0;
        MEM_READ(Z502Clock,&mmio);
        current_time=mmio.Field1;
        
        //calculate the estimated wake time
        long Time=current_time+TimeToSleep;
        
        
        //get the current pcb
        if(IsM==TRUE)
            temp=get_current_pcb();
        else
        {
            READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
            temp = (PCB *) QRemoveHead(runningqueue);
            READ_MODIFY(RUNNING_LOCK, 0, TRUE, &lock_result);
        }
        
        temp->P_Time=Time;
        temp->P_Status=waiting_for_timer;
        
        //put the process in timer queue according to wake time
        READ_MODIFY(TIMER_LOCK,1,TRUE,&lock_result);
        QInsert(timerqueue,(unsigned int)Time,temp);
        
        //if the process wake up earlier than others, start a new timer
        if(QNextItemInfo(timerqueue)==temp)
        {
            start_timer(TimeToSleep);
        }
        READ_MODIFY(TIMER_LOCK,0,TRUE,&lock_result);
        
        //if in single-process mode, do dispatcher
        if(IsM==FALSE)
        {
            CALL(dispatcher());
        }
        
        //if in multi-process mode, suspend the current process
        else
        {
            suspend_context();
        }
        return;
    }
}

/************************************************************************
 start_time
 This function is used to call MemoryMappedIo to start a new timer
 
 Input: Time: the time to sleep
 Output: NULL
************************************************************************/

void start_timer(long Time)
{
    MEMORY_MAPPED_IO mmio;
    mmio.Mode=Z502Start;
    mmio.Field1=Time;
    mmio.Field2=mmio.Field3=mmio.Field4=0;
    MEM_WRITE(Z502Timer,&mmio);
}


/************************************************************************
 change_priority
 This function is used to change priority of a process according to process
 id, this process could be in any states, do different operation when the
 process in different states
 Input: ProcessId: the id of the process to be changed
        NewPriority: new priority
        ErrorReturned: handle error
 Output: NULL
************************************************************************/

void change_priority(long ProcessID,long NewPriority,long* ErrorReturned)
{
    PCB* temp;
    INT32 lock_result;
    
    //new priority illegal
    if(NewPriority<=0)
    {
        *ErrorReturned=ERR_PRIORITY_ILLEGAL;
        return;
    }
    
    //process id illegal
    if(ProcessID<-1||ProcessID>=MAX_NUMBER_OF_PROCESS)
    {
        *ErrorReturned=ERR_PID_NO_EXIST;
        return;
    }
    
    //the targeted process is current process
    else if(ProcessID==-1)
    {
        READ_MODIFY(RUNNING_LOCK,1,TRUE,&lock_result);
        temp = QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK,0,TRUE,&lock_result);
    }
    
    //targeted process no exist
    else if(Process_Table[ProcessID]==NULL)
    {
        *ErrorReturned=ERR_PID_NO_EXIST;
        return;
    }
    else
        temp=Process_Table[ProcessID];
    
    //assign new priority
    temp->P_Pri=NewPriority;
    
    //targeted process in ready queue
    if(temp->P_Status==ready)
    {
        //take it out and put it again in ready queue
        READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
        QRemoveItem(readyqueue,temp);
        QInsert(readyqueue,(unsigned int)temp->P_Pri,temp);
        READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
        
        //scheduler printer
        scheduler_printer(((PCB*)QNextItemInfo(runningqueue))->P_Id,temp->P_Id,"Change_pri");
    }
    *ErrorReturned=ERR_SUCCESS;
}


/************************************************************************
 suspend_process
 This function is used to suspend a process, different operations will be
 taken when targeted process in different states
 
 Input: ProcessId: the id of the process to be suspended
        ErrorReturned: handle error
 Output: NULL
************************************************************************/

void suspend_process(long ProcessID, long* ErrorReturned)
{
    PCB* temp;
    INT32 lock_result;
    
    //process id illegal
    if(ProcessID<-1||ProcessID>=MAX_NUMBER_OF_PROCESS)
    {
        *ErrorReturned=ERR_PID_NO_EXIST;
        return;
    }
    
    //the targeted process is current process
    else if(ProcessID==-1)
    {
        READ_MODIFY(RUNNING_LOCK,1,TRUE,&lock_result);
        temp=(PCB*)QRemoveHead(runningqueue);
        READ_MODIFY(RUNNING_LOCK,0,TRUE,&lock_result);
        temp->P_Status=suspended;
        READ_MODIFY(SUSPEND_LOCK,1,TRUE,&lock_result);
        QInsert(suspendqueue,FALSE,temp);
        READ_MODIFY(SUSPEND_LOCK,0,TRUE,&lock_result);
        scheduler_printer(temp->P_Id,temp->P_Id,"Suspend");
        CALL(dispatcher());
        return;
    }
    else
    {
        temp=Process_Table[ProcessID];
        
        //process not exist
        if(temp==NULL)
        {
            *ErrorReturned=ERR_PID_NO_EXIST;
            return;
        }
        
        //process is already suspended
        else if(temp->P_Status==suspended)
        {
            *ErrorReturned=ERR_ALREADY_SUSPENDED;
            return;
        }
        
        //process is waiting for disk
        else if(temp->P_Status==waiting_for_disk||temp->P_Status==waiting_for_disk_and_suspended)
        {
            temp->P_Status=waiting_for_disk_and_suspended;
            *ErrorReturned=ERR_IN_DISKQUEUE;
            return;
        }
        
        //process is waiting for timer
        else if(temp->P_Status==waiting_for_timer||temp->P_Status==waiting_for_timer_and_suspended)
        {
            temp->P_Status=waiting_for_timer_and_suspended;
            *ErrorReturned=ERR_IN_TIMERQUEUE;
            return;
        }
    }
    
    //process in ready queue
    temp->P_Status=suspended;
    
    //remove it out from readyqueue
    READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
    QRemoveItem(readyqueue,temp);
    READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
    
    //put it in suspendqueue
    READ_MODIFY(SUSPEND_LOCK,1,TRUE,&lock_result);
    QInsert(suspendqueue,FALSE,temp);
    READ_MODIFY(SUSPEND_LOCK,0,TRUE,&lock_result);
    
    //scheduler printer
    scheduler_printer(((PCB*)QNextItemInfo(runningqueue))->P_Id,temp->P_Id,"Suspend");
    *ErrorReturned=ERR_SUCCESS;
}
/************************************************************************
 resume_process
 This function is used to resume a process. it does different operations
 when targeted process in different states
 
 
 Input: ProcessID: the id of targeted process
        ErrorReturned: error control
 Output: NULL
************************************************************************/

void resume_process(long ProcessID, long* ErrorReturned)
{
    PCB* temp;
    INT32 lock_result;
    
    //process id illegal
    if(ProcessID<=0||ProcessID>=MAX_NUMBER_OF_PROCESS)
    {
        *ErrorReturned=ERR_PID_NO_EXIST;
        return;
    }
    //process no exist
    else if(Process_Table[ProcessID]==NULL)
    {
        *ErrorReturned=ERR_PID_NO_EXIST;
        return;
    }
    //process is not suspended right now
    else if(Process_Table[ProcessID]->P_Status!=suspended)
    {
        *ErrorReturned=ERR_NOT_SUSPENDED;
    }
    
    else
    {
        
        *ErrorReturned=ERR_SUCCESS;
        
        //get the pcb from suspend queue
        READ_MODIFY(SUSPEND_LOCK,1,TRUE,&lock_result);
        temp=(PCB*)QRemoveItem(suspendqueue,Process_Table[ProcessID]);
        temp->P_Status=ready;
        READ_MODIFY(SUSPEND_LOCK,0,TRUE,&lock_result);
        
        //scheduler printer
        READ_MODIFY(RUNNING_LOCK,1,TRUE,&lock_result);
        scheduler_printer(((PCB*)QNextItemInfo(runningqueue))->P_Id,ProcessID,"Resume");
        READ_MODIFY(RUNNING_LOCK,0,TRUE,&lock_result);
        
        //insert the pcb in readyqueue
        READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
        QInsert(readyqueue,(unsigned int)temp->P_Pri,temp);
        READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
    }
}
/************************************************************************
 check_disk
 This function is used to check disk and output the result.
 
 
 Input: DiskID: the disk number to be checked
        ErrorReturned: error control
 Output: NULL
************************************************************************/

void check_disk(long DiskID,long* ErrorReturned)
{
    //use MemoryMappedIo to do the job
    MEMORY_MAPPED_IO mmio;
    mmio.Mode = Z502CheckDisk;
    mmio.Field1=DiskID;
    mmio.Field2=mmio.Field3=mmio.Field4=0;
    MEM_WRITE(Z502Disk,&mmio);
    if(mmio.Field4==ERR_BAD_PARAM)
        *ErrorReturned=ERR_DISKID_ILLEGAL;
    else
        *ErrorReturned=ERR_SUCCESS;
}

/************************************************************************
 physical_disk_read
 This function is used to read a certain sector in a certain disk.
 
 
 Input: DiskID: the disk number to be read
        Sector: the sector number to be read
        WriteBuffer: the location to store the data be read
 Output: NULL
************************************************************************/

void physical_disk_read(long DiskID, long Sector, char ReadBuffer[PGSIZE])
{
    PCB* temp;
    MEMORY_MAPPED_IO mmio;
    INT32 lock_result;
    PCB* temp_disk;
    
    
    //get current pcb
    if(IsM==TRUE)
        temp=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        temp = (PCB *) QRemoveHead(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, TRUE, &lock_result);
    }
    
    //set the related value in pcb
    temp->P_Status=waiting_for_disk;
    temp->P_Disk=DiskID;
    temp->P_Disk_Wait=wait_for_read;
    temp->P_Buffer=ReadBuffer;
    temp->P_Sector=Sector;
    
    //insert the current pcb in the corresponding diskqueue
    READ_MODIFY(DISK_LOCK_BASE+(INT32)DiskID,1,TRUE,&lock_result);
    QInsertOnTail(diskqueue_name[DiskID],temp);
    temp_disk=(PCB*)QNextItemInfo(diskqueue_name[DiskID]);
    READ_MODIFY(DISK_LOCK_BASE+(INT32)DiskID,0,TRUE,&lock_result);
    
    //if the corresponding diskqueue is not being used, start the disk
    if(temp==temp_disk)
    {
        mmio.Mode = Z502DiskRead;
        mmio.Field1 = DiskID;
        mmio.Field2 = Sector;
        mmio.Field3 = (long) ReadBuffer;
        MEM_WRITE(Z502Disk, &mmio);
        if(mmio.Field4==ERR_BAD_PARAM)
        {
            aprintf("Using wrong disk or sector id and do nothing\n");
            return;
        }
    }
    
    //suspend current process in multi-processor mode
    if(IsM==TRUE)
    {
        suspend_context();
        return;
    }
    
    
    scheduler_printer(temp->P_Id,temp->P_Id,"Disk");
    
    //do dispatcher in single-processor mode
    
    CALL(dispatcher());
}

/************************************************************************
 physical_disk_write
 This function is used to write a certain sector in a certain disk.
 
 
 Input: DiskID: the disk number to be written
        Sector: the sector number to be written
        WriteBuffer: the data to write
 Output: NULL
************************************************************************/

void physical_disk_write(long DiskID, long Sector, char WriteBuffer[PGSIZE])
{
    PCB* temp;
    MEMORY_MAPPED_IO mmio;
    INT32 lock_result;
    PCB* temp_disk;
    
    //get currernt pcb
    if(IsM==TRUE)
        temp=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        temp = (PCB *) QRemoveHead(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, TRUE, &lock_result);
    }
    
    //set some related value in pcb
    temp->P_Status=waiting_for_disk;
    temp->P_Disk=DiskID;
    temp->P_Disk_Wait=wait_for_write;
    temp->P_Buffer=WriteBuffer;
    temp->P_Sector=Sector;
    
    //insert the pcb in corresponding disk queue
    READ_MODIFY(DISK_LOCK_BASE+(INT32)DiskID,1,TRUE,&lock_result);
    QInsertOnTail(diskqueue_name[DiskID],temp);
    temp_disk=QNextItemInfo(diskqueue_name[DiskID]);
    READ_MODIFY(DISK_LOCK_BASE+(INT32)DiskID,0,TRUE,&lock_result);
    
    //when target disk not in use, do the disk write
    if(temp==temp_disk)
    {
        
        mmio.Mode = Z502DiskWrite;
        mmio.Field1 = DiskID;
        mmio.Field2 = Sector;
        mmio.Field3 = (long) WriteBuffer;
        MEM_WRITE(Z502Disk, &mmio);
        if(mmio.Field4==ERR_BAD_PARAM)
        {
            aprintf("Using wrong disk or sector id and do nothing\n");
            return;
        }
    
    }
   
    //suspend current process in multi-process mode
    if(IsM==TRUE)
    {
        suspend_context();
        return;
    }
    
    
    scheduler_printer(temp->P_Id,temp->P_Id,"Disk");
    
    //dispatcher in single-processor mode
    CALL(dispatcher());
}

/************************************************************************
 dispatcher_M
 dispatcher in multi-processor mode, this is executed by a single thread
 in a loop way until all process terminate, this is also the only work of
 the single thread, here we make this thread the initial thread.
 
 Input: NULL
 Output: NULL
************************************************************************/
void dispatcher_M(void)
{
    PCB* temp;
    INT32 lock_result;
    
    //keep doing dispatcher until whole simulation halt
    while (TRUE)
    {
        while(TRUE)
        {
            READ_MODIFY(READY_LOCK, 1, TRUE, &lock_result);
            temp = (PCB *) QRemoveHead(readyqueue);
            READ_MODIFY(READY_LOCK, 0, TRUE, &lock_result);
            
            //when readyqueue not empty, get them all out and start them
            if (temp != (PCB *) -1)
            {
                temp->P_Status = running;
                start_context(temp);
            }
            else
                break;
        }
        CALL();
    }
}



/************************************************************************
 dispatcher
 dispatcher in single-processor mode
 
 Input: NULL
 Output: NULL
************************************************************************/

void dispatcher(void)
{
    PCB* temp;
    MEMORY_MAPPED_IO mmio;
    BOOL IsEmpty;
    INT32 lock_result;
    PCB* temp_ready;
    
    //check whether diskqueue readyqueue　timerqueue are empty
    READ_MODIFY(RUNNING_LOCK,1,TRUE,&lock_result);
    READ_MODIFY(TIMER_LOCK,1,TRUE,&lock_result);
    READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE,1,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+1,1,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+2,1,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+3,1,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+4,1,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+5,1,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+6,1,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+7,1,TRUE,&lock_result);
    IsEmpty=QNextItemInfo(diskqueue0)==(void*)-1&&QNextItemInfo(diskqueue1)==(void*)-1&&QNextItemInfo(diskqueue2)==(void*)-1&&
        QNextItemInfo(diskqueue3)==(void*)-1&& QNextItemInfo(diskqueue4)==(void*)-1&&QNextItemInfo(diskqueue5)==(void*)-1&&
        QNextItemInfo(diskqueue6)==(void*)-1&&QNextItemInfo(diskqueue7)==(void*)-1 && QNextItemInfo(timerqueue)==(void*)-1&&
        QNextItemInfo(readyqueue)==(void*)-1;
    READ_MODIFY(RUNNING_LOCK,0,TRUE,&lock_result);
    READ_MODIFY(TIMER_LOCK,0,TRUE,&lock_result);
    READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE,0,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+1,0,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+2,0,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+3,0,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+4,0,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+5,0,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+6,0,TRUE,&lock_result);
    READ_MODIFY(DISK_LOCK_BASE+7,0,TRUE,&lock_result);
    
    
    //if diskqueue timerqueue readyqueue all empty, halt the simulation
    if(IsEmpty)
    {
        mmio.Mode=Z502Action;
        mmio.Field1=mmio.Field2=mmio.Field3=0;
        MEM_WRITE(Z502Halt, &mmio);
        return;
    }
    
    //waste time until ready queue is not empty
    while (TRUE)
    {
        READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
        temp_ready=(PCB*)QNextItemInfo(readyqueue);
        READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
        if(temp_ready!=(PCB*)-1)
            break;
        CALL();
    }
    
    
    //take out the readyqueue head
    READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
    temp=(PCB*)QNextItemInfo(readyqueue);
    READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
    scheduler_printer(0,temp->P_Id,"dispatcher");
    READ_MODIFY(READY_LOCK,1,TRUE,&lock_result);
    QRemoveItem(readyqueue,temp);
    READ_MODIFY(READY_LOCK,0,TRUE,&lock_result);
    READ_MODIFY(RUNNING_LOCK,1,TRUE,&lock_result);
    QInsert(runningqueue,FALSE,temp);
    READ_MODIFY(RUNNING_LOCK,0,TRUE,&lock_result);
    
    //switch context
    switch_context(temp);
}





/************************************************************************
 terminate_process
 This function is used to terminate process according to process id
 
 Input: ProcessID: the process id user wants to end
        ErrorReturned: error control
 Output: NULL
************************************************************************/
void terminate_process(long ProcessID,long * ErrorReturned)
{
    MEMORY_MAPPED_IO mmio;
    PCB* temp;
    BOOL halt=TRUE;
    INT32 lock_result;
    *ErrorReturned=ERR_SUCCESS;
    
    //process id illegal
    if(ProcessID<-2||ProcessID>=MAX_NUMBER_OF_PROCESS)
    {
        *ErrorReturned=ERR_PID_NO_EXIST;
        return;
    }
    
    //process id no exist
    else if(ProcessID!=-2&&ProcessID!=-1&&Process_Table[ProcessID]==NULL)
    {
        *ErrorReturned=ERR_PID_NO_EXIST;
        return;
    }
    
    //the process is current process
    else if(ProcessID==-1)
    {
        //multi-processor mode
        if(IsM==TRUE)
        {
            //get current pcb
            temp=get_current_pcb();
            
            //check the state of other process
            for(int i=1;i<MAX_NUMBER_OF_PROCESS;i++)
            {
                if(!Process_Table[i] || i==(temp->P_Id))
                    continue;
                if(Process_Table[i]->P_Status!=terminated)
                {
                    halt=FALSE;
                    
                    break;
                }
            }
            
            //if all other process is terminated halt the simulation
            if(halt==TRUE)
            {
                mmio.Mode=Z502Action;
                mmio.Field1=mmio.Field2=mmio.Field3=0;
                MEM_WRITE(Z502Halt, &mmio);
            }
            
            //if there is still other process alive, just termninate the current one
            temp->P_Status=terminated;
            READ_MODIFY(FRAME_LOCK,1,TRUE,&lock_result);
            
            //release all the frames occupied by the process
            for(int i=0;i<NUMBER_PHYSICAL_PAGES;i++)
            {
                if(FrameTable[i].pcb==temp)
                {
                    ava_frame_count=0;
                    FrameTable[i].pcb=NULL;
                    FrameTable[i].virtual_id=0;
                }
            }
            READ_MODIFY(FRAME_LOCK,0,TRUE,&lock_result);
            
            //suspend the current process
            suspend_context();
            return;
        }
        
        //single-processor mode
        //terminate the current process
        READ_MODIFY(RUNNING_LOCK,1,TRUE,&lock_result);
        temp=(PCB*)QRemoveHead(runningqueue);
        READ_MODIFY(RUNNING_LOCK,0,TRUE,&lock_result);
        temp->P_Status=terminated;
        READ_MODIFY(FRAME_LOCK,1,TRUE,&lock_result);
    
        //release all the frames occupied by the process
        for(int i=0;i<NUMBER_PHYSICAL_PAGES;i++)
        {
            if(FrameTable[i].pcb==temp)
            {
                ava_frame_count=0;
                FrameTable[i].pcb=NULL;
                FrameTable[i].virtual_id=0;
            }
        }
        READ_MODIFY(FRAME_LOCK,0,TRUE,&lock_result);
        
        scheduler_printer(temp->P_Id,temp->P_Id,"Terminate");
    
        //call dispatcher
        CALL(dispatcher());
    }
    
    //hold the simulation
    else if(ProcessID==-2)
    {
        //somulation halt
        mmio.Mode=Z502Action;
        mmio.Field1=mmio.Field2=mmio.Field3=0;
        MEM_WRITE(Z502Halt, &mmio);
        return;
    }
    else
    {/*
        //find out the destinated process and terminate
        temp_ready=ReadyQueue.head;
        temp=Process_Table[ProcessID];
        if(temp_ready->P_Next)
        {
            while(temp_ready->P_Next)
            {
                if (temp_ready->P_Next == temp)
                    break;
                temp = temp->P_Next;
            }
            temp_ready->P_Next=temp_ready->P_Next->P_Next;
        }
        else
            ReadyQueue.head=NULL;
        temp->P_Status=terminated;
        temp->P_Next=NULL;
        scheduler_printer(CurrentProcess->P_Id,temp->P_Id,"Terminate");
        return;
        */
    }
}


/************************************************************************
 get_process_id
 This function is used to get process id, the process could be current process
 or any other existing process
 
 Input: ProcessName: the name of the targeted process,"" denotes current process
        ProcessID: the address to receive the result
        ErrorReturned: error control
 Output: NULL
************************************************************************/

void get_process_id(char* ProcessName, long* ProcessID,long* ErrorReturned)
{
    INT32 lock_result;
    
    //get current process id
    if (strcmp(ProcessName,"")==0)
    {
        //multi-processor mode
        if(IsM==TRUE)
        {
            *ProcessID=get_current_pcb()->P_Id;
            *ErrorReturned = ERR_SUCCESS;
            return;
        }
        
        //single-processor mode
        READ_MODIFY(RUNNING_LOCK,1,TRUE,&lock_result);
        *ProcessID = ((PCB*)QNextItemInfo(runningqueue))->P_Id;
        READ_MODIFY(RUNNING_LOCK,0,TRUE,&lock_result);
        *ErrorReturned = ERR_SUCCESS;
        return;
    }
    else
        //traverse the process table to get the targeted process's id
        for (int i = 1; i < MAX_NUMBER_OF_PROCESS; i++)
            if (Process_Table[i])
                if (strcmp(ProcessName, Process_Table[i]->P_Name) == 0&&Process_Table[i]->P_Status!=terminated)
                {
                    *ProcessID = i;
                    *ErrorReturned = ERR_SUCCESS;
                    return;
                }
    *ErrorReturned=ERR_PNAME_NO_EXIST;
}


/************************************************************************
 initial_block0
 This function is used to initial block0 when doing format
 
 Input: NULL
 Output: NULL
************************************************************************/
void initial_block0()
{
    long disk_num;
    INT32 lock_result;
    
    
    //get diskid from current pcb
    if(IsM==TRUE)
        disk_num=get_current_pcb()->P_Disk;
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        disk_num = ((PCB *) QNextItemInfo(runningqueue))->P_Disk;
        READ_MODIFY(RUNNING_LOCK, 0, TRUE, &lock_result);
    }
    Block block0;
    
    //initialize bytes of block0
    block0.bytes[0] = (char)disk_num;
    block0.bytes[1] = BIT_MAP_SIZE;
    block0.bytes[2] = ROOT_DIR_SIZE;
    block0.bytes[3] = (char)SWAP_SIZE;
    block0.bytes[4] = (NUMBER_LOGICAL_SECTORS & MASK_LSF);
    block0.bytes[5] = (NUMBER_LOGICAL_SECTORS & MASK_MSF)>>8;
    block0.bytes[6] = (BIT_MAP_LOC & MASK_LSF);
    block0.bytes[7] = (BIT_MAP_LOC & MASK_MSF)>>8;
    block0.bytes[8] = (ROOT_DIR_LOC & MASK_LSF);
    block0.bytes[9] = (ROOT_DIR_LOC & MASK_MSF)>>8;
    block0.bytes[10] = (SWAP_LOC & MASK_LSF);
    block0.bytes[11] = (SWAP_LOC & MASK_MSF)>>8;
    for (int i = 12; i < 16; i++)
    {
        block0.bytes[i] = '\0';
    }
    
    //flush block0 into disk
    physical_disk_write(disk_num,0,block0.bytes);
}

/************************************************************************
 format
 This function is used to format a new disk. what it needs to do is
 initial block0, initial bit map, initial root directory
 
 Input: DiskID: the disk number to be formated
        ErrorReturned: error control
 Output: NULL
************************************************************************/


void format(long DiskID,long*ErrorReturned)
{
    INT32 lock_result;
    
    //diskid illegal
    if(DiskID<0||DiskID>=MAX_NUMBER_OF_DISKS)
    {
        *ErrorReturned=ERR_DISKID_ILLEGAL;
        return;
    }
    
    //tell the system that a disk could be used to store swap area
    hasformated=DiskID;
    
    //get current pcb and associate the disk number with it
    if(IsM==TRUE)
        get_current_pcb()->P_Disk=DiskID;
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        ((PCB *) QNextItemInfo(runningqueue))->P_Disk = DiskID;
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //initial block0
    initial_block0();
    
    //initial bit map
    initial_bit_map();
    
    //initial root directoory
    initial_header("root",root_type);
    
    //flush bit map into disk
    put_bit_map_in_disk();
}


/************************************************************************
 initial_bit_map
 This function is used to initial bit map, the main job is to set the location
 of bit map and root directory in bit map
 
 Input: NULL
 Output: NULL
************************************************************************/
void initial_bit_map(void)
{
    update_bit_map(0);
    
    for (int i = BIT_MAP_LOC; i <BIT_MAP_LOC+ BIT_MAP_SIZE * 4; i++)
        update_bit_map(i);
    for (int i =ROOT_DIR_LOC; i < ROOT_DIR_SIZE + ROOT_DIR_LOC; i++)
        update_bit_map(i);
 }

/************************************************************************
 update_bit_map
 This function is used to update bit map in memory. It could set a location
 in bit map from 1 to 0, also 0 to 1
 
 Input: sector_num: the number of the sector to be changed
 Output: NULL
************************************************************************/
void update_bit_map(long sector_num)
{
    PCB* current_pcb;
    INT32 lock_result;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    
    if(current_pcb->P_Disk<0)
        current_pcb->P_Disk=hasformated;
    
    //get start location of bit map
    char* temp=bit_map[current_pcb->P_Disk].data[0].bytes;
    
    //update the location associated with the sector number
    *(temp+sector_num/8)^=(1<<(7-(sector_num%8)));
   
    
    //record which section of the bit map changed
    //prepare to update it in real disk
    current_pcb->bit_map_change_sector[sector_num/128]=TRUE;
}


/************************************************************************
 put_bit_map_in_disk
 This function is used to flush the bit map into disk.
 
 Input: NULL
 Output: NULL
************************************************************************/
void put_bit_map_in_disk(void)
{
    PCB* current_pcb;
    INT32 lock_result;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //just update the part in bit map that have changed
    //avoid flush all 16 sectors of the bit map into disk
    //there is a structure in pcb to record which part of
    //the bit map has been changed
    for (int i = 0; i < BIT_MAP_SIZE * 4; i++)
    {
        if(current_pcb->bit_map_change_sector[i]==0)
            continue;
        physical_disk_write(current_pcb->P_Disk,BIT_MAP_LOC+i,bit_map[current_pcb->P_Disk].data[i].bytes);
        current_pcb->bit_map_change_sector[i]=FALSE;
    }
}


/************************************************************************
 initial_header
 This function is used to initial header for file or directory, it is used
 when creating new file or directory
 
 Input: name:the name of the file or directory
        type: whether it is root/directory/file
 Output: the inode number of the file or directory
************************************************************************/

long initial_header(char* name,int type)
{
    long inode_num;
    long index_loc;
    long header_loc;
    PCB* current_pcb;
    INT32 lock_result;
    MEMORY_MAPPED_IO mmio;
    directory* new_dir=(directory*)calloc(1,sizeof(directory));
    Block header;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //initial the name in the header
    int len=strlen(name);
    for (int i = 1; i <= 7; i++)
    {
        if (i <= len)
            header.bytes[i] = *(name + i-1);
        else
            header.bytes[i] = '\0';
    }
    
    //get a free location for index block
    //if we are initializing root directory
    //the location is certain
    
    if(type==root_type)
        index_loc=ROOT_DIR_LOC+1;
    else
    {
        READ_MODIFY(BITMAP_LOCK,1,TRUE,&lock_result);
        index_loc = get_next_free_sec();
        update_bit_map(index_loc);
        READ_MODIFY(BITMAP_LOCK,0,TRUE,&lock_result);
    }
    
    //initialize the index sector in the disk
    initial_index(index_loc);
   
   //get current time
    mmio.Mode=Z502ReturnValue;
    mmio.Field1=mmio.Field2=mmio.Field3=0;
    MEM_READ(Z502Clock,&mmio);
    
    //initialize the bytes between 8-15 for the header
    header.bytes[8]=(type==file_type?0:1);
    header.bytes[9] =(char)(mmio.Field1&MASK_LSF);
    header.bytes[10] =(char)((mmio.Field1&MASK_MSF)>>8);
    header.bytes[11] =(char)((mmio.Field1& MASK_SSF)>>16);
    header.bytes[12] =(char)(index_loc & MASK_LSF);
    header.bytes[13] =(char)((index_loc & MASK_MSF)>>8);
    header.bytes[14] = 0;
    header.bytes[15] = 0;
    
    
    if(type==root_type)
    {
        //for root header location is certain
        header_loc = ROOT_DIR_LOC;
        new_dir->head_loc=header_loc;
        new_dir->index_loc=index_loc;
        
        //inode number for root is 0
        inode_table[current_pcb->P_Disk][0]=new_dir;
        header.bytes[0]=0;
    }
    
    else
    {
        
        //get a new empty block for the header
        READ_MODIFY(BITMAP_LOCK,1,TRUE,&lock_result);
        header_loc = get_next_free_sec();
        update_bit_map(header_loc);
        READ_MODIFY(BITMAP_LOCK,0,TRUE,&lock_result);
        
        //get a new empty inode number
        for (int i = 1; i < 31; i++)
            if (inode_table[current_pcb->P_Disk][i] == 0)
            {
                new_dir->index_loc=index_loc;
                new_dir->head_loc=header_loc;
                inode_table[current_pcb->P_Disk][i]=new_dir;
                header.bytes[0]=(char)i;
                inode_num=i;
                break;
            }
    }
    
    //put the header in disk
    physical_disk_write(current_pcb->P_Disk,header_loc,header.bytes);
    
    //flush the bit map
    put_bit_map_in_disk();
    
    if(type==root_type)
        return 0;
    
    //if it is not root directory, update the current directory
    update_index(inode_table[current_pcb->P_Disk][current_pcb->cur_dir]->index_loc,-1,header_loc);
    return inode_num;
}



/************************************************************************
 initial_index
 This function is used to initialize index for file or directory
 
 Input: index_loc:the sector number of the index
 Output: NULL
************************************************************************/

void initial_index(long index_loc)
{
    INT32 lock_result;
    PCB* current_pcb;
    char bytes[16];
    
    //set all bytes 0
    for(int i=0;i<16;i++)
        bytes[i]='\0';
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
  
    //write the index into disk
    physical_disk_write(current_pcb->P_Disk,index_loc,bytes);
}

/************************************************************************
 get_next_free_swap
 This function is used to get a free location in bit_map, if this function
 is used, it means the swap area is now in the disk, the disk number is set
 by format()
 
 Input: NULL
 Output: sector number which is allocated as empty swap area location
 ************************************************************************/
int get_next_free_swap(void)
{
    char* temp;

    //get the start address of the bit_map, the current disk has been set by format
    temp=bit_map[hasformated].data[0].bytes;
    
    //search swap_area in bit map to get free location
    for(int i=SWAP_LOC;i<NORMAL_SEC_LOC;i++)
    {
        if((*(temp+i/8)&(128>>(i%BLOCK_BIT%BYTE_BIT)))==0)
            return i;
    }
    
    //fail too get a free location
    return -1;
}

/************************************************************************
 get_next_free_sec
 This function is used to get a free location in disk, this is called usually
 when a new header block, index block, or file content block need location
 
 Input: NULL
 Output: sector number which is allocated
 ************************************************************************/
int get_next_free_sec(void)
{
    INT32 lock_result;
    char* temp;
    
    //get current pcb
    if(IsM==TRUE)
        temp=bit_map[get_current_pcb()->P_Disk].data[0].bytes;
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        temp=bit_map[((PCB*)QNextItemInfo(runningqueue))->P_Disk].data[0].bytes;
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //search in bit map to get a free location
    for(int i=NORMAL_SEC_LOC;i<NUMBER_LOGICAL_SECTORS;i++)
    {
        if((*(temp+i/8)&(128>>(i%BLOCK_BIT%BYTE_BIT)))==0)
            return i;
    }
    
    //disk full
    return -1;
}

/************************************************************************
 open_dir
 This function is used to open a directory in current directory, if it doesn't
 exist,create it and then open it
 
 Input: DiskID_OR_Minus1: The targeted disk in which to open the directory
        DirectoryName: the name of the directory to be opened
        ErrorReturned: error control
 Output: NULL
 ************************************************************************/

void open_dir(long DiskID_OR_Minus1,char* DirectoryName,long* ErrorReturned)
{
    long inode_num;
    long temperror;
    PCB* current_pcb;
    long loc_in_cur_dir;
    INT32 lock_result;

    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    *ErrorReturned=ERR_SUCCESS;
    
    
    if(DiskID_OR_Minus1!=-1)
    {
        //disk id illegal
        if(DiskID_OR_Minus1<0||DiskID_OR_Minus1>7)
        {
            *ErrorReturned=ERR_DISKID_ILLEGAL;
            return;
        }
        //disk no foramt
        if(!inode_table[DiskID_OR_Minus1][0])
        {
            *ErrorReturned=ERR_DISK_NO_FORMAT;
            return;
        }
        //directory name error
        if(strcmp(DirectoryName,"root"))
        {
            *ErrorReturned=ERR_NAME_WRONG;
            return;
        }
        
        
        current_pcb->P_Disk=DiskID_OR_Minus1;
        current_pcb->cur_dir=0;
        return;
    }
    
    //directory name error
    if(strlen(DirectoryName)==0 || strlen(DirectoryName)>7)
    {
        *ErrorReturned=ERR_NAME_WRONG;
        return;
    }
    /*if(strcmp(DirectoryName, "..") == 0)
    {
        if(CurrentProcess->cur_dir->type==root_type)
        {
            *ErrorReturned=ERR_ALREADY_ROOT;
            return;
        }
        CurrentProcess->cur_dir=CurrentProcess->cur_dir->parent;
        return;
    }*/
    
    //check whether exist in current directory, if exist get the inode number of the directory
    loc_in_cur_dir=check_in_index(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][current_pcb->cur_dir]->index_loc,DirectoryName,-1);
    
    //not exist in current directory and current directory full
    if(loc_in_cur_dir==8)
    {
        *ErrorReturned=ERR_CUR_DIR_FULL;
        return;
    }
    
    //exist in current directory
    if(loc_in_cur_dir>9)
    {
        current_pcb->cur_dir=loc_in_cur_dir-9;
        return;
    }
    
    //not exist in current directory, but we have room to create the new directory
    inode_num=create_dir_or_file(DirectoryName,&temperror,dir_type);
    
    //fail to create
    if(temperror!=1)
    {
        *ErrorReturned=temperror;
        return;
    }
    
    else
        current_pcb->cur_dir=inode_num;
}

/************************************************************************
 open_file
 This function is used to open a file in current directory, if it doesn't
 exist,create it and then open it
 
 Input: FileName: The name of the file to be opened
        Inode: the inode number of the file to be opened
        ErrorReturned: error control
 Output: NULL
 ************************************************************************/

void open_file(char* FileName,long *Inode,long* ErrorReturned)
{
    
    long temperror;
    long inode_num;
    INT32 lock_result;
    *ErrorReturned=ERR_SUCCESS;
    PCB* current_pcb;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //no current directory
    if(current_pcb->cur_dir==-1)
    {
        *ErrorReturned=ERR_NO_CUR_DIR;
        return;
    }
    
    //file name wrong
    if(strlen(FileName)==0 || strlen(FileName)>7)
    {
        *ErrorReturned=ERR_NAME_WRONG;
        return;
    }
    
    //check whether the file exist in current directory and get the inode number
    inode_num=check_in_index(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][current_pcb->cur_dir]->index_loc,FileName,-1);
    if(inode_num>8)
    {
        current_pcb->cur_file=inode_num-9;
        *Inode=inode_num-9;
        return;
    }
    
    //file not exist in current directory but current directory already full
    else if(inode_num==8)
    {
        *ErrorReturned=ERR_CUR_DIR_FULL;
        return;
    }
    
    //create the file
    inode_num=create_dir_or_file(FileName,&temperror,file_type);
    
    //fail to create
    if(temperror!=ERR_SUCCESS)
    {
        *ErrorReturned = temperror;
        return;
    }
    current_pcb->cur_file=inode_num;
    *Inode=inode_num;
}
/************************************************************************
 check_in_index
 This function is used to check in index block, there are several scenes
 we may use this function
 1. check whether a file or directory exist in current directory, if not exist,
    return next free location in current directory
 2. check whther a logical block exist in a file, if not exist, create
    the logical block
 
 Input: disk_num:
        cur_dir_sector: the sector number of the current dir
        name:
        logical_loc:
 Output:
 ************************************************************************/

long check_in_index(long disk_num,long cur_dir_sector,char* name,int logical_loc)
{
    Block temp_index_block;
    Block temp_header_block;
    
    //read out the targeted sector associated with current directory for later operation
    physical_disk_read(disk_num,cur_dir_sector,temp_index_block.bytes);
    
    
    if(logical_loc>=0)
    {
        
        if(temp_index_block.bytes[2*logical_loc]||temp_index_block.bytes[2*logical_loc+1])
        {
            return (temp_index_block.bytes[2 * logical_loc]
                & MASK_LSF) + (temp_index_block.bytes[2 * logical_loc + 1] << 8);
        }
        else
            return -1;
    }
    for(int i=0;i<8;i++)
        if(temp_index_block.bytes[2*i]||temp_index_block.bytes[2*i+1])
        {
            if(name==NULL)
                continue;
            physical_disk_read(disk_num,(temp_index_block.bytes[2*i]& MASK_LSF)+(temp_index_block.bytes[2*i+1]<<8),temp_header_block.bytes);
            if(!memcmp(name,(temp_header_block.bytes+1),7))
            {
                return (9+temp_header_block.bytes[0]);
            }
        }
        else
        {
            return i;
        }
    return 8;
}


/************************************************************************
 create_dir_or_file
 This function is used to create a directory of file. This function may
 return a inode number, this number is used when next step is to open the
 file
 
 Input: Name: name of directory or file
        ErrorReturned: error control
        type: whether it is a directory or file
 Output: inode number
************************************************************************/

long create_dir_or_file(char* Name, long* ErrorReturned,long type)
{
    long loc_in_cur_dir;
    PCB* current_pcb;
    INT32 lock_result;
    long inode_num;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    *ErrorReturned=ERR_SUCCESS;
    
    //no current directory
    if (current_pcb->cur_dir == -1)
    {
        *ErrorReturned = ERR_NO_CUR_DIR;
        return -1;
    }
    
    //name illegal
    if(strlen(Name)==0 || strlen(Name)>7)
    {
        *ErrorReturned=ERR_NAME_WRONG;
        return -1;
    }
    
    //check in current directory whether the name already exist
    loc_in_cur_dir = check_in_index(current_pcb->P_Disk, inode_table[current_pcb->P_Disk][current_pcb->cur_dir]->index_loc, Name,-1);
    
    //name already exist
    if (loc_in_cur_dir >8)
    {
        *ErrorReturned = ERR_ALREADY_EXIST;
        return -1;
    }
    
    //name doesn't exist but no room for new directory or file
    else if (loc_in_cur_dir == 8)
    {
        *ErrorReturned = ERR_CUR_DIR_FULL;
        return -1;
    }
    
    //check whether there is room in disk for new directory or file
    if(get_next_free_sec()==-1)
    {
        *ErrorReturned=ERR_DISK_NO_ROOM;
        return -1;
    }
    
    //initial header of the file or directory and return back the inode number
    inode_num=initial_header(Name,(int)type);
    
    return inode_num;
}

/************************************************************************
 update_index
 This function is used to update index block of current directory or file
 
 Input: index_loc:sector number of targeted index block
        update_loc: the location in the index to be updated
        update_content: the content to be updated
 Output: NULL
************************************************************************/

void update_index(long index_loc,long updated_loc,long updated_content)
{
    PCB* current_pcb;
    Block temp_block;
    INT32 lock_result;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //there is no logical block number input, check which location in index to be updated
    if(updated_loc==-1)
    {
        updated_loc=check_in_index(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][current_pcb->cur_dir]->index_loc,NULL,-1);
    }
    
    //update the index block
    physical_disk_read(current_pcb->P_Disk,index_loc,temp_block.bytes);
    temp_block.bytes[2*updated_loc]=(char)(updated_content & MASK_LSF);
    temp_block.bytes[2*updated_loc+1]=(char)((updated_content & MASK_MSF)>>8);
    physical_disk_write(current_pcb->P_Disk,index_loc,temp_block.bytes);
}


/************************************************************************
 close_file
 This function is used to close current file
 
 Input: Inode: Inode number of the targeted file
        ErrorReturned: Error control
 Output: NULL
 ************************************************************************/
void close_file(long Inode, long* ErrorReturned)
{
    *ErrorReturned=ERR_SUCCESS;
    PCB* current_pcb;
    INT32 lock_result;
    
    //get current process
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //there is no current open file
    if(Inode!=current_pcb->cur_file)
    {
        *ErrorReturned=ERR_INODE_WRONG;
        return;
    }
    
    //close the current file and flush the bit map in disk
    current_pcb->cur_file=-1;
    put_bit_map_in_disk();
}

/************************************************************************
 read_file
 This function is used to read content from a logical location of an opened
 file.
 
 Input: Inode: Inode number of the targeted file
        FilelogicalBlock: The logical number in the file to read
        ReadBuffer: receive the data that has been read out
        ErrorReturned: error control
 Output: Null
 ************************************************************************/
 
void read_file(long Inode, long FileLogicalBlock, char* ReadBuffer, long* ErrorReturned)
{
    long sector_loc;
    *ErrorReturned=ERR_SUCCESS;
    PCB* current_pcb;
    INT32 lock_result;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //Logical number illegal
    if(FileLogicalBlock<0 || FileLogicalBlock>=8)
    {
        *ErrorReturned=ERR_FILE_LOGIC_NUM_ILLEGAL;
        return;
    }
    
    //Inode number wrong or it is not opened file
    if(Inode!=current_pcb->cur_file)
    {
        *ErrorReturned=ERR_INODE_WRONG;
        return;
    }
    
    //check whether there is content in logical block of current file
    sector_loc=check_in_index(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][Inode]->index_loc,(char*)None,(int)FileLogicalBlock);
    
    //there is no content in the targeted logicalfile number
    if(sector_loc==-1)
    {
        *ErrorReturned=ERR_NO_CONTENT;
        return;
    }
    
    //read back the content block from disk
    physical_disk_read(current_pcb->P_Disk,sector_loc,ReadBuffer);
}

/************************************************************************
 write_file
 This function is used to write content in logical location in now opened
 file, if the logical location now has no associated block, assign new block
 to it.
 
 Input: Inode: inode number of the targeted file
        FilelogicalBlock: The logical number in the file
        WriteBuffer: The block to be written in
        ErrorReturned: error control
 Output: Null
 ************************************************************************/

void write_file(long Inode, long FileLogicalBlock, char* WriteBuffer, long* ErrorReturned)
{
    long sector_loc;
    Block current_file_header;
    *ErrorReturned=ERR_SUCCESS;
    PCB* current_pcb;
    INT32 lock_result;
    long file_size;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //Filelogical number illegal
    if(FileLogicalBlock<0 || FileLogicalBlock>=8)
    {
        *ErrorReturned=ERR_FILE_LOGIC_NUM_ILLEGAL;
        return;
    }
    
    //the targeted file inode number is wrong or the file is not opened
    if(Inode!=current_pcb->cur_file)
    {
        *ErrorReturned=ERR_INODE_WRONG;
        return;
    }
    
    //check whether current file has the targeted logical block
    sector_loc=check_in_index(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][Inode]->index_loc,(char*)NULL,(int)FileLogicalBlock);
    
    //if the targeted logical block doesn't exist, assign block for it
    if(sector_loc==-1)
    {
        //target logical block not exist, assign new block
        READ_MODIFY(BITMAP_LOCK,1,TRUE,&lock_result);
        sector_loc=get_next_free_sec();
        update_bit_map(sector_loc);
        READ_MODIFY(BITMAP_LOCK,0,TRUE,&lock_result);
        
        //update index block of the current file
        update_index(inode_table[current_pcb->P_Disk][current_pcb->cur_file]->index_loc,FileLogicalBlock,sector_loc);
        
        //update header block of the current file, update the file size
        physical_disk_read(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][current_pcb->cur_file]->head_loc,current_file_header.bytes);
        file_size=(current_file_header.bytes[14]&MASK_LSF)+(current_file_header.bytes[15]<<8)+16;
        current_file_header.bytes[14]=(char)(file_size & MASK_LSF);
        current_file_header.bytes[15]=(char)((file_size & MASK_MSF)>>8);
        physical_disk_write(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][current_pcb->cur_file]->head_loc,current_file_header.bytes);
    }
    
    //write the targeted content block in disk
    physical_disk_write(current_pcb->P_Disk,sector_loc,WriteBuffer);
}

/************************************************************************
 dir_contents
 This function is used to print content information of current directory.
 It print attributes of files/directories existing in current directory.
 
 Input: ErrorReturned  collect error information
 Output: NULL
 ************************************************************************/
void dir_contents(long* ErrorReturned)
{
    Block temp_header;
    Block temp_index;
    Block temp_header_dir_or_file;
    char name[7];
    char type;
    PCB* current_pcb;
    INT32 lock_result;
    int time;
    long Inode;
    long filesize=0;
    *ErrorReturned=ERR_SUCCESS;
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    //there is no current directory
    if(current_pcb->cur_dir==-1)
    {
        *ErrorReturned=ERR_NO_CUR_DIR;
        return;
    }
    
    //get current dirctory's header and index block
    physical_disk_read(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][current_pcb->cur_dir]->head_loc,temp_header.bytes);
    physical_disk_read(current_pcb->P_Disk,inode_table[current_pcb->P_Disk][current_pcb->cur_dir]->index_loc,temp_index.bytes);
    
    
    memcpy(name,temp_header.bytes+1,7);
    
    
    aprintf("\nContents of Directory:    %s\n",name);
    aprintf("Inode,   FileName,    D/F,    Creation Time,    FILE SIZE\n");
    for(int i=0;i<8;i++)
    {
        
        if(!temp_index.bytes[2*i]&&!temp_index.bytes[2*i+1])
            continue;
        Inode=i;
        physical_disk_read(current_pcb->P_Disk,(temp_index.bytes[2*i]& MASK_LSF)+(temp_index.bytes[2*i+1]<<8),temp_header_dir_or_file.bytes);
        memcpy(name,temp_header_dir_or_file.bytes+1,7);
        time=(temp_header_dir_or_file.bytes[9] & MASK_LSF)+(temp_header_dir_or_file.bytes[11]<<16)+(temp_header_dir_or_file.bytes[10]<<8);
        if(temp_header_dir_or_file.bytes[8]&1)
            type='D';
        else
        {
            type = 'F';
            filesize=(temp_header_dir_or_file.bytes[14]&MASK_LSF)+(temp_header_dir_or_file.bytes[15]<<8);
        }
        if (type == 'D')
        {
            aprintf("%d         %s      %c         %d              %c\n", Inode, name, type, time, '-');
        }
        else
        {
            aprintf("%d         %s      %c         %d              %d\n", Inode, name, type, time, filesize);
        }
        filesize=0;
    }
}
/************************************************************************
 frame_fault_handler
 When this function being called, it denotes a targeted page number is not
 associated with a physical page. We need to find a physical memory for it.
 If physical memory us already full, we need to find a unlucky physical page
 to swap out
 
 Input: fault_virtual_frame_id  the targeted virtual page number
 ************************************************************************/

void frame_fault_handler(long fault_virtual_frame_id)
{
    long ava_frame=-1;
    char data[PGSIZE];
    PCB* current_pcb;
    int temp;
    INT32 lock_result;
    
    //physical memory still not full
    if(ava_frame_count<NUMBER_PHYSICAL_PAGES)
    {
        READ_MODIFY(FRAME_LOCK,1,TRUE,&lock_result);
        while (ava_frame_count < NUMBER_PHYSICAL_PAGES)
        {
            if (FrameTable[ava_frame_count].pcb == NULL && FrameTable[ava_frame_count].virtual_id != -1)
            {
                ava_frame = ava_frame_count++;
                break;
            }
            ava_frame_count++;
        }
        READ_MODIFY(FRAME_LOCK,0,TRUE,&lock_result);
        //physical memory full, do page replacement
        if(ava_frame==-1)
            ava_frame = page_replace();
    }
    else
    {
        //physical memory full, do page replacement
        ava_frame = page_replace();
    }
    
    //get current pcb
    if(IsM==TRUE)
        current_pcb=get_current_pcb();
    else
    {
        READ_MODIFY(RUNNING_LOCK, 1, TRUE, &lock_result);
        current_pcb = (PCB *) QNextItemInfo(runningqueue);
        READ_MODIFY(RUNNING_LOCK, 0, FALSE, &lock_result);
    }
    
    temp=current_pcb->P_PageTable[fault_virtual_frame_id] & PTBL_PHYS_PG_NO;
   
    //targeted virtual page exists in swap area, firstly swap the page in physical memory
    if(temp)
    {
        if(hasformated>=0)
        {
            physical_disk_read(hasformated,temp,data);
            READ_MODIFY(BITMAP_LOCK,1,TRUE,&lock_result);
            update_bit_map(temp);
            READ_MODIFY(BITMAP_LOCK,0,TRUE,&lock_result);
        }
        else
        {
            for (int i = 0; i < 16; i++)
            {
                data[i] = swaparea[temp].data[i];
                swaparea[temp].data[i] = '\0';
            }
            swaparea[temp].status = 0;
        }
        Z502WritePhysicalMemory((INT32) ava_frame, data);
    }
    
    //already find physicla memory, associate it with page table of current pcb and update related information in physical memory
    current_pcb->P_PageTable[fault_virtual_frame_id]=(short)(ava_frame | PTBL_VALID_BIT | PTBL_REFERENCED_BIT );
    READ_MODIFY(FRAME_LOCK,1,TRUE,&lock_result);
    FrameTable[ava_frame].pcb = current_pcb;
    FrameTable[ava_frame].virtual_id = fault_virtual_frame_id;
    READ_MODIFY(FRAME_LOCK,0,TRUE,&lock_result);
    
    //if swap area in disk,flush the bit map in disk
    if(hasformated>=0)
        put_bit_map_in_disk();
}


/************************************************************************
 page_replace
 This function is used to find an unlucky page in physical memory and swap
 it out to empty the physical address for new page.
 
 This function has no input, it use LRU algorithm to find the unlucky page
 The output is the physcial page number that is empty now.
 ************************************************************************/
int page_replace(void)
{
    
    long victim=-1;
    long tempvirtualid;
    long tempswaploc=-1;
    char data[PGSIZE];
    INT32 lock_result;
    PCB* temp_pcb;
    
    //using LRU  algorithm to find an unlucky page to be replaced
    READ_MODIFY(FRAME_LOCK,1,TRUE,&lock_result);
    while(1)
    {
        long i=my_clock%NUMBER_PHYSICAL_PAGES;
        my_clock++;
        tempvirtualid=FrameTable[i].virtual_id;
        if(FrameTable[i].pcb==NULL)
            continue;
        if((FrameTable[i].pcb->P_PageTable[tempvirtualid] & PTBL_REFERENCED_BIT)==0)
        {
            victim=i;
            break;
        }
        (FrameTable[i].pcb->P_PageTable[tempvirtualid])-=PTBL_REFERENCED_BIT;
    }
    
    //update related value in frametable
    temp_pcb=FrameTable[victim].pcb;
    FrameTable[victim].pcb=NULL;
    FrameTable[victim].virtual_id=-1;
    READ_MODIFY(FRAME_LOCK,0,TRUE,&lock_result);
    
    //read the unlucky page from physical memory
    Z502ReadPhysicalMemory(victim,data);
    
    //swap out the unlucky page
    
    //if swap area is in a disk
    if(hasformated>=0)
    {
        READ_MODIFY(BITMAP_LOCK,1,TRUE,&lock_result);
        tempswaploc=get_next_free_swap();
        update_bit_map(tempswaploc);
        READ_MODIFY(BITMAP_LOCK,0,TRUE,&lock_result);
        physical_disk_write(hasformated,tempswaploc,data);
    }
    
    //swap area not in disk(designed for test 43 test 44)
    else
    {
        for (int i = 0; i < SWAP_NUM; i++)
            if (swaparea[i].status == 0)
                tempswaploc = i;
        memcpy(swaparea[tempswaploc].data,data,PGSIZE);
        swaparea[tempswaploc].status=1;
    }
    
    //reset some related information
    temp_pcb->P_PageTable[tempvirtualid]=(short)(tempswaploc);
    
    return (int)victim;
}


/************************************************************************
 SVC
 The beginning of the OS502.  Used to receive software interrupts.
 All system calls come to this point in the code and are to be
 handled by the student written code here.
 The variable do_print is designed to print out the data for the
 incoming calls, but does so only for the first ten calls.  This
 allows the user to see what's happening, but doesn't overwhelm
 with the amount of data.
 ************************************************************************/

void svc_handler_add(short call_type,long const *const Err)
{
    if(*Err==ERR_SUCCESS)
        aprintf("\n%s succeed\n",call_names[call_type]);
    else
    {
        aprintf("\n%s has error: %s\n",call_names[call_type],err_names[*Err]);
    }
}

void svc(SYSTEM_CALL_DATA *SystemCallData)
{
	short call_type;
	static short do_print = 10;
	short i;
	MEMORY_MAPPED_IO mnio;
    
    
    call_type = (short) SystemCallData->SystemCallNumber;
	if (do_print > 0&&handler!=None)
	{
		aprintf("\nSVC handler: %s\n", call_names[call_type]);
		for (i = 0; i < SystemCallData->NumberOfArguments - 1; i++)
		{
			//Value = (long)*SystemCallData->Argument[i];
			aprintf("Arg %d: Contents = (Decimal) %8ld,  (Hex) %8lX\n", i,
					(unsigned long) SystemCallData->Argument[i],
					(unsigned long) SystemCallData->Argument[i]);
		}
		if(handler==Initial)
		    do_print--;
	}
	switch(call_type)
	{
	    // Get time service call
		case SYSNUM_GET_TIME_OF_DAY:
        {
            mnio.Mode = Z502ReturnValue;
            mnio.Field1 = mnio.Field2 = mnio.Field3 = 0;
            MEM_READ(Z502Clock, &mnio);
            *(long *) SystemCallData->Argument[0] = mnio.Field1;
            break;
        }
        // Terminate system call
		case SYSNUM_TERMINATE_PROCESS:
        {
            terminate_process((long)SystemCallData->Argument[0],SystemCallData->Argument[1]);
            if(do_print>0&&handler!=None)
                svc_handler_add(SYSNUM_TERMINATE_PROCESS,SystemCallData->Argument[1]);
            break;
        }
		// Get current process id
		case SYSNUM_GET_PROCESS_ID:
        {
            get_process_id((char *) (SystemCallData->Argument[0]),
                           SystemCallData->Argument[1],
                           SystemCallData->Argument[2]);
            if (do_print > 0 && handler != None)
                svc_handler_add(SYSNUM_GET_PROCESS_ID, SystemCallData->Argument[2]);
            break;
        }
        //
		case SYSNUM_SLEEP:
        {
            Sleep((long)SystemCallData->Argument[0]);
            break;
        }
		// Create a process   (long*)Argument:0-ProcessName 1-StartingAddress 2-InitialPriority 3-&ProcessID 4-&ErrorReturned);
		case SYSNUM_CREATE_PROCESS:
        {
            create_process((char*)SystemCallData->Argument[0],SystemCallData->Argument[1],(long)SystemCallData->Argument[2],SystemCallData->Argument[3],SystemCallData->Argument[4]);
            if(do_print>0&&handler!=None)
                svc_handler_add(SYSNUM_CREATE_PROCESS,SystemCallData->Argument[4]);
            break;
        }
	    // Suspend a process
	    case SYSNUM_SUSPEND_PROCESS:
        {
            suspend_process((long)SystemCallData->Argument[0],SystemCallData->Argument[1]);
            if(do_print>0&&handler!=None)
                svc_handler_add(SYSNUM_SUSPEND_PROCESS,SystemCallData->Argument[1]);
            break;
        }
	    case SYSNUM_RESUME_PROCESS:
        {
            resume_process((long)SystemCallData->Argument[0],SystemCallData->Argument[1]);
            if(do_print>0&&handler!=None)
                svc_handler_add(SYSNUM_RESUME_PROCESS,SystemCallData->Argument[1]);
            break;
        }
        case SYSNUM_PHYSICAL_DISK_READ:
        {
            physical_disk_read((long)SystemCallData->Argument[0],(long)SystemCallData->Argument[1],(char*)SystemCallData->Argument[2]);
            break;
        }
        case SYSNUM_PHYSICAL_DISK_WRITE:
        {
            physical_disk_write((long)SystemCallData->Argument[0],(long)SystemCallData->Argument[1],(char*)SystemCallData->Argument[2]);
            break;
        }
	    case SYSNUM_CHANGE_PRIORITY:
        {
            change_priority((long)SystemCallData->Argument[0],(long)SystemCallData->Argument[1],SystemCallData->Argument[2]);
            if(do_print>0&&handler!=None)
                svc_handler_add(SYSNUM_CHANGE_PRIORITY,SystemCallData->Argument[2]);
            break;
        }
	    case SYSNUM_CHECK_DISK:
        {
            check_disk((long) SystemCallData->Argument[0], SystemCallData->Argument[1]);
            if (do_print > 0 && handler != None)
                svc_handler_add(SYSNUM_CHECK_DISK, SystemCallData->Argument[1]);
            break;
        }
	    case SYSNUM_FORMAT:
        {
            format((long) SystemCallData->Argument[0], SystemCallData->Argument[1]);
            break;
        }
	    case SYSNUM_CREATE_DIR:
        {
            create_dir_or_file((char*) SystemCallData->Argument[0], SystemCallData->Argument[1],dir_type);
            break;
        }
	    case SYSNUM_CREATE_FILE:
        {
            create_dir_or_file((char*) SystemCallData->Argument[0], SystemCallData->Argument[1],file_type);
            break;
        }
	    case SYSNUM_OPEN_DIR:
        {
            open_dir((long) SystemCallData->Argument[0],(char*)SystemCallData->Argument[1],SystemCallData->Argument[2]);
            break;
        }
	     case SYSNUM_OPEN_FILE:
        {
            open_file((char*)SystemCallData->Argument[0],SystemCallData->Argument[1],SystemCallData->Argument[2]);
            break;
        }
        case SYSNUM_CLOSE_FILE:
        {
            close_file((long)SystemCallData->Argument[0],SystemCallData->Argument[1]);
            break;
        }
	    case SYSNUM_READ_FILE:
        {
            read_file((long)SystemCallData->Argument[0],(long)SystemCallData->Argument[1],(char*)SystemCallData->Argument[2],SystemCallData->Argument[3]);
            break;
        }
	    case SYSNUM_WRITE_FILE:
        {
            write_file((long)SystemCallData->Argument[0],(long)SystemCallData->Argument[1],(char*)SystemCallData->Argument[2],SystemCallData->Argument[3]);
            break;
        }
	    case SYSNUM_DIR_CONTENTS:
        {
            dir_contents(SystemCallData->Argument[0]);
            break;
        }
       default:
			aprintf("ERROR! call_type not recognized!\n");
			aprintf("Call_type is - %i\n", call_type);
	}
}                                               // End of svc

/************************************************************************
 osInit
 This is the first routine called after the simulation begins.  This
 is equivalent to boot code.  All the initial OS components can be
 defined and initialized here.
 ************************************************************************/

void osInit(int argc, char *argv[])
{
    long Err;
    long pid;
    
   
    
    void *PageTable = calloc(2, NUMBER_VIRTUAL_PAGES);
	INT32 i;
	MEMORY_MAPPED_IO mmio;
    
	
	sleep(1);
	
	//Initialize the queues
    readyqueue=QCreate("readyqueue");
    timerqueue=QCreate("timerqueue");
    suspendqueue=QCreate("suspendqueue");
    runningqueue=QCreate("runningqueue");
    diskqueue_name[0]=diskqueue0=QCreate("diskqueue0");
    diskqueue_name[1]=diskqueue1=QCreate("diskqueue1");
    diskqueue_name[2]=diskqueue2=QCreate("diskqueue2");
    diskqueue_name[3]=diskqueue3=QCreate("diskqueue3");
    diskqueue_name[4]=diskqueue4=QCreate("diskqueue4");
    diskqueue_name[5]=diskqueue5=QCreate("diskqueue5");
    diskqueue_name[6]=diskqueue6=QCreate("diskqueue6");
    diskqueue_name[7]=diskqueue7=QCreate("diskqueue7");
    
    
    
    
    
    
    
	// Demonstrates how calling arguments are passed thru to here
	aprintf("Program called with %d arguments:", argc);
	for (i = 0; i < argc; i++)
	    aprintf(" %s", argv[i]);
	aprintf("\n");
	aprintf("Calling with argument 'sample' executes the sample program.\n");
	// Here we check if a second argument is present on the command line.
	// If so, run in multiprocessor mode.  Note - sometimes people change
	// around where the "M" should go.  Allow for both possibilities
	if (argc > 2) {
		if ((strcmp(argv[1], "M") ==0) || (strcmp(argv[1], "m")==0)) {
			strcpy(argv[1], argv[2]);
			strcpy(argv[2],"M\0");
		}
		if ((strcmp(argv[2], "M") ==0) || (strcmp(argv[2], "m")==0)) {
			aprintf("Simulation is running as a MultProcessor\n\n");
			IsM=TRUE;
			mmio.Mode = Z502SetProcessorNumber;
			mmio.Field1 = MAX_NUMBER_OF_PROCESSORS;
			mmio.Field2 = (long) 0;
			mmio.Field3 = (long) 0;
			mmio.Field4 = (long) 0;
			MEM_WRITE(Z502Processor, &mmio);   // Set the number of processors
		}
	}
	else
	{
	    aprintf("Simulation is running as a UniProcessor\n");
		aprintf("Add an 'M' to the command line to invoke multiprocessor operation.\n\n");
	}
	//          Setup so handlers will come to code in base.c
	TO_VECTOR[TO_VECTOR_INT_HANDLER_ADDR ] = (void *) InterruptHandler;
	TO_VECTOR[TO_VECTOR_FAULT_HANDLER_ADDR ] = (void *) FaultHandler;
	TO_VECTOR[TO_VECTOR_TRAP_HANDLER_ADDR ] = (void *) svc;

	//  Determine if the switch was set, and if so go to demo routine.
 
	PageTable = (void *) calloc(2, NUMBER_VIRTUAL_PAGES);
	if ((argc > 1) && (strcmp(argv[1], "sample") == 0))
	{
		mmio.Mode = Z502InitializeContext;
		mmio.Field1 = 0;
		mmio.Field2 = (long) SampleCode;
		mmio.Field3 = (long) PageTable;

		MEM_WRITE(Z502Context, &mmio);   // Start of Make Context Sequence
		mmio.Mode = Z502StartContext;
		// Field1 contains the value of the context returned in the last call
		mmio.Field2 = START_NEW_CONTEXT_AND_SUSPEND;
		MEM_WRITE(Z502Context, &mmio);     // Start up the context
  
	} // End of handler for sample code - This routine should never return here
/*#ifdef          MyTestingMode
	MyTestingosInit(argc, argv);
#endif*/
    

    
    
    if(( argc > 1 ) && ( strcmp( argv[1], "test1" ) == 0 ) )
    {
        sp_mode=None;
        handler=Full;
        mp_mode=None;
        create_process(argv[1],(void*)test1,10, &pid, &Err);
    }
    
    else if(( argc > 1 ) && ( strcmp( argv[1], "test2" ) == 0 ) )
    {
        sp_mode=None;
        handler=Full;
        mp_mode=None;
        create_process(argv[1], (void*)test2,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test3" ) == 0 ) )
    {
        sp_mode=Full;
        handler=Full;
        mp_mode=None;
        create_process(argv[1], (void*)test3,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test4" ) == 0 ) )
    {
        sp_mode=Full;
        handler=Initial;
        mp_mode=None;
        create_process(argv[1], (void*)test4,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test5" ) == 0 ) )
    {
        sp_mode=Limited;
        handler=Initial;
        mp_mode=None;
        create_process(argv[1], (void*)test5,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test6" ) == 0 ) )
    {
        sp_mode=Limited;
        handler=Initial;
        mp_mode=None;
        create_process(argv[1], (void*)test6,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test7" ) == 0 ) )
    {
        sp_mode=Initial;
        handler=Limited;
        mp_mode=None;
        create_process(argv[1], (void*)test7,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test8" ) == 0 ) )
    {
        sp_mode=Full;
        handler=Full;
        mp_mode=None;
        create_process(argv[1], (void*)test8,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test9" ) == 0 ) )
    {
        sp_mode=Full;
        handler=Full;
        mp_mode=None;
        create_process(argv[1], (void*)test9,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test10" ) == 0 ) )
    {
        handler=Initial;
        sp_mode=Full;
        mp_mode=None;
        if(argc>2)
        {
            handler=Initial;
            sp_mode=Limited;
            mp_mode=None;
        }
        create_process(argv[1], (void*)test10,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test11" ) == 0 ) )
    {
        handler=Initial;
        sp_mode=Limited;
        mp_mode=None;
        if(argc>2)
        {
            handler=Initial;
            sp_mode=Limited;
            mp_mode=None;
        }
        create_process(argv[1], (void*)test11,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test12" ) == 0 ) )
    {
        sp_mode=None;
        handler=Initial;
        mp_mode=None;
        create_process(argv[1], (void*)test12,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test21" ) == 0 ) )
    {
        handler=Full;
        sp_mode=None;
        mp_mode=None;
        create_process(argv[1], (void*)test21,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test22" ) == 0 ) )
    {
        handler=Full;
        sp_mode=None;
        mp_mode=None;
        create_process(argv[1], (void*)test22,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test23" ) == 0 ) )
    {
        handler=Initial;
        sp_mode=Full;
        mp_mode=None;
        create_process(argv[1], (void*)test23,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test24" ) == 0 ) )
    {
        handler=Initial;
        sp_mode=Limited;
        mp_mode=None;
        create_process(argv[1], (void*)test24,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test25" ) == 0 ) )
    {
        handler=Initial;
        sp_mode=Limited;
        mp_mode=None;
        if(argc>2)
        {
            handler=Initial;
            sp_mode=Limited;
            mp_mode=None;
        }
        create_process(argv[1], (void*)test25,10 , &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test41" ) == 0 ) )
    {
        handler=Full;
        sp_mode=None;
        mp_mode=Full;
        create_process(argv[1], (void*)test41,10 , &pid, &Err);
    }

    else if(( argc > 1 ) && ( strcmp( argv[1], "test42" ) == 0 ) )
    {
        handler = Full;
        sp_mode = None;
        mp_mode=Full;
        create_process(argv[1], (void *) test42, 10, &pid, &Err);
    }

    else if(( argc > 1 ) && ( strcmp( argv[1], "test43" ) == 0 ) )
    {
        handler = Initial;
        sp_mode = None;
        mp_mode=Full;
        create_process(argv[1], (void *) test43, 10, &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test44" ) == 0 ) )
    {
        handler = Initial;
        sp_mode = None;
        mp_mode=Limited;
        create_process(argv[1], (void *) test44, 10, &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test45" ) == 0 ) )
    {
        handler = Initial;
        sp_mode = None;
        mp_mode=Limited;
        if(argc>2)
        {
            handler=Initial;
            sp_mode=None;
            mp_mode=Limited;
        }
        create_process(argv[1], (void *) test45, 10, &pid, &Err);
    }
    else if(( argc > 1 ) && ( strcmp( argv[1], "test46" ) == 0 ) )
    {
        handler = Initial;
        sp_mode = None;
        mp_mode=Limited;
        create_process(argv[1], (void *) test46, 10, &pid, &Err);
    }
    
    
    
    //The initialize thread won't be suspend it will be used only for dispatcher
    if(IsM==TRUE)
        dispatcher_M();
    
    
    
    
    
    //  By default test0 runs if no arguments are given on the command line
	//  Creation and Switching of contexts should be done in a separate routine.
	//  This should be done by a "OsMakeProcess" routine, so that
	//  test0 runs on a process recognized by the operating system.
    create_process("test0", (void *) test0, 10, &pid, &Err);
}                                               // End of osInit
