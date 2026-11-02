// SPDX-License-Identifier: BSD-2-Clause
// The cellular bridge: the IOKit side of Apple's PCIe baseband (AppleBaseband, AppleBasebandPCIControl and one
// AppleBasebandPCIInterface per channel, with the user-client classes the sandbox knows them by) over this SoC's modem. ABM and
// BBUpdater talk to these as to an MHI modem; the QMUX channels go to the modem's QMI services over QRTR (coachz_bb_qmux.h), the
// sahara channel is answered by a device end that reads and drops the firmware it is offered (coachz_bb_sahara.h), and the board
// methods are answered by coachz_bb_control.h. Started only with /chosen/coachz-baseband (tools/xnu_screen.py --baseband).
//
// Threads: the Wi-Fi thread owns QRTR (bsd/dev/coachz_wifi.c): it hands modem messages to coachz_baseband_qrtr and sends what
// coachz_baseband_poll gives it. User threads only queue. One lock covers the bridge state and the queues; user memory is copied
// outside it, and asynchronous reads complete on a thread call.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#include "coachz_bb_qmux.h"
#include "coachz_bb_sahara.h"
#include "coachz_bb_control.h"
#include "coachz_bb_channels.h"

#define BB_FRAME_MAX (64u * 1024u)       // one read or write: a QMUX frame, a SAHARA packet or a READ_DATA answer
#define BB_QUEUE 64u                     // frames waiting for ABM, per channel
#define BB_READS 8u                      // asynchronous reads waiting for a frame, per channel
#define BB_TX 64u                        // QMI messages waiting for the Wi-Fi thread
#define BB_SYNC_READ_SECONDS 30u

// Which SAHARA images the device end asks for. SBL1 (Qualcomm image id 0x15) is the first image a boot loader requests; the rest of
// a Mav16 boot (rpm, tz, apps, acdb, mba, qdsp6sw in the bbfw) follow once BBUpdater's log shows how it takes this one.
static const uint32_t bb_sahara_images[] = { 0x15 };

namespace {
struct bb_frame { uint32_t len; uint8_t *data; };
struct bb_read { OSAsyncReference64 reference; IOMemoryDescriptor *memory; uint32_t len; void *owner; };
struct bb_channel {
  bb_frame frames[BB_QUEUE];
  uint32_t head, count;
  bb_read reads[BB_READS];
  uint32_t reads_waiting;
  void *owner;
  uint32_t opens, frames_in, frames_out, frames_dropped, bytes_in, bytes_out;
};
struct bb_tx { uint32_t local_port, node, port, len; uint8_t *data; };
}

static IOLock *bb_lock;
static thread_call_t bb_complete_call;
static struct cbq_bridge bb_qmux;
static struct cbs_device bb_sahara;
static struct cbc_state bb_control;
static bb_channel bb_ch[CBH_PUBLISHED];
static bb_tx bb_txq[BB_TX];
static uint32_t bb_tx_head, bb_tx_count, bb_tx_dropped;
static void (*bb_send)(uint32_t local_port, uint32_t node, uint32_t port, const void *message, uint32_t length);
static void (*bb_del_client)(uint32_t local_port);
static int bb_published;
static uint32_t bb_private_seen[64], bb_private_count, bb_ctl_logged;
static uint8_t bb_out[BB_FRAME_MAX];     // answers built under the lock

extern "C" {
int coachz_baseband_enabled(void);
void coachz_baseband_configure(void (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t), void (*del_client)(uint32_t));
void coachz_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port);
void coachz_baseband_del_server(uint32_t service, uint32_t node, uint32_t port);
int coachz_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size);
void coachz_baseband_poll(void);
void coachz_baseband_report(unsigned second);
}

// ------------------------------------------------------------------ device tree
static OSData *bb_chosen(const char *path, const char *key)
{
  IORegistryEntry *entry = IORegistryEntry::fromPath(path, gIODTPlane);
  if (!entry) return nullptr;
  OSData *value = OSDynamicCast(OSData, entry->getProperty(key));
  if (value) value->retain();
  entry->release();
  return value;
}

extern "C" int coachz_baseband_enabled(void)
{
  static int cached = -1;
  if (cached < 0) {
    OSData *value = bb_chosen("/chosen", "coachz-baseband");
    uint32_t on = 0;
    if (value && value->getLength() == 4) memcpy(&on, value->getBytesNoCopy(), 4);
    OSSafeReleaseNULL(value);
    cached = on != 0;
  }
  return cached;
}

// ------------------------------------------------------------------ queues (bb_lock held)
static void bb_complete(thread_call_param_t, thread_call_param_t);

static void bb_init(void)
{
  if (bb_lock) return;
  bb_lock = IOLockAlloc();
  bb_complete_call = thread_call_allocate(bb_complete, nullptr);
  cbq_init(&bb_qmux);
  cbc_init(&bb_control);
  cbs_init(&bb_sahara, bb_sahara_images, sizeof bb_sahara_images / sizeof bb_sahara_images[0], 0x10000);
}

static void bb_push(uint32_t channel, const uint8_t *data, uint32_t len)
{
  bb_channel *c = &bb_ch[channel];
  uint8_t *copy = len && len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
  if (!copy || c->count == BB_QUEUE) {
    if (copy) IOFreeData(copy, len);
    c->frames_dropped++;
    return;
  }
  memcpy(copy, data, len);
  c->frames[(c->head + c->count++) % BB_QUEUE] = { len, copy };
  c->frames_out++;
  c->bytes_out += len;
  IOLockWakeup(bb_lock, c, false);
  if (c->reads_waiting && bb_complete_call) thread_call_enter(bb_complete_call);
}

static bb_frame bb_pop(uint32_t channel)
{
  bb_channel *c = &bb_ch[channel];
  bb_frame f = { 0, nullptr };
  if (!c->count) return f;
  f = c->frames[c->head];
  c->head = (c->head + 1) % BB_QUEUE;
  c->count--;
  return f;
}

static void bb_flush(uint32_t channel)
{
  for (bb_frame f = bb_pop(channel); f.data; f = bb_pop(channel)) IOFreeData(f.data, f.len);
}

// A QMI message for the modem, sent by the Wi-Fi thread from coachz_baseband_poll.
static void bb_queue_tx(uint32_t local_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t len)
{
  uint8_t *copy = len ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
  if (!copy || bb_tx_count == BB_TX) {
    if (copy) IOFreeData(copy, len);
    bb_tx_dropped++;
    return;
  }
  memcpy(copy, data, len);
  bb_txq[(bb_tx_head + bb_tx_count++) % BB_TX] = { local_port, node, port, len, copy };
}

// The modem "booted again" as ABM sees it (reset, power on): its boot loader greets on sahara.
static void bb_sahara_restart(void)
{
  struct cbs_out o;
  bb_flush(CBH_SAHARA);
  cbs_init(&bb_sahara, bb_sahara_images, sizeof bb_sahara_images / sizeof bb_sahara_images[0], 0x10000);
  cbs_start(&bb_sahara, &o);
  for (uint32_t i = 0; i < o.count; i++) bb_push(CBH_SAHARA, o.data[i], o.len[i]);
}

static void bb_private_note(uint32_t service, uint32_t message)
{
  const uint32_t key = service << 16 | (message & 0xffff);
  for (uint32_t i = 0; i < bb_private_count; i++)
    if (bb_private_seen[i] == key) return;
  if (bb_private_count < 64) bb_private_seen[bb_private_count++] = key;
  printf("COACHZ baseband: private QMI service 0x%x message 0x%x answered not supported\n", service, message);
}

// One write from ABM on a channel, after its bytes were copied in.
static void bb_host_write(uint32_t channel, const uint8_t *data, uint32_t len)
{
  bb_channel *c = &bb_ch[channel];
  c->frames_in++;
  c->bytes_in += len;
  if (channel == CBH_SAHARA) {
    const enum cbs_state before = bb_sahara.state;
    for (uint32_t at = 0; at < len && bb_sahara.state != CBS_FAILED;) {
      struct cbs_out o;
      const uint32_t used = cbs_receive(&bb_sahara, data + at, len - at, &o);
      for (uint32_t i = 0; i < o.count; i++) bb_push(CBH_SAHARA, o.data[i], o.len[i]);
      if (!used) break;
      at += used;
    }
    if (bb_sahara.state != before && bb_sahara.state == CBS_COMMAND)
      printf("COACHZ baseband: sahara command mode\n");
    if (bb_sahara.state != before && (bb_sahara.state == CBS_FINISHED || bb_sahara.state == CBS_FAILED || bb_sahara.state == CBS_WAIT_DONE))
      printf("COACHZ baseband: sahara %s, image %u of %u (id 0x%x), host mode %u status %u, last command %u, %u requests, %llu bytes\n",
             bb_sahara.state == CBS_FINISHED ? "finished" : bb_sahara.state == CBS_FAILED ? "failed" : "image read",
             bb_sahara.image_index + 1, bb_sahara.image_count, bb_sahara.images[bb_sahara.image_index], bb_sahara.host_mode,
             bb_sahara.host_status, bb_sahara.last_command, bb_sahara.requests, bb_sahara.bytes);
    return;
  }
  if (channel != CBH_QMUX1 && channel != CBH_QMUX2) return;   // counted above, nothing behind it yet
  const uint32_t qmux = channel == CBH_QMUX1 ? 0 : 1;
  const struct cbq_result r = cbq_from_host(&bb_qmux, qmux, data, len, bb_out, sizeof bb_out);
  switch (r.kind) {
  case CBQ_TO_HOST:
    bb_push(channel, bb_out, r.len);
    if (bb_qmux.ctl_other != bb_ctl_logged) {
      bb_ctl_logged = bb_qmux.ctl_other;
      printf("COACHZ baseband: CTL message 0x%x accepted without action\n", (uint32_t)(data[CBQ_HDR + 2] | data[CBQ_HDR + 3] << 8));
    }
    break;
  case CBQ_TO_MODEM:
    bb_queue_tx(r.local_port, r.node, r.port, data + CBQ_HDR, r.len);
    break;
  case CBQ_PRIVATE: {
    const uint8_t *sdu = data + CBQ_HDR;
    bb_private_note(r.service, (uint32_t)(sdu[3] | sdu[4] << 8));
    const uint32_t n = cbq_private_unsupported(sdu, r.service, r.cid, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    break;
  }
  case CBQ_DROP:
    break;
  }
}

// ------------------------------------------------------------------ asynchronous completions
// IOUserClient::sendAsyncResult64 is a protected static; this never-instantiated subclass only forwards to it.
class CoachzBasebandAsync : public IOUserClient {
public:
  static IOReturn send(OSAsyncReference64 reference, IOReturn result, io_user_reference_t *args, UInt32 count)
  {
    return sendAsyncResult64(reference, result, args, count);
  }
};

static void bb_finish_read(bb_read *read, IOReturn result, const uint8_t *data, uint32_t len)
{
  io_user_reference_t args[1] = { 0 };
  if (data && read->memory) {
    const uint32_t n = len < read->len ? len : read->len;
    args[0] = read->memory->writeBytes(0, data, n);
    if (len > read->len) result = kIOReturnOverrun;
  }
  if (read->memory) {
    read->memory->complete();
    read->memory->release();
    read->memory = nullptr;
  }
  CoachzBasebandAsync::send(read->reference, result, args, 1);
}

static void bb_complete(thread_call_param_t, thread_call_param_t)
{
  for (;;) {
    bb_read read;
    bb_frame frame = { 0, nullptr };
    bool found = false;
    IOLockLock(bb_lock);
    for (uint32_t c = 0; c < CBH_PUBLISHED && !found; c++) {
      bb_channel *ch = &bb_ch[c];
      if (!ch->reads_waiting || !ch->count) continue;
      read = ch->reads[0];
      memmove(&ch->reads[0], &ch->reads[1], (--ch->reads_waiting) * sizeof ch->reads[0]);
      frame = bb_pop(c);
      found = true;
    }
    IOLockUnlock(bb_lock);
    if (!found) return;
    bb_finish_read(&read, kIOReturnSuccess, frame.data, frame.len);
    IOFreeData(frame.data, frame.len);
  }
}

// Every read a client has waiting ends with `result` (close, abort).
static void bb_abort_reads(uint32_t channel, void *owner, IOReturn result)
{
  bb_read ended[BB_READS];
  uint32_t n = 0;
  IOLockLock(bb_lock);
  bb_channel *ch = &bb_ch[channel];
  for (uint32_t i = 0; i < ch->reads_waiting;) {
    if (!owner || ch->reads[i].owner == owner) {
      ended[n++] = ch->reads[i];
      memmove(&ch->reads[i], &ch->reads[i + 1], (--ch->reads_waiting - i) * sizeof ch->reads[0]);
    } else {
      i++;
    }
  }
  IOLockWakeup(bb_lock, ch, false);
  IOLockUnlock(bb_lock);
  for (uint32_t i = 0; i < n; i++) bb_finish_read(&ended[i], result, nullptr, 0);
}

// ------------------------------------------------------------------ user memory
static uint8_t *bb_copy_in(task_t task, mach_vm_address_t address, uint32_t len)
{
  if (!len || len > BB_FRAME_MAX) return nullptr;
  IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, len, kIODirectionOut, task);
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(len));
  bool ok = memory && copy && memory->prepare() == kIOReturnSuccess;
  if (ok) {
    ok = memory->readBytes(0, copy, len) == len;
    memory->complete();
  }
  OSSafeReleaseNULL(memory);
  if (!ok && copy) {
    IOFreeData(copy, len);
    copy = nullptr;
  }
  return copy;
}

static bool bb_async(IOExternalMethodArguments *args) { return args->asyncWakePort != MACH_PORT_NULL && args->asyncReference; }

static void bb_reply(IOExternalMethodArguments *args, IOReturn result, uint32_t count, uint64_t a = 0, uint64_t b = 0)
{
  io_user_reference_t values[2] = { a, b };
  OSAsyncReference64 reference;
  memset(reference, 0, sizeof reference);
  memcpy(reference, args->asyncReference, sizeof(io_user_reference_t) *
         (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
  CoachzBasebandAsync::send(reference, result, values, count);
}

// ------------------------------------------------------------------ AppleBasebandPCIInterface
class AppleBasebandPCIUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandPCIUserClient)
  task_t fTask;
  uint32_t fChannel;
public:
  bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override
  {
    fTask = task;
    return IOUserClient::initWithTask(task, security, type, properties);
  }
  void setChannel(uint32_t channel) { fChannel = channel; }
  IOReturn clientClose() override
  {
    closeChannel();
    terminate();
    return kIOReturnSuccess;
  }
  void closeChannel()
  {
    bb_abort_reads(fChannel, this, kIOReturnAborted);
    IOLockLock(bb_lock);
    if (bb_ch[fChannel].owner == this) bb_ch[fChannel].owner = nullptr;
    IOLockUnlock(bb_lock);
  }
  IOReturn write(IOExternalMethodArguments *args);
  IOReturn read(IOExternalMethodArguments *args);
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override;
};
OSDefineMetaClassAndStructors(AppleBasebandPCIUserClient, IOUserClient)

IOReturn AppleBasebandPCIUserClient::write(IOExternalMethodArguments *args)
{
  uint8_t *copy = nullptr;
  uint32_t len = 0;
  if (bb_async(args)) {
    if (args->scalarInputCount < 2) return kIOReturnBadArgument;
    len = (uint32_t)args->scalarInput[1];
    copy = bb_copy_in(fTask, args->scalarInput[0], len);
  } else if (args->structureInputDescriptor) {
    IOMemoryDescriptor *memory = args->structureInputDescriptor;
    len = (uint32_t)memory->getLength();
    copy = len && len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
    bool ok = copy && memory->prepare() == kIOReturnSuccess;
    if (ok) {
      ok = memory->readBytes(0, copy, len) == len;
      memory->complete();
    }
    if (!ok && copy) {
      IOFreeData(copy, len);
      copy = nullptr;
    }
  } else if (args->structureInput && args->structureInputSize) {
    len = args->structureInputSize;
    copy = len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
    if (copy) memcpy(copy, args->structureInput, len);
  }
  if (!copy) return kIOReturnBadArgument;
  IOLockLock(bb_lock);
  bb_host_write(fChannel, copy, len);
  IOLockUnlock(bb_lock);
  IOFreeData(copy, len);
  if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 1, len);
  return kIOReturnSuccess;
}

IOReturn AppleBasebandPCIUserClient::read(IOExternalMethodArguments *args)
{
  if (bb_async(args)) {
    if (args->scalarInputCount < 2 || !args->scalarInput[1]) return kIOReturnBadArgument;
    bb_read r;
    memset(&r, 0, sizeof r);
    memcpy(r.reference, args->asyncReference, sizeof(io_user_reference_t) *
           (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
    r.len = (uint32_t)args->scalarInput[1];
    r.owner = this;
    r.memory = IOMemoryDescriptor::withAddressRange(args->scalarInput[0], r.len, kIODirectionIn, fTask);
    if (!r.memory || r.memory->prepare() != kIOReturnSuccess) {
      OSSafeReleaseNULL(r.memory);
      return kIOReturnVMError;
    }
    IOLockLock(bb_lock);
    bb_channel *ch = &bb_ch[fChannel];
    const bool queued = ch->reads_waiting < BB_READS;
    if (queued) ch->reads[ch->reads_waiting++] = r;
    if (queued && ch->count && bb_complete_call) thread_call_enter(bb_complete_call);
    IOLockUnlock(bb_lock);
    if (!queued) {
      r.memory->complete();
      r.memory->release();
      return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
  }
  // synchronous: the next frame, waiting up to BB_SYNC_READ_SECONDS
  uint64_t deadline = 0;
  clock_interval_to_deadline(BB_SYNC_READ_SECONDS, kSecondScale, &deadline);
  IOLockLock(bb_lock);
  bb_channel *ch = &bb_ch[fChannel];
  int waited = THREAD_AWAKENED;
  while (!ch->count && ch->owner == this && waited == THREAD_AWAKENED)
    waited = IOLockSleepDeadline(bb_lock, ch, deadline, THREAD_INTERRUPTIBLE);
  bb_frame frame = bb_pop(fChannel);
  IOLockUnlock(bb_lock);
  if (!frame.data) return waited == THREAD_TIMED_OUT ? kIOReturnTimeout : kIOReturnAborted;
  IOReturn result = kIOReturnSuccess;
  if (args->structureOutputDescriptor) {
    IOMemoryDescriptor *memory = args->structureOutputDescriptor;
    const uint32_t room = (uint32_t)memory->getLength(), n = frame.len < room ? frame.len : room;
    if (memory->prepare() == kIOReturnSuccess) {
      memory->writeBytes(0, frame.data, n);
      memory->complete();
      args->structureOutputDescriptorSize = n;
    } else {
      result = kIOReturnVMError;
    }
    if (frame.len > room) result = kIOReturnOverrun;
  } else if (args->structureOutput) {
    const uint32_t n = frame.len < args->structureOutputSize ? frame.len : args->structureOutputSize;
    memcpy(args->structureOutput, frame.data, n);
    if (frame.len > args->structureOutputSize) result = kIOReturnOverrun;
    args->structureOutputSize = n;
  } else {
    result = kIOReturnBadArgument;
  }
  IOFreeData(frame.data, frame.len);
  return result;
}

// Selectors and shapes: coachz_bb_channels.h (libIOABP's IOConnectCall* sites).
IOReturn AppleBasebandPCIUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                                                    OSObject *, void *)
{
  if (!args) return kIOReturnBadArgument;
  switch (selector) {
  case 0: {   // open
    IOLockLock(bb_lock);
    bb_channel *ch = &bb_ch[fChannel];
    const bool free_channel = !ch->owner || ch->owner == this;
    if (free_channel) {
      ch->owner = this;
      if (!ch->opens++) printf("COACHZ baseband: %s opened\n", cbh_published[fChannel]);
      if (fChannel == CBH_SAHARA) bb_sahara_restart();
    }
    IOLockUnlock(bb_lock);
    return free_channel ? kIOReturnSuccess : kIOReturnExclusiveAccess;
  }
  case 1:     // close
    closeChannel();
    return kIOReturnSuccess;
  case 2:
    return write(args);
  case 3:
    return read(args);
  case 4:     // sendImage (BHI): the modem is already running; the image is accepted and dropped
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 2, args->scalarInputCount > 1 ? args->scalarInput[1] : 0, 0);
    return kIOReturnSuccess;
  case 6:     // abort channel: pending reads end
    bb_abort_reads(fChannel, this, kIOReturnAborted);
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 0);
    return kIOReturnSuccess;
  case 7:     // start channel
  case 9:     // map shared memory: nothing here reads it
  case 10:    // unmap
  case 12:    // deregister time event
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 0);
    return kIOReturnSuccess;
  case 11:    // register time event: none is ever sent
    return kIOReturnSuccess;
  default:    // 5 readRegister, 8 time sync: no MHI registers or time base behind this channel
    printf("COACHZ baseband: channel %s selector %u unsupported\n", cbh_published[fChannel], selector);
    return kIOReturnUnsupported;
  }
}

class AppleBasebandPCIInterface : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIInterface)
  uint32_t fChannel;
public:
  bool setup(uint32_t channel)
  {
    fChannel = channel;
    return setProperty(CBH_PROTOCOL_KEY, cbh_published[channel]);
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out) override
  {
    if (!out) return kIOReturnBadArgument;
    *out = nullptr;
    static uint32_t seen;
    if (!(__atomic_fetch_or(&seen, 1u << fChannel, __ATOMIC_RELAXED) & (1u << fChannel)))
      printf("COACHZ baseband: first user client on %s\n", cbh_published[fChannel]);
    AppleBasebandPCIUserClient *client = OSTypeAlloc(AppleBasebandPCIUserClient);
    if (!client) return kIOReturnNoMemory;
    client->setChannel(fChannel);
    if (!client->initWithTask(task, security, type, properties) || !client->attach(this)) {
      client->release();
      return kIOReturnError;
    }
    if (!client->start(this)) {
      client->detach(this);
      client->release();
      return kIOReturnError;
    }
    *out = client;
    return kIOReturnSuccess;
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type, IOUserClient **out) override
  {
    return newUserClient(task, security, type, nullptr, out);
  }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIInterface, IOService)

// ------------------------------------------------------------------ AppleBasebandPCIControl
class AppleBasebandPCIControlUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandPCIControlUserClient)
  OSAsyncReference64 fLogs;
  bool fLogsPending;
public:
  void endLogs()
  {
    if (!fLogsPending) return;
    fLogsPending = false;
    io_user_reference_t none[1] = { 0 };
    CoachzBasebandAsync::send(fLogs, kIOReturnAborted, none, 1);
  }
  IOReturn clientClose() override
  {
    endLogs();
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    switch (selector) {
    case 0:   // open
    case 3:   // logger tune
      return kIOReturnSuccess;
    case 1:   // close
      endLogs();
      return kIOReturnSuccess;
    case 2:   // read logs: there are none; the request waits until close
      if (!bb_async(args) || fLogsPending) return kIOReturnBadArgument;
      memset(fLogs, 0, sizeof fLogs);
      memcpy(fLogs, args->asyncReference, sizeof(io_user_reference_t) *
             (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
      fLogsPending = true;
      return kIOReturnSuccess;
    default:
      return kIOReturnUnsupported;
    }
  }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControlUserClient, IOUserClient)

template <class Client>
static IOReturn bb_open(IOService *service, task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out)
{
  if (!out) return kIOReturnBadArgument;
  *out = nullptr;
  Client *client = OSTypeAlloc(Client);
  if (!client) return kIOReturnNoMemory;
  if (!client->initWithTask(task, security, type, properties) || !client->attach(service)) {
    client->release();
    return kIOReturnError;
  }
  if (!client->start(service)) {
    client->detach(service);
    client->release();
    return kIOReturnError;
  }
  *out = client;
  return kIOReturnSuccess;
}

class AppleBasebandPCIControl : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIControl)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    static uint32_t seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) printf("COACHZ baseband: first user client on AppleBasebandPCIControl\n");
    return bb_open<AppleBasebandPCIControlUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControl, IOService)

// ------------------------------------------------------------------ AppleBaseband
class AppleBasebandUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    uint64_t out[4] = { 0 };
    uint32_t nout = args->scalarOutputCount < 4 ? args->scalarOutputCount : 4;
    IOLockLock(bb_lock);
    const int result = cbc_call(&bb_control, selector, args->scalarInput, args->scalarInputCount, out, &nout);
    // A reset or power-on starts the boot loader again: it greets BBUpdater on sahara.
    if (!result && (selector == CBC_RESET_MODEM || selector == CBC_POWERCYCLE || (selector == CBC_POWER && args->scalarInputCount && args->scalarInput[0])))
      bb_sahara_restart();
    IOLockUnlock(bb_lock);
    if (result == CBC_UNSUPPORTED) printf("COACHZ baseband: AppleBaseband selector %u unsupported\n", selector);
    for (uint32_t i = 0; i < nout; i++) args->scalarOutput[i] = out[i];
    args->scalarOutputCount = nout;
    return result;
  }
};
OSDefineMetaClassAndStructors(AppleBasebandUserClient, IOUserClient)

class AppleBaseband : public IOService {
  OSDeclareDefaultStructors(AppleBaseband)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    return bb_open<AppleBasebandUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleBaseband, IOService)

// ------------------------------------------------------------------ the data plane's kernel services
// ABM's data::TransportService calls ApplePDPHelperInterface::getInstance()->enableMetadataTrailer() without a null check, and getInstance
// is null without an AppleIPAppender service to open. ApplePDPHelper picks its variant by which PDP manager is registered; this is the Mav
// one. Nothing is behind them until pdp_ip exists: every AppleIPAppender method (0 addFlow ... 20 registerFilterRuleCallBack, libApplePDPHelper)
// succeeds with zeroed outputs, and the first use of each selector is logged.
class AppleIPAppenderUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleIPAppenderUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    static uint32_t seen;
    if (selector < 32 && !(__atomic_fetch_or(&seen, 1u << selector, __ATOMIC_RELAXED) & (1u << selector)))
      printf("COACHZ baseband: AppleIPAppender selector %u answered with zeros\n", selector);
    for (uint32_t i = 0; i < args->scalarOutputCount; i++) args->scalarOutput[i] = 0;
    if (args->structureOutput && args->structureOutputSize) bzero(args->structureOutput, args->structureOutputSize);
    if (args->structureOutputDescriptor) args->structureOutputDescriptorSize = 0;
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(AppleIPAppenderUserClient, IOUserClient)

class AppleIPAppender : public IOService {
  OSDeclareDefaultStructors(AppleIPAppender)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    return bb_open<AppleIPAppenderUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleIPAppender, IOService)

// libPCITransport's pci::event::PDPListener matches the base class, ApplePDPHelper the leaf and AppleBasebandPCIPDPManager.
class AppleBasebandPCIPDPManagerBase : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIPDPManagerBase)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManagerBase, IOService)

class AppleBasebandPCIPDPManager : public AppleBasebandPCIPDPManagerBase {
  OSDeclareDefaultStructors(AppleBasebandPCIPDPManager)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManager, AppleBasebandPCIPDPManagerBase)

class AppleBasebandPCIMAVPDPManager : public AppleBasebandPCIPDPManager {
  OSDeclareDefaultStructors(AppleBasebandPCIMAVPDPManager)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIMAVPDPManager, AppleBasebandPCIPDPManager)

// ------------------------------------------------------------------ publication and the Wi-Fi thread's entry points
template <class Service>
static Service *bb_publish(IOService *provider, uint32_t channel = ~0u)
{
  Service *service = OSTypeAlloc(Service);
  bool ok = service && service->init();
  if (ok && channel != ~0u) {
    AppleBasebandPCIInterface *interface = OSDynamicCast(AppleBasebandPCIInterface, service);
    ok = interface && interface->setup(channel);
  }
  ok = ok && service->attach(provider);
  if (ok && !service->start(provider)) {
    service->detach(provider);
    ok = false;
  }
  if (!ok) {
    OSSafeReleaseNULL(service);
    return nullptr;
  }
  service->registerService();
  return service;
}

// The modem's QRTR is up (GLINK started): publish the services ABM looks for. `send` and `del_client` run on the Wi-Fi thread only.
extern "C" void coachz_baseband_configure(void (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t), void (*del_client)(uint32_t))
{
  if (!coachz_baseband_enabled() || bb_published) return;
  bb_init();
  bb_send = send;
  bb_del_client = del_client;
  IOService *provider = IOService::getResourceService();
  int made = 0;
  made += provider && bb_publish<AppleBaseband>(provider);
  made += provider && bb_publish<AppleBasebandPCIControl>(provider);
  for (uint32_t c = 0; c < CBH_PUBLISHED; c++) made += provider && bb_publish<AppleBasebandPCIInterface>(provider, c);
  made += provider && bb_publish<AppleBasebandPCIMAVPDPManager>(provider);
  made += provider && bb_publish<AppleIPAppender>(provider);
  bb_published = 1;
  printf("COACHZ baseband: %d of %u services published (AppleBaseband, AppleBasebandPCIControl, %u channels, the PDP manager, AppleIPAppender)\n", made, 4u + CBH_PUBLISHED, CBH_PUBLISHED);
}

extern "C" void coachz_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port)
{
  if (!coachz_baseband_enabled()) return;
  bb_init();
  IOLockLock(bb_lock);
  cbq_server_new(&bb_qmux, service, version_instance, node, port);
  IOLockUnlock(bb_lock);
}

extern "C" void coachz_baseband_del_server(uint32_t service, uint32_t node, uint32_t port)
{
  if (!bb_lock) return;
  IOLockLock(bb_lock);
  cbq_server_del(&bb_qmux, service, node, port);
  IOLockUnlock(bb_lock);
}

// QRTR data for one of the bridge's client ports: 1 when the port is the bridge's.
extern "C" int coachz_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t, const void *body, uint32_t size)
{
  if (dst_port < CBQ_PORT_BASE || dst_port >= CBQ_PORT_BASE + CBQ_MAX_CLIENTS) return 0;
  if (!bb_lock) return 1;
  IOLockLock(bb_lock);
  const struct cbq_result r = cbq_from_modem(&bb_qmux, dst_port, src_node, static_cast<const uint8_t *>(body), size, bb_out, sizeof bb_out);
  if (r.kind == CBQ_TO_HOST) bb_push(r.channel == 0 ? CBH_QMUX1 : CBH_QMUX2, bb_out, r.len);
  IOLockUnlock(bb_lock);
  return 1;
}

// On the Wi-Fi thread: QMI requests for the modem, then DEL_CLIENT for released clients.
extern "C" void coachz_baseband_poll(void)
{
  if (!bb_lock || !bb_send) return;
  bb_tx batch[16];
  uint32_t n = 0, released[16], r = 0, port;
  IOLockLock(bb_lock);
  while (bb_tx_count && n < 16) {
    batch[n++] = bb_txq[bb_tx_head];
    bb_tx_head = (bb_tx_head + 1) % BB_TX;
    bb_tx_count--;
  }
  while (r < 16 && (port = cbq_next_release(&bb_qmux))) released[r++] = port;
  IOLockUnlock(bb_lock);
  for (uint32_t i = 0; i < n; i++) {
    bb_send(batch[i].local_port, batch[i].node, batch[i].port, batch[i].data, batch[i].len);
    IOFreeData(batch[i].data, batch[i].len);
  }
  for (uint32_t i = 0; i < r; i++)
    if (bb_del_client) bb_del_client(released[i]);
}

extern "C" void coachz_baseband_report(unsigned second)
{
  if (!bb_lock || !bb_published) return;
  IOLockLock(bb_lock);
  uint32_t clients = 0, servers = 0;
  for (uint32_t i = 0; i < CBQ_MAX_CLIENTS; i++) clients += bb_qmux.client[i].used;
  for (uint32_t i = 0; i < CBQ_MAX_SERVERS; i++) servers += bb_qmux.server[i].present;
  printf("COACHZ baseband: %u s: %u QMI services, %u clients, dropped %u/%u; sahara state %u, %u executes, %u requests; board calls %u (%u unsupported)\n",
         second, servers, clients, bb_qmux.dropped, bb_tx_dropped, bb_sahara.state, bb_sahara.executes, bb_sahara.requests, bb_control.calls, bb_control.unsupported);
  for (uint32_t c = 0; c < CBH_PUBLISHED; c++) {
    const bb_channel *ch = &bb_ch[c];
    if (ch->opens)
      printf("COACHZ baseband: %s: opens %u, in %u frames %u bytes, out %u frames %u bytes, dropped %u, queued %u, reads waiting %u\n",
             cbh_published[c], ch->opens, ch->frames_in, ch->bytes_in, ch->frames_out, ch->bytes_out, ch->frames_dropped, ch->count,
             ch->reads_waiting);
  }
  IOLockUnlock(bb_lock);
}
