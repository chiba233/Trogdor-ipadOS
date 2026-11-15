// SPDX-License-Identifier: BSD-2-Clause
// The rear camera for user space (/chosen/coachz-camera = 2): the IOService CoachzCamera and its user client CoachzCameraUserClient.
// coachz_camera.c keeps the sensor and the receive path powered after its self-test and, while a client asks, streams real images
// (MIPI RAW10, SGRBG10) into a ring of four frame buffers in low DRAM. A client reads the newest complete frame through a structure
// output; the frame is copied straight from its physical pages (IOMemoryDescriptor::readBytes, no permanent kernel mapping, since the
// ARM io_submap is short), so no memory-mapping message has to pass the sandbox. The service carries NativeSandboxGrantProcess "*",
// the property IOUserClient.cpp's native_sandbox_grant reads, so an app may open it.
//
// Selectors: 0 start streaming, 1 stop; 2 status, scalar out {state, frames completed, newest ring index, width, height, stride,
// frame bytes, ring count, exposure lines, analog gain}; 3 set exposure (lines) and analog gain (128 = 1x), scalar in; 4 copy the
// newest complete frame into the structure output (at least frame bytes), scalar out {its frame number}.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSString.h>

extern "C" {
extern volatile uint32_t coachz_camera_state, coachz_camera_want, coachz_camera_seq, coachz_camera_last;
extern volatile uint32_t coachz_camera_exposure, coachz_camera_gain, coachz_camera_settings;
uint64_t coachz_camera_frame_pa(uint32_t index);
uint32_t coachz_camera_geometry(uint32_t which);
void coachz_camera_publish(void);
}

namespace {
constexpr uint32_t kCopyChunk = 64 * 1024;
uint32_t gClients;
}

class CoachzCameraUserClient : public IOUserClient {
  OSDeclareDefaultStructors(CoachzCameraUserClient)
  bool fStreaming = false;
  uint8_t *fChunk = nullptr;

public:
  bool start(IOService *provider) override
  {
    if (!IOUserClient::start(provider)) return false;
    fChunk = static_cast<uint8_t *>(IOMallocData(kCopyChunk));
    return fChunk != nullptr;
  }
  void stop(IOService *provider) override
  {
    setStreaming(false);
    IOUserClient::stop(provider);
  }
  void free() override
  {
    if (fChunk) IOFreeData(fChunk, kCopyChunk);
    fChunk = nullptr;
    IOUserClient::free();
  }
  IOReturn clientClose() override
  {
    setStreaming(false);
    terminate();
    return kIOReturnSuccess;
  }
  void setStreaming(bool on)
  {
    if (on == fStreaming) return;
    fStreaming = on;
    const uint32_t clients = on ? __atomic_add_fetch(&gClients, 1u, __ATOMIC_ACQ_REL) : __atomic_sub_fetch(&gClients, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&coachz_camera_want, clients ? 1u : 0u, __ATOMIC_RELEASE);
  }
  IOReturn copyNewest(IOExternalMethodArguments *args)
  {
    IOMemoryDescriptor *out = args->structureOutputDescriptor;
    const uint32_t bytes = coachz_camera_geometry(3);
    if (!out || out->getLength() < bytes || args->scalarOutputCount < 1) return kIOReturnBadArgument;
    if (coachz_camera_state != 2 || !coachz_camera_seq) return kIOReturnNotReady;
    const uint32_t seq = __atomic_load_n(&coachz_camera_seq, __ATOMIC_ACQUIRE);
    IOMemoryDescriptor *frame = IOMemoryDescriptor::withPhysicalAddress(coachz_camera_frame_pa(coachz_camera_last), bytes, kIODirectionOut);
    if (!frame) return kIOReturnNoMemory;
    IOReturn result = out->prepare(kIODirectionIn);
    for (uint32_t at = 0; result == kIOReturnSuccess && at < bytes; at += kCopyChunk) {
      const uint32_t n = bytes - at < kCopyChunk ? bytes - at : kCopyChunk;
      if (frame->readBytes(at, fChunk, n) != n || out->writeBytes(at, fChunk, n) != n) result = kIOReturnIOError;
    }
    out->complete(kIODirectionIn);
    frame->release();
    args->scalarOutput[0] = seq;
    args->scalarOutputCount = 1;
    return result;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    switch (selector) {
    case 0: setStreaming(true); return kIOReturnSuccess;
    case 1: setStreaming(false); return kIOReturnSuccess;
    case 2: {
      if (args->scalarOutputCount < 10) return kIOReturnBadArgument;
      const uint64_t v[10] = {coachz_camera_state, coachz_camera_seq, coachz_camera_last, coachz_camera_geometry(0), coachz_camera_geometry(1),
                              coachz_camera_geometry(2), coachz_camera_geometry(3), coachz_camera_geometry(4), coachz_camera_exposure,
                              coachz_camera_gain};
      for (uint32_t i = 0; i < 10; i++) args->scalarOutput[i] = v[i];
      args->scalarOutputCount = 10;
      return kIOReturnSuccess;
    }
    case 3:
      if (args->scalarInputCount < 2) return kIOReturnBadArgument;
      coachz_camera_exposure = static_cast<uint32_t>(args->scalarInput[0]);
      coachz_camera_gain = static_cast<uint32_t>(args->scalarInput[1]);
      __atomic_add_fetch(&coachz_camera_settings, 1u, __ATOMIC_RELEASE);
      return kIOReturnSuccess;
    case 4: return copyNewest(args);
    default: return kIOReturnUnsupported;
    }
  }
};
OSDefineMetaClassAndStructors(CoachzCameraUserClient, IOUserClient)

class CoachzCamera : public IOService {
  OSDeclareDefaultStructors(CoachzCamera)

public:
  IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out) override
  {
    CoachzCameraUserClient *client = OSTypeAlloc(CoachzCameraUserClient);
    if (!client) return kIOReturnNoMemory;
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
OSDefineMetaClassAndStructors(CoachzCamera, IOService)

extern "C" void coachz_camera_publish(void)
{
  static bool published;
  if (published) return;
  IOService *provider = IOService::getResourceService();
  CoachzCamera *service = OSTypeAlloc(CoachzCamera);
  bool ok = provider && service && service->init();
  if (ok) {
    service->setProperty("IOUserClientClass", "CoachzCameraUserClient");
    service->setProperty("NativeSandboxGrantProcess", "*");
    ok = service->attach(provider);
  }
  if (ok && !service->start(provider)) {
    service->detach(provider);
    ok = false;
  }
  if (!ok) {
    if (service) service->release();
    IOLog("COACHZ camera: service not published\n");
    return;
  }
  service->registerService();
  published = true;
  IOLog("COACHZ camera: CoachzCamera published\n");
}
