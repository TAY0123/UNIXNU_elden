// SPDX-License-Identifier: BSD-2-Clause-Patent
// Elden-specific inherited-state adapter. Generic DWC3/CDC core remains MIT.
// This is independent of QcomXnuUsbDebug's private translated-DMA adapter.
#if defined(ELDEN_USB_DEBUG) && ELDEN_USB_DEBUG
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include "EldenUsbResources.h"
extern "C" {
#include "Dwc3.h"
// Same declaration as XNU's private processor.h; symbol is present in the
// owned source kernel. Do not assume the private master_processor macro is data.
processor_t current_processor(void);
processor_t thread_bind(processor_t);
#if defined(ELDEN_RECOVERY_USB)
unsigned elden_usb_read_log(uint64_t *, void *, unsigned, uint64_t *);
#endif
#if defined(ELDEN_OWNER_STARTUP) && ELDEN_OWNER_STARTUP
extern volatile int native_owner_cache_status, native_owner_data_status, elden_init_exec_accepted;
#endif
}
#if defined(ELDEN_RECOVERY_USB)
static uint64_t eldenUsbLogCursor, eldenUsbLogLost;

static size_t eldenUsbTakeLog(uint8_t *out, size_t limit) {
  return elden_usb_read_log(&eldenUsbLogCursor, out, static_cast<unsigned>(limit), &eldenUsbLogLost);
}

static uint64_t eldenUsbDropped() { return eldenUsbLogLost; }
#else
static volatile unsigned gEldenUserTrapSeen;
extern "C" void elden_note_user_trap(int pid) {
#if defined(ELDEN_OWNER_STARTUP) && ELDEN_OWNER_STARTUP
  if(pid==1&&elden_init_exec_accepted)__atomic_store_n(&gEldenUserTrapSeen,1,__ATOMIC_RELEASE);
#else
  (void)pid;
#endif
}
static struct {
  volatile unsigned lock;
  unsigned head,tail,used;
  uint64_t dropped;
  uint8_t data[131072];
} gEldenUsbLog;
extern "C" void elden_usb_log_putc(char c) {
  if(!__sync_bool_compare_and_swap(&gEldenUsbLog.lock,0,1)) {
    __atomic_fetch_add(&gEldenUsbLog.dropped,1,__ATOMIC_RELAXED);return;
  }
  if(gEldenUsbLog.used==sizeof(gEldenUsbLog.data))__atomic_fetch_add(&gEldenUsbLog.dropped,1,__ATOMIC_RELAXED);
  else {
    gEldenUsbLog.data[gEldenUsbLog.head]=(uint8_t)c;
    gEldenUsbLog.head=(gEldenUsbLog.head+1)&(sizeof(gEldenUsbLog.data)-1);++gEldenUsbLog.used;
  }
  __sync_lock_release(&gEldenUsbLog.lock);
}
static size_t eldenUsbTakeLog(uint8_t *out,size_t limit) {
  if(!__sync_bool_compare_and_swap(&gEldenUsbLog.lock,0,1))return 0;
  size_t n=gEldenUsbLog.used<limit?gEldenUsbLog.used:limit;
  for(size_t i=0;i<n;i++){out[i]=gEldenUsbLog.data[gEldenUsbLog.tail];gEldenUsbLog.tail=(gEldenUsbLog.tail+1)&(sizeof(gEldenUsbLog.data)-1);}
  gEldenUsbLog.used-=(unsigned)n;__sync_lock_release(&gEldenUsbLog.lock);return n;
}
static uint64_t eldenUsbDropped() { return __atomic_load_n(&gEldenUsbLog.dropped,__ATOMIC_RELAXED); }
#endif
static bool eldenUsbAcquire(volatile unsigned *lock) {
  unsigned expected = 0;
  return __atomic_compare_exchange_n(lock,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
}

static struct {
  OSSharedPtr<IOMemoryMap> gccMap,regsMap,smmuMap,qsMap;
  IOBufferMemoryDescriptor *dma;
  uintptr_t gcc,regs,smmu,qs;
  uint64_t physical,txBytes;
  processor_t processor;
  void *cpu;
  uint64_t context[DWC3_CONTEXT_BYTES/8];
  volatile unsigned busy,attempted;
  volatile unsigned initState;
  const char *stage;
  char session[40];
  bool active,failed,readyReported,userReported;
  uint32_t traceEvent,traceA,traceB;
  char command[64];unsigned commandBytes;bool commandOverflow;
#if defined(ELDEN_RECOVERY_USB)
  bool management;
  unsigned hostSeen;
  unsigned resetRequest;
#endif
} gEldenUsb;
static struct dwc3 *eldenDwc(){return reinterpret_cast<struct dwc3*>(gEldenUsb.context);}
static uint32_t eldenUsbRead(uintptr_t base,uint32_t offset) {
  uint32_t value=read32(base,offset);__asm__ volatile("dmb oshld":::"memory");return value;
}
static void eldenUsbWrite(uintptr_t base,uint32_t offset,uint32_t value) {
  uintptr_t p=base+offset;
  __asm__ volatile("str %w0, [%1]\n\tdsb sy"::"r"(value),"r"(p):"memory");
}
static uint32_t eldenDwcRead(void *,uint32_t offset) {
  if(offset>=ELDEN_USB_DWC_BYTES||(offset&3)){gEldenUsb.failed=true;return 0;}
  return eldenUsbRead(gEldenUsb.regs,offset);
}
static void eldenDwcWrite(void *,uint32_t offset,uint32_t value) {
  if(offset>=ELDEN_USB_DWC_BYTES||(offset&3)){gEldenUsb.failed=true;return;}
  eldenUsbWrite(gEldenUsb.regs,offset,value);
}
static void eldenDwcDelay(void *,uint32_t us){IODelay(us);}
static void eldenDwcBarrier(void *){__asm__ volatile("dsb sy":::"memory");}
static void eldenDwcSync(void *,void *cpu,size_t bytes,enum dwc3_sync_direction direction) {
  uintptr_t p=(uintptr_t)cpu,base=(uintptr_t)gEldenUsb.cpu;
  if(p<base||p>base+DWC3_DMA_BYTES||bytes>base+DWC3_DMA_BYTES-p){gEldenUsb.failed=true;return;}
  uintptr_t end=(p+bytes+63)&~(uintptr_t)63;
  for(p&=~(uintptr_t)63;p<end;p+=64) {
    if(direction==DWC3_TO_DEVICE)__asm__ volatile("dc cvac, %0"::"r"(p):"memory");
    else __asm__ volatile("dc ivac, %0"::"r"(p):"memory");
  }
  __asm__ volatile("dsb sy":::"memory");
}
static void eldenDwcTrace(void *,enum dwc3_trace event,uint32_t a,uint32_t b) {
  // Do not recurse into logging while the USB core owns its state.
  gEldenUsb.traceEvent=event;gEldenUsb.traceA=a;gEldenUsb.traceB=b;
}
static void eldenUsbReply(const char *s){
  size_t n=strlen(s),queued=dwc3_queued(eldenDwc());
  if(queued>DWC3_TX_BYTES||n+2>DWC3_TX_BYTES-queued)return;
  dwc3_queue(eldenDwc(),"\r\n",2);dwc3_queue(eldenDwc(),s,n);
}
static void eldenUsbCommand() {
  gEldenUsb.command[gEldenUsb.commandBytes]=0;
  if(gEldenUsb.commandOverflow)eldenUsbReply("ELDEN ERR command-length\r\n");
  else if(!strcmp(gEldenUsb.command,"ping"))eldenUsbReply("ELDEN PONG v1\r\n");
  else if(!strcmp(gEldenUsb.command,"status")) {
    struct dwc3_snapshot s;dwc3_snapshot(eldenDwc(),&s);char line[320];
#if defined(ELDEN_OWNER_STARTUP) && ELDEN_OWNER_STARTUP
    snprintf(line,sizeof(line),"ELDEN STATUS v1 session=%s configured=%u dtr=%u events=%u tx=%llu dropped=%llu cache=%d data=%d init_exec=%d user_trap=%u fastboot=unqualified\r\n",gEldenUsb.session,
             s.configured,s.dtr,s.events,gEldenUsb.txBytes,eldenUsbDropped(),
             native_owner_cache_status,native_owner_data_status,elden_init_exec_accepted,__atomic_load_n(&gEldenUserTrapSeen,__ATOMIC_ACQUIRE));
#else
    const char *fastboot = "unqualified";
#if defined(ELDEN_RECOVERY_USB)
    if (gEldenUsb.management) fastboot = "enabled";
#endif
    snprintf(line,sizeof(line),"ELDEN STATUS v1 session=%s configured=%u dtr=%u events=%u tx=%llu dropped=%llu fastboot=%s\r\n",gEldenUsb.session,
             s.configured,s.dtr,s.events,gEldenUsb.txBytes,eldenUsbDropped(),fastboot);
#endif
    eldenUsbReply(line);
  } else if(!strcmp(gEldenUsb.command,"reboot-fastboot")||!strcmp(gEldenUsb.command,"reboot")) {
#if defined(ELDEN_RECOVERY_USB)
    if (gEldenUsb.management) {
      gEldenUsb.resetRequest = !strcmp(gEldenUsb.command,"reboot-fastboot") ? 2 : 1;
      eldenUsbReply(gEldenUsb.resetRequest == 2 ? "ELDEN ACK reboot-fastboot\r\n" : "ELDEN ACK reboot\r\n");
    } else
#endif
    eldenUsbReply("ELDEN ERR reboot-unqualified; use the validated recovery path\r\n");
  } else if(gEldenUsb.commandBytes) {
#if defined(ELDEN_RECOVERY_USB)
    if (gEldenUsb.management) eldenUsbReply("ELDEN COMMANDS ping status reboot reboot-fastboot\r\n");
    else
#endif
    eldenUsbReply("ELDEN COMMANDS ping status\r\n");
  }
  gEldenUsb.commandBytes=0;gEldenUsb.commandOverflow=false;
}
static void eldenUsbPump(bool commands) {
  if(dwc3_poll(eldenDwc(),16)<0){gEldenUsb.failed=true;return;}
  if(commands) {
    uint8_t rx[128];size_t n=dwc3_read(eldenDwc(),rx,sizeof(rx));
    for(size_t i=0;i<n;i++) {
      uint8_t c=rx[i];
      if(c=='\r'||c=='\n')eldenUsbCommand();
      else if(c>=32&&c<=126) {
        if(gEldenUsb.commandBytes<sizeof(gEldenUsb.command)-1)gEldenUsb.command[gEldenUsb.commandBytes++]=(char)c;
        else gEldenUsb.commandOverflow=true;
      }
    }
  }
  if(dwc3_ready(eldenDwc())) {
    uint8_t bytes[512];size_t queued=dwc3_queued(eldenDwc());
    if(queued>DWC3_TX_BYTES){gEldenUsb.failed=true;return;}
    size_t available=DWC3_TX_BYTES-queued;if(available>sizeof(bytes))available=sizeof(bytes);
    size_t n=eldenUsbTakeLog(bytes,available);
    if(n){size_t sent=dwc3_queue(eldenDwc(),bytes,n);gEldenUsb.txBytes+=sent;if(sent!=n)gEldenUsb.failed=true;}
  }
}
#if defined(ELDEN_RECOVERY_USB)
static void eldenUsbReset(bool fastboot)
{
  register uint64_t x0 __asm__("x0") = fastboot ? 0xc4000012 : 0x84000009;
  register uint64_t x1 __asm__("x1") = fastboot ? 0x80010001 : 0;
  register uint64_t x2 __asm__("x2") = fastboot ? 2 : 0;
  register uint64_t x3 __asm__("x3") = 0;
  __asm__ volatile("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) ::
                   "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17", "cc", "memory");
}

static bool eldenUsbDrain(unsigned attempts)
{
  for (unsigned i = 0; i < attempts && dwc3_ready(eldenDwc()); ++i) {
    if (dwc3_poll(eldenDwc(), 8) < 0) return false;
    struct dwc3_snapshot state;
    dwc3_snapshot(eldenDwc(), &state);
    if (!state.queued && !(state.active_endpoints & (1u << 5))) return true;
    IODelay(1000);
  }
  return false;
}

#ifdef ELDEN_RECOVERY_RPMH_READ
static void eldenRpmhReadWorker(void *, wait_result_t)
{
  elden_adreno830_run_rpmh_read();
  thread_terminate(current_thread());
}

static void eldenUsbStartRpmhRead()
{
  if (!gEldenRpmhReadDeadline || !eldenUsbAcquire(&gEldenUsb.busy)) return;
  uint64_t required;
  clock_interval_to_deadline(3, 1000000000, &required);
  const char begin[] = "\r\nELDEN-A830: RPMh MX read worker starting\r\n";
  bool sent = required < gEldenRpmhReadDeadline && eldenUsbDrain(300) &&
      dwc3_queue(eldenDwc(), begin, sizeof(begin) - 1) == sizeof(begin) - 1 &&
      eldenUsbDrain(300);
  // This worker owns the start decision. Consume it before releasing USB.
  gEldenRpmhReadDeadline = 0;
  __atomic_store_n(&gEldenUsb.busy, 0, __ATOMIC_RELEASE);
  if (!sent) {
    IOLog("ELDEN-A830: RPMh read skipped, console drain or reset deadline unavailable\n");
    return;
  }
  thread_t thread = nullptr;
  if (kernel_thread_start(eldenRpmhReadWorker, nullptr, &thread) != KERN_SUCCESS) {
    IOLog("ELDEN-A830: RPMh read worker could not start\n");
    return;
  }
  thread_deallocate(thread);
}
#endif

static bool eldenUsbWaitForPanicHost()
{
  uint64_t deadline;
  clock_interval_to_deadline(12, 1000000000, &deadline);
  while (!dwc3_ready(eldenDwc()) && mach_absolute_time() < deadline) {
    if (dwc3_poll(eldenDwc(), 8) < 0 || gEldenUsb.failed) return false;
    IODelay(1000);
  }
  return dwc3_ready(eldenDwc());
}

static bool eldenUsbSendPanic(const char *message)
{
  if (!gEldenUsb.management || !gEldenUsb.active || gEldenUsb.failed ||
      current_processor() != gEldenUsb.processor || !eldenUsbAcquire(&gEldenUsb.busy)) return false;
  bool sent = false;
  if (eldenUsbWaitForPanicHost() && eldenUsbDrain(300)) {
    // Replay the retained prefix and tail before the final panic packet. Do not
    // handle received commands or take a blocking lock in the panic context.
    for (unsigned i = 0; i < 160; ++i) {
      uint8_t log[512];
      size_t bytes = eldenUsbTakeLog(log, sizeof(log));
      if (!bytes || dwc3_queue(eldenDwc(), log, bytes) != bytes || !eldenUsbDrain(25)) break;
    }
    const char begin[] = "\r\nELDEN PANIC BEGIN\r\n";
    const char end[] = "\r\nELDEN PANIC END\r\n";
    size_t size = message ? strnlen(message, 4095) : 0;
    sent = dwc3_queue(eldenDwc(), begin, sizeof(begin) - 1) == sizeof(begin) - 1 &&
        (!size || dwc3_queue(eldenDwc(), message, size) == size) &&
        dwc3_queue(eldenDwc(), end, sizeof(end) - 1) == sizeof(end) - 1 &&
        eldenUsbDrain(600);
  }
  __atomic_store_n(&gEldenUsb.busy, 0, __ATOMIC_RELEASE);
  return sent;
}

extern "C" void elden_usb_capture_panic(const char *message);
void elden_usb_capture_panic(const char *message)
{
  if (!__atomic_exchange_n(&gEldenPanicResetArmed, 0, __ATOMIC_ACQ_REL)) return;
  if (!eldenUsbSendPanic(message)) {
    // The QR is already rendered. Use the hardware timer before kernel clocks are ready.
    uint64_t frequency, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0\n\tmrs %1, cntvct_el0" : "=r"(frequency), "=r"(start));
    do {
      __asm__ volatile("yield\n\tmrs %0, cntvct_el0" : "=r"(now));
    } while (now - start < 8 * frequency);
  }
  eldenUsbReset(true);
  eldenUsbReset(false);
}
#endif
#if !defined(ELDEN_RECOVERY_USB)
extern "C" void elden_usb_panic_drain(void) {
  if(!gEldenUsb.active||gEldenUsb.failed||current_processor()!=gEldenUsb.processor)return;
  if(!eldenUsbAcquire(&gEldenUsb.busy))return;
  // Bounded, nonallocating drain; never execute received commands during panic.
  for(unsigned i=0;i<4&&!gEldenUsb.failed;i++)eldenUsbPump(false);
  __atomic_store_n(&gEldenUsb.busy,0,__ATOMIC_RELEASE);
}
#endif
static bool eldenUsbInitialize() {
  gEldenUsb.stage="cache geometry";
  uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
  if((4U<<((ctr>>16)&15))!=64){IOLog("ELDEN_USB: unsupported cache-line size\n");return false;}
  gEldenUsb.stage="inherited clocks and power";
  gEldenUsb.gcc=mapPhys(ELDEN_USB_GCC_PHYS,ELDEN_USB_GCC_BYTES,gEldenUsb.gccMap);if(!gEldenUsb.gcc)return false;
  uint32_t power=eldenUsbRead(gEldenUsb.gcc,ELDEN_USB_GDSCR),master=eldenUsbRead(gEldenUsb.gcc,ELDEN_USB_MASTER_CLK);
  uint32_t cfg=eldenUsbRead(gEldenUsb.gcc,ELDEN_USB_CFG_CLK),aggre=eldenUsbRead(gEldenUsb.gcc,ELDEN_USB_AGGRE_CLK);
  IOLog("ELDEN_USB: SM8750 power=%08x master=%08x cfg=%08x aggre=%08x\n",power,master,cfg,aggre);
  if(!(power&0x80000000U)||(power&1)||((master|cfg|aggre)&0x80000000U)) {
    IOLog("ELDEN_USB: inherited clocks/power unavailable; no controller write\n");return false;
  }
  gEldenUsb.stage="MMIO mapping and inherited device role";
  gEldenUsb.regs=mapPhys(ELDEN_USB_DWC_PHYS,ELDEN_USB_DWC_BYTES,gEldenUsb.regsMap);
  gEldenUsb.smmu=mapPhys(ELDEN_USB_SMMU_PHYS,ELDEN_USB_SMMU_BYTES,gEldenUsb.smmuMap);
  gEldenUsb.qs=mapPhys(ELDEN_USB_QSCRATCH_PHYS,ELDEN_USB_QSCRATCH_BYTES,gEldenUsb.qsMap);
  if(!gEldenUsb.regs||!gEldenUsb.smmu||!gEldenUsb.qs)return false;
  uint32_t id=eldenDwcRead(nullptr,0xc120),ctl=eldenDwcRead(nullptr,0xc110);
  unsigned width=eldenDwcRead(nullptr,0xc140)>>24;
  IOLog("ELDEN_USB: core=%08x gctl=%08x DMAwidth=%u\n",id,ctl,width);
  if(((id>>16)!=0x5533&&(id>>16)!=0x3331)||((ctl>>12)&3)!=2||width<32||width>64) {
    IOLog("ELDEN_USB: inherited core/device role refused; no controller write\n");return false;
  }
  gEldenUsb.stage="contiguous DMA allocation and width";
  gEldenUsb.dma=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous)|
      static_cast<IOOptionBits>(kIOMemoryMapperNone),DWC3_DMA_BYTES,4096);
  if(!gEldenUsb.dma)return false;
  gEldenUsb.cpu=gEldenUsb.dma->getBytesNoCopy();IOByteCount contiguous=0;
  gEldenUsb.physical=gEldenUsb.dma->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
  if(!gEldenUsb.cpu||!gEldenUsb.physical||(gEldenUsb.physical&4095)||contiguous<DWC3_DMA_BYTES||
      (width<64&&gEldenUsb.physical+DWC3_DMA_BYTES>(1ULL<<width)))return false;
  gEldenUsb.stage="inherited SID40 identity";
  auto route=eldenUfsIdentity(ELDEN_USB_SID,gEldenUsb.physical,DWC3_DMA_BYTES,
      [](unsigned offset){return eldenUsbRead(gEldenUsb.smmu,offset);});
  IOLog("ELDEN_USB: SID40 slot=%u CB=%u smr=%x s2cr=%x cbar=%x sctlr=%x\n",route.slot,route.bank,route.smr,route.s2cr,route.cbar,route.sctlr);
  if(route.error){IOLog("ELDEN_USB: inherited DMA identity refused; no SMMU/controller writes\n");return false;}
  const struct dwc3_platform io={nullptr,eldenDwcRead,eldenDwcWrite,eldenDwcDelay,eldenDwcSync,eldenDwcBarrier,eldenDwcTrace};
  const struct dwc3_identity identity={0x1209,0x7004,"Elden Lab","Y700 XNU Debug","ELDEN-XNU"};
  gEldenUsb.stage="DWC3 device initialization";
  int status=dwc3_init(gEldenUsb.context,sizeof(gEldenUsb.context),&io,&identity,
                       gEldenUsb.cpu,gEldenUsb.physical,DWC3_DMA_BYTES);
  if(status||gEldenUsb.failed){IOLog("ELDEN_USB: DWC3 init failed %d; DMA retained\n",status);return false;}
  // Qualcomm documented VBUS/session-valid override; preserve other fields.
  uint32_t hs=eldenUsbRead(gEldenUsb.qs,0x10),ss=eldenUsbRead(gEldenUsb.qs,0x30);
  eldenUsbWrite(gEldenUsb.qs,0x10,hs|(1U<<20)|(1U<<28));
  eldenUsbWrite(gEldenUsb.qs,0x30,ss|(1U<<24));
  gEldenUsb.stage="DWC3 start";
  status=dwc3_start(eldenDwc());
  if(status){IOLog("ELDEN_USB: start failed %d; DMA retained\n",status);return false;}
  gEldenUsb.active=true;
  gEldenUsb.stage="awaiting host enumeration";
  IOLog("ELDEN_USB: CDC started DMA=%llx; awaiting PC DTR; no SMMU writes\n",gEldenUsb.physical);
  return true;
}
static void eldenUsbWorker(void *,wait_result_t) {
  gEldenUsb.processor=current_processor();
  thread_bind(gEldenUsb.processor);
  if(current_processor()!=gEldenUsb.processor)thread_block(THREAD_CONTINUE_NULL);
  if(current_processor()!=gEldenUsb.processor||!eldenUsbInitialize()) {
    IOLog("ELDEN_USB: unavailable at %s\n",gEldenUsb.stage?gEldenUsb.stage:"worker affinity");
#if defined(ELDEN_RECOVERY_USB)
    char status[96];
    snprintf(status, sizeof(status), "ELDEN USB unavailable: %s", gEldenUsb.stage ? gEldenUsb.stage : "worker affinity");
    sheng_log_status(status);
#endif
    __atomic_store_n(&gEldenUsb.initState,3,__ATOMIC_RELEASE);thread_terminate(current_thread());return;
  }
  __atomic_store_n(&gEldenUsb.initState,2,__ATOMIC_RELEASE);
  for(;;) {
    if(gEldenUsb.failed)break;
    if(eldenUsbAcquire(&gEldenUsb.busy)) {
      eldenUsbPump(true);
#if defined(ELDEN_RECOVERY_USB)
      if (gEldenUsb.resetRequest) {
        bool fastboot = gEldenUsb.resetRequest == 2;
        gEldenUsb.resetRequest = 0;
        eldenUsbDrain(300);
        eldenUsbReset(fastboot);
        eldenUsbReply("ELDEN ERR firmware reset returned\r\n");
      }
#endif
      bool ready=dwc3_ready(eldenDwc());
#if defined(ELDEN_RECOVERY_USB)
      if (ready) __atomic_store_n(&gEldenUsb.hostSeen, 1, __ATOMIC_RELEASE);
#endif
      __atomic_store_n(&gEldenUsb.busy,0,__ATOMIC_RELEASE);
      if(ready&&!gEldenUsb.readyReported){gEldenUsb.readyReported=true;IOLog("ELDEN_USB: host connected; console stream active\n");}
#ifdef ELDEN_RECOVERY_RPMH_READ
      if (ready) eldenUsbStartRpmhRead();
#endif
#if !defined(ELDEN_RECOVERY_USB)
      if(__atomic_load_n(&gEldenUserTrapSeen,__ATOMIC_ACQUIRE)&&!gEldenUsb.userReported){gEldenUsb.userReported=true;IOLog("ELDEN_BOOT: PID1 AArch64 EL0 BSD syscall observed after init exec\n");}
#endif
    }
    IOSleep(1);
  }
  if(eldenUsbAcquire(&gEldenUsb.busy)){dwc3_stop(eldenDwc());__atomic_store_n(&gEldenUsb.busy,0,__ATOMIC_RELEASE);}
  IOLog("ELDEN_USB: stopped trace=%u a=%x b=%x; DMA retained\n",gEldenUsb.traceEvent,gEldenUsb.traceA,gEldenUsb.traceB);
  thread_terminate(current_thread());
}
static void eldenStartUsbDebug() {
  unsigned enabled=0;PE_parse_boot_argn("elden-usb-debug",&enabled,sizeof(enabled));
  if(enabled!=1)return;
  if(eldenUsbAcquire(&gEldenUsb.attempted)) {
#if defined(ELDEN_RECOVERY_USB)
    unsigned management = 0;
    PE_parse_boot_argn("elden-usb-management", &management, sizeof(management));
    gEldenUsb.management = management == 1;
#endif
    strlcpy(gEldenUsb.session,"unbound",sizeof(gEldenUsb.session));
    PE_parse_boot_argn("elden-debug-session",gEldenUsb.session,sizeof(gEldenUsb.session));
    gEldenUsb.session[sizeof(gEldenUsb.session)-1]=0;
    bool sessionValid=strlen(gEldenUsb.session)==16;
    for(unsigned i=0;sessionValid&&i<16;i++)if(!((gEldenUsb.session[i]>='0'&&gEldenUsb.session[i]<='9')||
        (gEldenUsb.session[i]>='a'&&gEldenUsb.session[i]<='f')))sessionValid=false;
    if(!sessionValid)strlcpy(gEldenUsb.session,"unbound",sizeof(gEldenUsb.session));
    gEldenUsb.stage="worker creation";__atomic_store_n(&gEldenUsb.initState,1,__ATOMIC_RELEASE);
    thread_t thread=nullptr;
    if(kernel_thread_start(eldenUsbWorker,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    else {__atomic_store_n(&gEldenUsb.initState,3,__ATOMIC_RELEASE);IOLog("ELDEN_USB: worker creation failed; controller untouched\n");}
  }
#if !defined(ELDEN_RECOVERY_USB)
  unsigned required=0;PE_parse_boot_argn("elden-usb-required",&required,sizeof(required));
  if(required==1) {
    for(unsigned i=0;i<500&&__atomic_load_n(&gEldenUsb.initState,__ATOMIC_ACQUIRE)==1;i++)IOSleep(10);
    unsigned state=__atomic_load_n(&gEldenUsb.initState,__ATOMIC_ACQUIRE);
    if(state!=2)panic("ELDEN_USB qualification: state=%u stage=%s trace=%u a=%x b=%x; controller DMA retained",state,
                     gEldenUsb.stage?gEldenUsb.stage:"unknown",gEldenUsb.traceEvent,gEldenUsb.traceA,gEldenUsb.traceB);
  }
#endif
}
#endif
