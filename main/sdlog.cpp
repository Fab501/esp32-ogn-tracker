#include <stdint.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

#include "hal.h"
#include "gps.h"
#include "sdlog.h"
#include "timesync.h"
#include "fifo.h"

static char  LogFileName[32];
static FILE *LogFile = 0;

static   uint16_t  LogDate = 0;                                   // [~days] date = FatTime>>16
static TickType_t  LogOpenTime;                                   // [msec] when was the log file (re)open
static const  TickType_t  LogReopen = 30000;                      // [msec] when to close and re-open the log file

const size_t FIFOsize = 16384;
static FIFO<char, FIFOsize> Log_FIFO;                             // 16K buffer for SD-log
       SemaphoreHandle_t Log_Mutex;                               // Mutex for the FIFO to prevent mixing between threads

void Log_Write(char Byte)                                         // write a byte into the log file buffer (FIFO)
{ if(Log_FIFO.Write(Byte)>0) return;                              // if byte written into FIFO return
  while(Log_FIFO.Write(Byte)<=0) vTaskDelay(1); }                 // wait while the FIFO is full - we have to use vTaskDelay not TaskYIELD

int Log_Free(void) { return Log_FIFO.Free(); }                    // how much space left in the buffer

static int Log_Open(void)
{ LogDate=GPS_FatTime>>16;                                        // get the FAT-time date part
  int32_t Day   =  LogDate    &0x1F;                              // get day, month, year
  int32_t Month = (LogDate>>5)&0x0F;
  int32_t Year  = (LogDate>>9)-20;
  uint32_t Date = 0;
  if(Year>=0) Date = Day*10000 + Month*100 + Year;                // create DDMMYY number for easy printout
  strcpy(LogFileName, "/sdcard/CONS/TR000000.LOG");
  Format_UnsDec(LogFileName+15, Date, 6);                         // format the date into the log file name
  LogFile = fopen(LogFileName, "at");                             // try to open the file
  if(LogFile==0)                                                  // if this fails
  { if(mkdir("/sdcard/CONS", 0777)<0) return -1;                  // try to create the sub-directory
    LogFile = fopen(LogFileName, "at"); if(LogFile==0) return -1; } // and again attempt to open the log file
  LogOpenTime=xTaskGetTickCount();
  return 0; }

                                                                  // TaskYIELD would not give time to lower priority task like log-writer
static void Log_Check(void)                                       // time check:
{ if(!LogFile) return;                                            // if last operation in error then don't do anything
  TickType_t TimeSinceOpen = xTaskGetTickCount()-LogOpenTime;     // when did we (re)open the log file last time
  if(LogDate)
  { if(TimeSinceOpen<LogReopen) return; }                         // if fresh (less than 30 seconds) then nothing to do
  else
  { if(TimeSinceOpen<(LogReopen/4)) return; }
  fclose(LogFile); LogFile=0;                                     // close and reopen the log file when older than 10 seconds
  uint32_t Time = TimeSync_Time();
  struct stat LogStat;
  struct utimbuf LogTime;
  if(stat(LogFileName, &LogStat)>=0)                              // get file attributes (maybe not really needed)
  { LogTime.actime  = Time;                                       // set access and modification times of the dest. file
    LogTime.modtime = Time;
    utime(LogFileName, &LogTime); }
}

static int WriteLog(size_t MaxBlock=FIFOsize/2)                    // process the queue of lines to be written to the log
{ if(!LogFile) return 0;
  int Count=0;
  for( ; ; )
  { char *Block; size_t Len=Log_FIFO.getReadBlock(Block); if(Len==0) break;
    if(Len>MaxBlock) Len/=2;
    int Write=fwrite(Block, 1, Len, LogFile);
    Log_FIFO.flushReadBlock(Len);
    if(Write!=Len) { fclose(LogFile); LogFile=0; return -1; }
    Count+=Write; }
  return Count; }

#ifdef WITH_SDLOG

extern "C"
 void vTaskSDLOG(void* pvParameters)
{ Log_FIFO.Clear();

  for( ; ; )
  { if(!SD_isMounted())                                              // if SD ia not mounted:
    { vTaskDelay(5000); SD_Mount(); continue; }                      // try to (Re)mount it after a delay of 5sec
    if(!LogFile)                                                     // when SD mounted and log file not open:
    { Log_Open();                                                    // try to (re)open it
      if(!LogFile) { SD_Unmount(); vTaskDelay(1000); continue; }     // if can't be open then unmount the SD and retry at a delay of 1sec
    }
    if(Log_FIFO.Full()<FIFOsize/4) { vTaskDelay(100); }              // if little data to copy, then wait 0.1sec for more data
    int Write;
    do { Write=WriteLog(); } while(Write>0);                         // write the console output to the log file
    if(Write<0) { SD_Unmount(); vTaskDelay(1000); continue; }        // if write fails then unmount the SD card and (re)try after a delay of 1sec
    // if(Write==0) vTaskDelay(100);
    Log_Check(); }                                                   // make sure the log is well saved by regular close-reopen
}

#endif // WITH_SDLOG
