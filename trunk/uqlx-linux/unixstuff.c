/*
 * (c) UQLX - see COPYRIGHT
 */

/************************************************************************************************/
/* Modification history                                                                         */
/* 0712 TF Adapted to linux ARM                                                                 */
/*      Removed all references to EMX                                                           */
/*      Signal handling aligned between Linux arm and Linux x86                                 */
/*      Major code cleanup                                                                      */
/*      NOTE: Not tested on Linux/68k                                                           */
/************************************************************************************************/ 

#include "QL68000.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <time.h>

#include <fcntl.h> 
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "xcodes.h"  
#include "QL_config.h" 
#include "QInstAddr.h"

#include "vm.h"

#include "unix.h"
#include "boot.h"
#include "qx_proto.h"
#include "QL_sound.h"

#define TIME_DIFF    283996800
void GetDateTime(w32 *);
long long qlClock = 0;

#ifdef VTIME 
int qlttc = 50;
int qltime = 0;
int qitc = 10;
#endif

int ux_boot = 0;
int ux_bfd = 0;
char *ux_bname = "";
int start_iconic = 0;
char *pwindow = NULL;
int is_patching = 0;
int UQLX_optind;
int UQLX_argc;
char **UQLX_argv;

extern uw32 rtop_hard;
extern int screen_drawable;

int do_update = 0;  /* initial delay for screen drawing */

#define min(_a_,_b_) (_a_<_b_ ? _a_ : _b_)
#define max(_a_,_b_) (_a_>_b_ ? _a_ : _b_)

int rx1, rx2, ry1, ry2, finishflag, doscreenflush;
int QLdone = 0;

extern void FlushDisplay (void);

extern void DbgInfo (void);

#ifdef AUTO_BOOT
void btrap3 (void);
#endif

extern void SchedulerCmd (void);
extern void KbdCmd (void);

#ifndef XAW
extern void process_events (void);
#endif

int script = 0;
int redir_std = 0;

int scrcnt = 0;
volatile int doPoll = 0;

#ifdef VTIME
volatile poll_req = 0;   /* debug only */
#endif

int noints = 0;
int schedCount = 0;
extern int min_idle;
int HasDialog;

/* define useful values for pagesize. Those come from sysconfig if USE_VM is defined */
#if defined(VM_SCR) || defined(USE_VM)
#else
long pagesize/*=RM_PAGESIZE*/;
int pageshift/*=RM_SHIFT*/;
#endif

void cleanup_dialog (){}

int InitDialog ()
{
   if (pwindow == NULL)
   {
      char buf [PATH_MAX], arg [100];
      char *p;
      int QMpid, bfpid;
      
#ifdef XAW
      return 0;
#else
      QMpid = getpid ();
      sprintf (arg, "%d", QMpid);

      strncpy (buf, IMPL,PATH_MAX); 
      p = buf + strlen(buf) - 1;
      if (*p != '/') 
         strncat (buf,"/", PATH_MAX);
#ifndef USE_IPC
      strncat (buf, "ButtonFrame", PATH_MAX);
#else
      strncat(buf, "Xgui", PATH_MAX);
#endif

      if (access(buf, R_OK | X_OK)) 
         return;

      bfpid = qm_fork (cleanup_dialog, 0);
      if (bfpid) 
         return bfpid;

      execl (buf, buf, arg, NULL);
      perror ("Sorry, could not exec() GUI");
      /*kill(QMpid,SIGUSR1);*/
      _exit(0);
#endif
   }
}

int rtc_emu_on = 0;
void prep_rtc_emu ()
{
#ifdef USE_VM
   rtc_emu_on = 1;
   mprotect ((Ptr)theROM + 0x18000, pagesize, PROT_NONE);
#endif
}

void set_rtc_emu ()
{
#ifdef USE_VM
   uw32 t;
  
   rtc_emu_on = 0;
   GetDateTime (&t);
   mprotect((Ptr)theROM + 0x18000, pagesize, PROT_READ | PROT_WRITE);
   WL((Ptr)theROM + 0x18000, t);
   *(unsigned char *)((Ptr)theROM + 0x18063) = (display_mode == 4 ? 0 : 8);
   mprotect((Ptr)theROM + 0x18000, pagesize, is_patching ? PROT_NONE : PROT_READ);
#endif
}

/* Read a system variable word sized from QDOS memory    */
/* NOTE(TF): Apparently not used anywhere in the code!   */
uw16 sysvar_w (uw32 a)
{
   return RW((Ptr)theROM + 0x28000 + a);
}

/* Read a system variable (long) from QDOS memory        */
uw32 sysvar_l (uw32 a)
{
  return RL((Ptr)theROM+0x28000+a);
}

/* restore mprotect() for area according to RamMap       */
void uqlx_prestore (unsigned long start, unsigned long len)
{
#if defined(USE_VM) || defined(VM_SCR) 
   int i, tmp, prot;

   for (i = start >> pageshift; (start + len) > (i << pageshift); i++) 
   {
      switch (RamMap [i])
      {
         case QX_NONE: 
            prot = PROT_READ; 
            break;
         case QX_ROM:  
            prot = PROT_READ; 
            break;
         /*case QX_QXM:  
            prot = PROT_READ; 
            break;*/
         case QX_RAM:  
            prot = PROT_READ | PROT_WRITE; 
            break;
         case QX_SCR:  
            if (!scrModTable[i - (qlscreen.qm_lo >> pageshift)] && do_update)
               prot = PROT_READ; 
            else 
               prot = PROT_READ | PROT_WRITE;
            break;
         case QX_IO:   
            prot = is_patching ? PROT_NONE : PROT_READ; 
            break;
         default: 
            printf("illegal value in uqlx_prestore\n");
      }
#ifndef EVM_SCR
      if (MPROTECT((Ptr)theROM+start,len,prot))
      {
          perror("could not change VM permission");
          printf("  at ql-addr %x, len %x, protection %x \n",(unsigned)start,(unsigned)len,prot);
      }
#endif
   }
#endif
}

/* set both RamMap and mprotect() */
void uqlx_protect (unsigned long  start, unsigned long len, int type)
{
   int i, tmp;

   tmp = type;

   for (i = start>>pageshift; (start+len) > (i<<pageshift); i++) 
   {
      RamMap[i]=type;
   }
#if defined(USE_VM) || defined(VM_SCR)
   {
      int prot = 0;
      switch (tmp) 
      {
         case QX_NONE: 
            prot = PROT_READ; 
            break;
         case QX_ROM:  
            prot = PROT_READ; 
            break;
         /*case QX_QXM:  
            prot = PROT_READ; 
            break;*/
         case QX_RAM:  
            prot = PROT_READ | PROT_WRITE; 
            break;
         case QX_SCR:  
            prot = PROT_READ; 
            break;
         case QX_IO:   
            prot = is_patching ? PROT_NONE : PROT_READ; 
            break;
         default: 
            printf("illegal value in uqlx_protect\n");
      }
      if (MPROTECT((Ptr)theROM + start, len, prot))
         perror("could not change VM permission");
   }
#endif
}

#ifdef SHOWINTS
static long alrm_count = 0;
static long a_ticks = 0;
static long aa_cnt = 0;
#endif

static int flptest = 0;

void dosignal ()
{
   uw32 t;
  
   doPoll = 0;
  
#ifdef SOUND
   if (delay_list)
      qm_sound ();
#endif

#ifndef XAW
   if (!script && !QLdone)
      process_events ();
#endif

#ifdef SHOWINTS
   aa_cnt += alrm_count;
   alrm_count = 0;
   if (++a_ticks > 49)
   {
      printf("received %d alarm signals, processed %d\n",aa_cnt,a_ticks);
      a_ticks = aa_cnt = 0;
   }
#endif
    
   if(--scrcnt < 0)
   {
      doscreenflush = 1;
      set_rtc_emu();
   }

   if (flptest++ > 25)
   {
      flptest = 0;
      TestCloseDevs ();
      process_ipc ();
   }
  
#ifndef xx_VTIME
   FrameInt (); 
#endif
}

extern int xbreak;
void cleanup (int err)
{
#ifndef XAW
   if (HasDialog > 0)
      kill (HasDialog, SIGINT);

   cleanup_ipc ();
   if (!script && !xbreak)
      x_screen_close ();
#endif
   CleanRAMDev ("RAM");
   exit (err);
}

void oncc (int sig)
{
   printf ("exiting\n");
   QLdone = 1;
}

void signalTimer ()
{
   doPoll = 1;
#ifdef VTIME
   poll_req++;
#endif
   schedCount = 0;
#ifdef SHOWINTS
   alrm_count++;
#endif
}

void ontsignal (int sig)
{
   /*set_rtc_emu();*/   /* .. not yet working */
   signalTimer ();
}

/* rather crude but reliable */
static int fat_int_called = 0;

void on_fat_int (int x)
{
   if (fat_int_called==1) 
      exit(45);
   if (fat_int_called > 1) 
      raise(9);
   fat_int_called++;

   alarm (0);
   printf ("Terminate on signal %d\n", x);
   printf ("This may be due to an internal error,\n"
           "a feature not emulated or an 68000 exception\n"
           "that typically occurs only when QDOS is unrecoverably\n"
           "out of control\n");
   dbginfo("FATAL error, PC may not be displayed correctly\n");
   cleanup(44);
}

void init_timers ()
{
   struct itimerval val;

#ifndef VTIME   
   val.it_value.tv_sec = 0L;
   val.it_value.tv_usec = 20000;
  
   val.it_interval = val.it_value;

   setitimer(ITIMER_REAL, &val, NULL);
#endif
}

void InitDialogErr (int x)
{
   HasDialog = -1;
   /* init_timers (); */
}

#ifdef UX_WAIT
#include <sys/wait.h>
struct cleanup_entry
{
   void (*cleanup) ();
   unsigned long int id;
   pid_t pid;
   struct cleanup_entry *next;
};

static struct cleanup_entry * cleanup_list = NULL;
static int run_reaper;

static void sigchld_handler(int sig)
{
   run_reaper = 1;
}

static  int qm_wait(fc)
int *fc;
{
   int pid;
     
   pid = wait3(fc, WNOHANG, (struct rusage *)NULL);
   return pid;
}

/* exactly like fork but registers cleanup handler */
int qm_fork(void (*cleanup)(), unsigned long id)
{
   struct cleanup_entry *ce;
   int pid;

   pid = fork ();
   if (pid > 0)
   {
      ce = (void*)malloc(sizeof(struct cleanup_entry));
      ce->pid = pid;
      ce->id = id;
      ce->cleanup = cleanup;
      ce->next = cleanup_list;
      cleanup_list = ce;
   }
   return pid;
}

static void qm_reaper ()
{
   struct cleanup_entry *ce,**last;
   int pid, found;
   int failcode;

   run_reaper = 0;
   while((pid = qm_wait(&failcode)) > 0)
   {
      ce = cleanup_list;
      last = &cleanup_list;
      found = 0;
      while(ce)
      {
         if (pid == ce->pid)
         {
            *last = ce->next;
            (*(ce->cleanup))(ce->pid,ce->id,failcode);
            free(ce);
            found = 1;
            break;
         }
         last = &(ce->next);
         ce = ce->next;
      }
      if (!found) 
         printf("hm, pid %d not found in cleanup list?\n",pid);
   }
}
#endif

void init_signals ()
{
   struct sigaction sac;
   long i;

   sigemptyset(&(sac.sa_mask));
   sac.sa_flags = 0;
#if !defined(SUNOS) && !defined(NO_NODEFER)
   sac.sa_flags=SA_NODEFER;
#endif

   /* Works for plain signal handlers where we don't need more info */
   for(i = 1; i <= SIGUSR2; i++)
   {
      sac.sa_handler = oncc;
      sigaction(i, &sac, NULL);
   }
   sac.sa_handler = SIG_IGN;
   sigaction(SIGPIPE, &sac, NULL);

   sac.sa_handler = ontsignal;
   sigaction(SIGALRM, &sac, NULL);

#ifndef NSIG
#define NSIG _NSIG
#endif

   sac.sa_handler = on_fat_int;
   sigaction(SIGSEGV, &sac, NULL);
   sigaction(SIGBUS, &sac, NULL);
   sigaction(SIGILL, &sac, NULL);
   sigaction(SIGQUIT, &sac, NULL);

   sac.sa_handler = InitDialogErr;
   if (sigaction(SIGUSR1, &sac, NULL))
      perror("could not redefine SIGUSR1\n");

#ifndef DARWIN
#if defined(VM_SCR) || defined(USE_VM)
#if defined(SPARC)||defined(__arm__)||defined(__i486__)
   sac.sa_sigaction =  (void*)segv_handler;  /* SUN seem to be no longer the only to do this..*/
#else
   sac.sa_handler = segv_handler;  
#endif

   /* This is now done symmetrically on all Linux platforms except 68k */
#if defined(SPARC)||defined(__arm__)||defined(__i486__)
   sac.sa_flags = SA_SIGINFO;
#endif
   sigaction(SIGSEGV, &sac, NULL);
#ifdef SPARC
   sac.sa_sigaction = buserr_handler;
#else
   sac.sa_handler = (void*)buserr_handler;
#endif
   sigaction(SIGBUS, &sac, NULL);
#endif
#ifdef UX_WAIT
   sac.sa_handler = sigchld_handler;
   sigaction(SIGCHLD, &sac, NULL);
#endif
#else // DARWIN
  //mach_exn_init();
#endif

#ifdef UX_WAIT
   sac.sa_handler = sigchld_handler;
   sigaction(SIGCHLD, &sac, NULL);
#endif
}

int load_rom(char *,w32);

void ChangedMemory (int from, int to)
{
   int i;
   uw32 dto,dfrom;

   /* QL screen memory involved? */
   if ((from >= qlscreen.qm_lo && from <= qlscreen.qm_hi) || 
       (to >= qlscreen.qm_lo && to <= qlscreen.qm_hi))
   {
      dfrom = max(qlscreen.qm_lo, from);
      dto = min(qlscreen.qm_hi, to);

      for (i = 0; i < sct_size; i++) 
         scrModTable[i] = (i * pagesize + qlscreen.qm_lo <= dto &&
                           i * pagesize + qlscreen.qm_lo >= dfrom);
   }
}
 
char **argv;

#if defined(USE_VM) || defined(VM_SCR)
void restart_emulator ()
{
   struct itimerval tv;

   tv.it_value.tv_sec = 0;
   tv.it_value.tv_usec = 0;
   tv.it_interval.tv_sec = 0;
   tv.it_interval.tv_usec = 0;
   setitimer(ITIMER_REAL, &tv, NULL);
   /*printf("restarting '%s'\n",argv[0]);*/
   execvp(argv[0], argv);
}
#endif

void DbgInfo(void)
{
   int i;

   /* "ssp" is ssp *before* sv-mode was entered (if active now) */
   /* USP is saved value of a7 or meaningless if not in sv-mode */
   printf("DebugInfo: PC=%x, code=%x, SupervisorMode: %s USP=%x SSp=%x A7=%x\n",
         (Ptr)pc-(Ptr)theROM, code,
         (supervisor ? "yes" : "no" ),
         usp,ssp,*sp);
   printf("Register Dump:\t Dn\t\tAn\n");
   for(i = 0; i < 8; i++)
      printf("%d\t\t%8x\t%8x\n",i,reg[i],aReg[i]);
}

long uqlx_tz;

long ux2qltime (long t)
{
   return t + TIME_DIFF + uqlx_tz;
}

long ql2uxtime (long t)
{
   return t - TIME_DIFF - uqlx_tz;
}

void GetDateTime (w32 *t)
{
   struct timeval tp;

#ifndef VTIME  

   gettimeofday(&tp, (void*)0);
   *t = ux2qltime(tp.tv_sec) + qlClock;;
#else
   *t = qltime;
#endif
}

int rombreak = 0;

int allow_rom_break(int flag)
{
   if (flag < 0) 
      return rombreak;

   if (flag)
   {
      uqlx_protect(0, 3 * 32768, QX_RAM);
      rombreak = 1;
   }
   else
   {
      uqlx_protect(0, 3 * 32768, QX_ROM);
      rombreak = 0; 
   }
   return rombreak;
}

void init_uqlx_tz ()
{
   struct tm ltime;
   struct tm gtime;
   time_t ut;

   ut = time(NULL);
   ltime = *localtime(&ut);
   gtime = *gmtime(&ut);

   gtime.tm_isdst = ltime.tm_isdst;
   uqlx_tz = mktime(&ltime) - mktime(&gtime);
}

w32 ReadQlClock(void)
{
   w32 t;
  
   GetDateTime(&t);
   return t;
}

int impopen(char *name,int flg,int mode)
{
   char buff[PATH_MAX],*p;
   int r,md;

   md = mode;
  
   if ((r = open(name,flg,md))!=-1)
      return r;
  
   if(*name == '~')
   {
      char *p = buff;
      strcpy(p, getenv("HOME"));
      strcat(p, name + 1);
      name = p;
   }
  
   return open(name,flg,md);

   strcpy(buff,IMPL);
   p=buff+strlen(buff);
   if (*(p-1)!='/') strcat(buff,"/");
   strncat(buff,name,PATH_MAX);
  
   return open(buff,flg,md);
}

int load_rom(char *name,w32 addr)
{ 
   struct stat b;
   int r;
   int fd;

  
   fd = impopen(name,O_RDONLY,0); 
   if (fd<0) 
   {
      perror("Warning: could not find ROM image ");
      printf(" - rom name %s\n",name);
      return 0; 
   }

   fstat(fd,&b);
   if (b.st_size!=16384 && addr!=0) 
      printf("Warning: ROM size of 16K expected, %s is %d\n",
             name, (int)b.st_size);
   if (addr & 16383)
      printf("Warning: addr %x for ROM %s not multiple of 16K\n",addr,name);

   r = read(fd, (Ptr)theROM + addr, b.st_size);
   if (r<0) 
   {
      perror("Warning, could not load ROM \n");
      printf("name %s, addr %x, QDOS origin %x\n",name,addr,(unsigned)theROM);
      return 0; 
   }
   if(V3)printf("loaded %s \t\tat %x\n",name,addr);
   close(fd);
  
   return r;
}

int scr_planes = 2;
int scr_width, scr_height;

int verbose = 2;
bctype *RamMap;

#ifndef XAW
extern int shmflag;
#endif

#ifndef XSCREEN
void parse_screen(char *x)
{
   printf("sorry, '-g' option works only with XSCREEN enabled,\ncheck your Makefile\n"); 
}
#endif

int sct_size;
//char *scrModTable, 
char *oldscr ;

static char obuf[BUFSIZ];

void CoreDump ()
{
   int fd,r;
  
   fd = open("qlcore",O_RDWR|O_CREAT,0644);
   if (fd<0) 
      perror("coredump failed: read: :");
   if (fd > -1)
   {
      r = write(fd, theROM, 1024*1024);
      if (!r) 
         perror("coredump failed: write: ");
      close(fd);
      if (r)
         printf("memory dump saved as qlcore\n");
   }
}

char *uqlx_version = "0.50";

#include "uqlx_cfg.h"

char *qm_findx(char *name)
{
   char *loc;
   static char buf[PATH_MAX+40];

   strncpy(buf, getenv("HOME"), PATH_MAX);
   qaddpath(buf, "lib/uqlx", PATH_MAX);
   if (!access(buf, R_OK | X_OK))
      loc = buf;
   else 
      loc=NULL;

   if (!loc && !access(IMPL, R_OK | X_OK))
      loc = IMPL;
   if (!loc && !access("/usr/local/lib/uqlx", R_OK | X_OK))
      loc = "/usr/local/lib/uqlx/";
   if ( !loc && !access("/usr/lib/uqlx",R_OK | X_OK))
      loc="/usr/lib/uqlx/";
   if ( !loc && !access("/usr/local/uqlx/lib/",R_OK | X_OK))
      loc="/usr/local/uqlx/lib/";

   return loc;
}

void browse_manuals ()
{
   int unused;
   char buf[PATH_MAX + 25];
   char *loc;

   loc = qm_findx ("browse_manual");

   strncpy(buf,loc,PATH_MAX);
   qaddpath(buf,"browse_manual ",PATH_MAX);
   strncat(buf,IMPL,PATH_MAX);
   //printf("executing %s\n",buf);
   unused = system(buf);
   exit(0);
}

void vmtest()
{
#if defined(LINUX) && defined(m68k)
   int res;

   char buf[PATH_MAX+1];
   char *loc;

   loc=qm_findx("vmtest");

   strncpy(buf,loc,PATH_MAX);
   qaddpath(buf,"vmtest",PATH_MAX);

   res=system(buf);
   if (res==1)
   {
      printf("vmtest failed, for m68k get latest kernel patch from"
             "rz@linux-m68k.org");
      exit(44);
   }
   if (res)
   {
      printf("problem executing %s\n",buf);
      exit(1);
   }
#endif
}

int toggle_hog(int val)
{
   /*printf("toggle_hog, setting to %d\n",val);*/
   if (val < 0) 
      return QMD.cpu_hog;
   QMD.cpu_hog = val;
   if(QMD.cpu_hog) 
      min_idle = 20000;
   else 
      min_idle = 5;
   return QMD.cpu_hog;
}

void usage(char **argv)
{
   printf("UQLX release %s:\n\tusage: %s [options] arguments\n",release,argv[0] );
   printf("\t options:\n"
          "\t\t -?                  : list options \n"
          "\t\t -??                 : browse manuals, may set $BROWSER \n"
          "\t\t -f config_file      : read alternative config file\n"
          "\t\t -m                  : use greyscale display \n"
          "\t\t -c                  : display colors [if possible] \n"
          "\t\t -i                  : start iconified \n"
          "\t\t -h                  : waste all cpu time available \n"
          "\t\t -r MEM              : set RAMTOP to this many Kb \n"
          "\t\t -o romname          : use this ROM \n"
          "\t\t -b 'boot_cmd'       : define BOOT device with boot_cmd \n"
          "\t\t -s 'boot_cmd'       :    .. and do scripting \n"
          "\t\t -g NxM              : define screen size to NxM \n"
          "\t\t -v num              : set verbosity\n"
          "\t\t -W window           : reparent root window \n"
          "\t\t -n                  : dont patch anything (xuse only)\n\n"
          "\t arguments: available through Superbasic commands\n\n"
         );
   exit(0);
}
void SetParams (int ac, char **av)
{
   char *rf;
   char sysrom[200];
   int  rl = 0;
   int j,c; 
   int mem=-1, col=-1, hog=-1, no_patch=-1;
   int gg=0;
  
   setvbuf(stdout,obuf,_IOLBF,BUFSIZ);

   *sysrom=0;
  
#ifndef NO_GETOPT 
   while((c = getopt(ac,av,"f:micnhr:o:s:b:g:W:p?::v:")) != EOF)
   {
      switch(c)
      {
         case 'g':
            gg = 0;
            parse_screen(optarg);
            break;
         case 'f':
         {
            char temp[PATH_MAX];
            sprintf(temp, "UQLX_CFG=%s", optarg);
            putenv(temp);
         }
         break;
         case 'o':
            strcpy(sysrom,optarg);
            break;
         case 'm':
            col = 0;
            break;
         case 'c':
            col = 1;
            break;
         case 'r':
            mem = atoi(optarg);
            break;
         case 'h':
            hog = 1;
            break;
         case 'n':
            no_patch = 1;
            break;
         case 'i':
            start_iconic=1;
            break;    
         case 'v':
            verbose=atoi(optarg);
            break;
         case 'p':
            is_patching=1;
            verbose=0;
            /* FALLTHRU */
         case 's':
            script=1;
            redir_std=1;
            /* FALLTHRU */
         case 'b':
         {
            int len;
          
            if (optarg && strcmp("",optarg))
            {
               ux_boot=2;
               len=strlen(optarg);
               ux_bname=malloc(len+2);
               strncpy(ux_bname,optarg,len);
               ux_bname[len]=10;
               ux_bname[len+1]=0;
            }
         }
         break;
         case 'W':
            pwindow=optarg;
            break;
            case '?':
            if(optarg)
               browse_manuals();
            else
               usage(av);
            break;
         default: 
            usage(av);
      }
   }

   UQLX_argc = ac;
   UQLX_argv = av;
   UQLX_optind = optind;

#else
   UQLX_argc = ac;
   UQLX_argv = av;
   UQLX_optind = 1;
#endif
   QMParams();
   if (mem > 0 && mem < 17) 
      mem=mem * 1024;

   {
      char *av0 = strrchr(av[0],'/');
      av0 = av0 ? av0 : av[0];

      if (*sysrom) 
         strcpy(QMD.sysrom,sysrom);
      else if( strcmp(av0,"qm") )
      {
         if (strstr(av0,"js")) 
            strcpy(QMD.sysrom,"js_rom");
         if (strstr(av0,"min")) 
            strcpy(QMD.sysrom,"minerva_rom");
      }
    
      if (gg==0 && strstr(av0,"x"))
      {
         if (strcmp(QMD.sysrom,"minerva_rom"))
         {
            strcpy(QMD.sysrom,"minerva_rom");
            if (V2)
               printf("using Minerva ROM\n");
         }
         if (strstr(av0,"xxx"))
            parse_screen(QMD.size_xxx);
         else
            if (strstr(av0,"xx"))
               parse_screen(QMD.size_xx);
            else
               parse_screen(QMD.size_x);
      }
   }
  
   if(mem != -1) QMD.ramtop = mem;
   if(col != -1) QMD.color = col;
   if(hog != -1) QMD.cpu_hog=1;
   if(no_patch != -1) QMD.no_patch=1;

   if (QMD.no_patch) do_update=1;
   //printf("do_update: %d\n",do_update);
  
   toggle_hog(QMD.cpu_hog);
   
   RTOP = QMD.ramtop * 1024; 
}

#ifdef G_reg
w32 _reg[16];
#endif

#define MAX_DISKS 2
typedef struct BlockDriverState BlockDriverState;
static const int ide_iobase[3] = { 0, 0x1f0, 0x170 };
static const int ide_iobase2[3] = { 8,  0x3f6, 0x376 };
static const int ide_irq[2] = { 14, 15 };
BlockDriverState *bs_table[MAX_DISKS];
#ifdef DARWIN
const char *hd_filename[MAX_DISKS]={};
#else
const char *hd_filename[MAX_DISKS]={"/home/rz/.qldir/IMG1",
                                    "/home/rz/.qldir/IMG2"};
#endif

static int cyls=0;
static int heads=0;
static int secs=0;
static int snapshot=0;

#define HAS_CDROM 0

#ifdef ENABLE_IDE

#define BDRV_TYPE_HD     0
#define BDRV_TYPE_CDROM  1
#define BDRV_TYPE_FLOPPY 2

BlockDriverState *bdrv_new(const char *device_name);
void bdrv_set_type_hint(BlockDriverState *bs, int type);
#endif


void init_xhw()
{
#ifdef ENABLE_IDE
   int i=0;

   /* open the virtual block devices */

   if (HAS_CDROM)
   {
      s_table[1] = bdrv_new("cdrom");
      bdrv_set_type_hint(bs_table[1], BDRV_TYPE_CDROM);
   }
   for(i = 0; i < MAX_DISKS; i++) 
   {
      if (hd_filename[i]) 
      {
         if (!bs_table[i]) 
         {
            char buf[64];
            snprintf(buf, sizeof(buf), "hd%c", i + 'a');  
            bs_table[i] = bdrv_new(buf);
         }
         if (bdrv_open(bs_table[i], hd_filename[i], snapshot) < 0) 
         {
            fprintf(stderr, "could not open hard disk image '%s\n",
                    hd_filename[i]);
            ///exit(1);
         }
         if (i == 0 && cyls != 0)
            bdrv_set_geometry_hint(bs_table[i], cyls, heads, secs);
      }
   }

   i = 0;
   ide_init(ide_iobase[i], ide_iobase2[i], ide_irq[i],
            bs_table[2 * i], bs_table[2 * i + 1]);
#endif
}

void uqlxInit ()
{
   char *rf;
   int  rl=0;
   void *tbuff;
   int i, j, c;
   int mem = -1, col = -1, hog = -1;

#if (defined(USE_VM) || defined(VM_SCR) || defined(EVM_SCR))
#ifdef _SC_PAGESIZE
   pagesize = sysconf(_SC_PAGESIZE);
#elif _SC_PAGE_SIZE
   pagesize = sysconf(_SC_PAGE_SIZE);
#else  // yet another way.. was that Darwin??
   pagesize = getpagesize();
#endif
#else // no USE_VM
   pagesize = 4096;
#endif
   if(pagesize > 8192)
   {
      printf("USE_VM can't work with pagesize %x\n",(unsigned)pagesize);
      exit(1);
   }
   for(pageshift = 0,i = pagesize;i;i >>= 1, pageshift++);
      pageshift--;
   
   if ((1<<pageshift) != pagesize)
   {
      printf("your machine seems to have a very odd pagesize of %ld bytes\n",pagesize);
      printf("please recompile with -DNO_PSHIFT added to your buildflags\n");
      exit(1);
   }
   else
      if(V3)
         printf("page size %ld, page shift %d\n",pagesize,pageshift);

   rx1 = 0;
   rx2 = qlscreen.xres-1;
   ry1 = 0;
   ry2 = qlscreen.yres-1;
   finishflag = 0;

   if(V1)
      printf("*** QL Emulator v%s ***\nrelease %s\n\n",uqlx_version,release);
   tzset();

#if defined(VM_SCR) || defined(USE_VM) || defined(ZEROMAP)
   vm_init();
#else
   theROM = malloc(RTOP);
   if (theROM == NULL)
   {
      printf("sorry, not enough memory for a %dK QL\n",RTOP / 1024);
      exit(1);
   }
#endif
   /*memset(theROM,0,(128+64)*1024);*/ /* somewhat safer... */

   RamMap = malloc((((1024*1024*1024)/*& ADDR_MASK*/)>>RM_SHIFT)*sizeof(bctype));

   tbuff=malloc(65536*sizeof(void *));
  
   {
      char roms[PATH_MAX];
      char *p;      

      if(rf = getenv("QL_ROM"))
          rl = load_rom(rf,0);
      if (!rl)
      {
         p = (char*)stpcpy(roms, QMD.romdir);
         if(*(p-1) != '/')
         {
            *p++ = '/';
         }
         strcpy(p, QMD.sysrom);
          
         rl=load_rom(roms ,(w32)0);
         if (!rl) 
         {
            fprintf(stderr,"Could not find qdos ROM image, exiting\n");
            exit(2);
         }
      }
      {
         QMLIST *q;
         for(q=QMD.romlist;q;q=q->next)
         {
            ROMITEM *c = q->udata;
            strcpy(p, c->romname);
            rl = load_rom(roms ,(w32) c->romaddr);
            if (V3)printf("ROM %s %s loaded at 0x%lx\n", 
                          c->romname, rl ? "" : "NOT", c->romaddr);
         }
      }
   }

   init_uqlx_tz();  
  
#ifdef XAW
   if (script)
   {
      fprintf(stderr,"scriptmode mode not supported with XAW\n");
      script=0;
   }
#endif


   init_signals();

#ifndef XAW
   if (!script && 1)
   {
#ifdef USE_IPC
      init_ipc();
#endif
      HasDialog=InitDialog();
   }
  
#endif

   init_iso();

   init_xhw();                           /* init extra HW */
   LoadMainRom();                        /* patch QDOS ROM*/

   /* Minerva cannot handle more than 16M of memory... */
   if (isMinerva && RTOP > 16384 * 1024) 
      RTOP = 16384 * 1024;
   /* ...everything else not more than 4M */
   if (!isMinerva && RTOP > 4096 * 1024) 
      RTOP = 4096 * 1024;

   rtop_hard = RTOP;

   if (isMinerva)
   {
      qlscreen.xres = qlscreen.xres & (~(7));
      qlscreen.linel = qlscreen.xres/4;
      qlscreen.qm_len = qlscreen.linel*qlscreen.yres;
      
      qlscreen.qm_lo = 128 * 1024;
      qlscreen.qm_hi=128 * 1024 + qlscreen.qm_len;
      if (qlscreen.qm_len>0x8000)
      {
         if ( ((long)RTOP-qlscreen.qm_len)<256*1024+8192)
         {
            /*RTOP+=qlscreen.qm_len;*/
            printf("sorry, not enough RAM for such a big screen\n");
            goto bsfb;
         }
         qlscreen.qm_lo=((RTOP-qlscreen.qm_len)>>15)<<15;    /* RTOP MUST BE 32K aligned.. */
         qlscreen.qm_hi=qlscreen.qm_lo+qlscreen.qm_len;
         RTOP=qlscreen.qm_lo;
      }
   }
   else  /* JS doesn't handle big screen */
   {
      bsfb:
      qlscreen.linel = 128;
      qlscreen.yres = 256;
      qlscreen.xres = 512;
      
      qlscreen.qm_lo = 128 * 1024;
      qlscreen.qm_hi = 128 * 1024 + 32 * 1024;
      qlscreen.qm_len = 0x8000;
   }
  
   vm_setscreen();

#ifndef XAW
   if (!script)
      x_screen_open(0);
#endif

#ifdef TRACE
  TraceInit();
#endif
   EmulatorTable(tbuff);
#ifdef G_reg
   reg = _reg;
#endif
   if (!isMinerva)
   {
      table[IPC_CMD_CODE] = UseIPC;        /* installl pseudoops */
      table[IPCR_CMD_CODE] = ReadIPC;
      table[IPCW_CMD_CODE] = WriteIPC;
      table[KEYTRANS_CMD_CODE] = QL_KeyTrans;
      
      table[FSTART_CMD_CODE] = FastStartup;
   }
   table[ROMINIT_CMD_CODE] = InitROM;
   table[MDVIO_CMD_CODE] = MdvIO;
   table[MDVO_CMD_CODE] = MdvOpen;
   table[MDVC_CMD_CODE] = MdvClose;
   table[MDVSL_CMD_CODE] = MdvSlaving;
   table[MDVFO_CMD_CODE] = MdvFormat;
   table[POLL_CMD_CODE] = PollCmd;

#if defined(USE_VM) || defined(VM_SCR)
   table[REGEMU_CMD_CODE]=RegEmuCmd;
#endif

#ifdef SERIAL
#ifndef NEWSERIAL
   table[OSERIO_CMD_CODE] = SerIO;
   table[OSERO_CMD_CODE] = SerOpen;
   table[OSERC_CMD_CODE] = SerClose;
#endif
#endif

   table[SCHEDULER_CMD_CODE] = SchedulerCmd;
   if (isMinerva)
   {
      table[MIPC_CMD_CODE] = KbdCmd;
      table[KBENC_CMD_CODE] = KBencCmd;
   }
   table[BASEXT_CMD_CODE]=BASEXTCmd;
#ifdef AUTO_BOOT
   table[0x4e43] = btrap3;
#endif
  
#ifdef NEW_AREG
   g_reg = reg;
#else
   aReg = reg + 8;
   sp = &reg[15];
#endif

   /* setup memory map */
   uqlx_protect(0,(0xffffffff & ADDR_MASK)+1, QX_NONE);
   uqlx_protect(0,RTOP,QX_RAM);

   uqlx_protect(0,64*1024,QX_ROM);                /* QL ROM, ext ROMs     */

   uqlx_protect(64*1024,32768,QX_QXM);            /* hidden emulator add-on */

   uqlx_protect(64*1024+32768,pagesize, QX_IO);      /* QL I/O       */


   /* QX_NONE must be used for extended screen mem while startup to avoid
      false postives during memory testing */
   if (qlscreen.qm_lo == 131072)
      uqlx_protect(qlscreen.qm_lo,qlscreen.qm_len,QX_SCR);
   else
   {  
      /* ignore memarea of old screen */   
      uqlx_protect(131072,32768,QX_RAM);
      /* make sure screen mem isn't recognised as RAM,
       undoe this effect in init_xscreen() */
      uqlx_protect(qlscreen.qm_lo,qlscreen.qm_len,QX_NONE);
   }

   init_timers();
  
   InitialSetup();
  
   if (isMinerva)
   {
      reg[1]=RTOP&(~16383) | 1 | 2 | 4 | 16;
      SetPC(0x186);
   }
  
#if defined(VM_SCR) || defined(USE_VM)
   vm_on ();
#endif

   QLdone = 0;
}

#ifndef XAW
void QLRun(void)
#else
   Cond CPUWork(void)
#endif
   {
      int scrchange,i;

#ifndef XAW
      exec:
#endif
   ExecuteChunk(3000);
#ifdef UX_WAIT
   if (run_reaper)
      qm_reaper();
#endif
  
   if (do_update && screen_drawable && doscreenflush && !script && !QLdone)
   {
#if defined(VM_SCR) || defined(USE_VM) || defined (EVM_SCR)
      scrchange=0;
      for(i=0;i<sct_size;i++)
         scrchange=scrchange || scrModTable[i];
      
      if(scrchange)
#else
      if ((displayTo-displayFrom>0 && displayFrom!=0) || 
          (rx1<=rx2 && ry1<=ry2))
#endif
      {
         FlushDisplay();
#if !defined(VM_SCR) && !defined(EVM_SCR)
         displayFrom=0; 
         displayTo=0; 
#endif          
         doscreenflush=0;
         scrcnt=5;
      }  
   } 
#ifdef VTIME   
   if ((qlttc--)<=0)
   {
      qlttc=3750;
      qltime++;
   }
  if ((qitc--)<0)
   {
      qitc=3;
      doPoll=1;
      /*FrameInt();*/   
      poll_req++;
      /*dosignal();*/
   }
#endif
  
#ifndef XAW
   if (!QLdone) 
      goto  exec;
  

   cleanup(0);
#else
   return 0;
#endif
}