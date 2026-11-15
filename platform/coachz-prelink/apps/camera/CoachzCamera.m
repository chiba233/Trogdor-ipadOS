// SPDX-License-Identifier: BSD-2-Clause
// CoachzCamera.app: the rear camera on screen. It opens the kernel's CoachzCamera service (drivers/RamBlock/camera/CoachzCamera.cpp,
// /chosen/coachz-camera = 2), asks it to stream, copies the newest frame (MIPI RAW10, SGRBG10, 1640x1232) about fifteen times a second,
// turns each 2x2 Bayer cell into one pixel (820x616 BGRA) and shows it. A simple exposure loop moves the mean towards mid grey with the
// sensor's exposure lines first and its analog gain after (ov8856.c ranges: exposure 6 to vts - 6, gain 128 to 2047, 128 = 1x).
#import <UIKit/UIKit.h>
#include <mach/mach.h>

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMainPortDefault;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t main, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owner, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallMethod(io_connect_t connection, uint32_t selector, const uint64_t *input, uint32_t inputCnt,
                                  const void *inputStruct, size_t inputStructCnt, uint64_t *output, uint32_t *outputCnt,
                                  void *outputStruct, size_t *outputStructCnt);

enum { kSelStart = 0, kSelStop = 1, kSelStatus = 2, kSelExposure = 3, kSelFrame = 4 };

@interface CameraController : UIViewController
@end

@implementation CameraController {
  io_connect_t _connect;
  UIImageView *_view;
  UILabel *_label;
  dispatch_queue_t _queue;
  uint8_t *_raw;
  uint32_t *_rgb;
  uint32_t _width, _height, _stride, _bytes;
  uint64_t _exposure, _gain, _frames, _last;
  BOOL _running;
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.blackColor;
  _view = [[UIImageView alloc] initWithFrame:self.view.bounds];
  _view.contentMode = UIViewContentModeScaleAspectFit;
  _view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:_view];
  _label = [[UILabel alloc] initWithFrame:CGRectMake(16, 40, 600, 24)];
  _label.textColor = UIColor.whiteColor;
  _label.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightRegular];
  [self.view addSubview:_label];
  _queue = dispatch_queue_create("camera", DISPATCH_QUEUE_SERIAL);
  [self open];
}

- (void)show:(NSString *)text
{
  dispatch_async(dispatch_get_main_queue(), ^{ self->_label.text = text; });
}

- (void)open
{
  io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("CoachzCamera"));
  if (!service) { [self show:@"No CoachzCamera service: the camera did not start (kernel log, COACHZ camera)"]; return; }
  kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &_connect);
  IOObjectRelease(service);
  if (kr != KERN_SUCCESS) { [self show:[NSString stringWithFormat:@"Open failed: 0x%x", kr]]; return; }
  uint64_t status[10] = {0};
  uint32_t count = 10;
  kr = IOConnectCallMethod(_connect, kSelStatus, NULL, 0, NULL, 0, status, &count, NULL, NULL);
  if (kr != KERN_SUCCESS || count < 10 || !status[6]) { [self show:[NSString stringWithFormat:@"Status failed: 0x%x", kr]]; return; }
  _width = (uint32_t)status[3]; _height = (uint32_t)status[4]; _stride = (uint32_t)status[5]; _bytes = (uint32_t)status[6];
  _exposure = status[8]; _gain = status[9];
  _raw = malloc(_bytes);
  _rgb = malloc((size_t)(_width / 2) * (_height / 2) * 4);
  IOConnectCallMethod(_connect, kSelStart, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
  _running = YES;
  [self tick];
}

// One RAW10 sample: four pixels share five bytes, the fifth holding each one's two low bits.
static inline uint32_t sample(const uint8_t *row, uint32_t x)
{
  const uint8_t *g = row + (x >> 2) * 5;
  return (uint32_t)g[x & 3] << 2 | ((g[4] >> ((x & 3) * 2)) & 3u);
}

- (double)convert
{
  const uint32_t w = _width / 2, h = _height / 2;
  uint64_t sum = 0;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *r0 = _raw + (size_t)(2 * y) * _stride, *r1 = r0 + _stride;
    uint32_t *out = _rgb + (size_t)y * w;
    for (uint32_t x = 0; x < w; x++) {
      // SGRBG: G R on even rows, B G on odd rows.
      const uint32_t g0 = sample(r0, 2 * x), r = sample(r0, 2 * x + 1), b = sample(r1, 2 * x), g1 = sample(r1, 2 * x + 1);
      const uint32_t R = r >> 2, G = (g0 + g1) >> 3, B = b >> 2;
      sum += G;
      out[x] = 0xff000000u | R << 16 | G << 8 | B;   // BGRA in memory, little-endian
    }
  }
  return (double)sum / ((double)w * h);
}

- (void)expose:(double)mean
{
  // Towards a mean of 110 of 255, a quarter of the way each frame; exposure lines before gain.
  const double want = 110.0, ratio = mean > 1.0 ? want / mean : 4.0;
  if (ratio > 0.9 && ratio < 1.1) return;
  double total = (double)_exposure * (double)_gain / 128.0;
  total *= 1.0 + (ratio - 1.0) * 0.25;
  const double maxExposure = 1250.0;
  uint64_t exposure = (uint64_t)fmin(fmax(total, 6.0), maxExposure);
  uint64_t gain = (uint64_t)fmin(fmax(total / (double)exposure * 128.0, 128.0), 2047.0);
  if (exposure == _exposure && gain == _gain) return;
  _exposure = exposure; _gain = gain;
  const uint64_t in[2] = {_exposure, _gain};
  IOConnectCallMethod(_connect, kSelExposure, in, 2, NULL, 0, NULL, NULL, NULL, NULL);
}

- (void)tick
{
  if (!_running) return;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 66 * NSEC_PER_MSEC), _queue, ^{
    uint64_t number = 0;
    uint32_t count = 1;
    size_t size = self->_bytes;
    kern_return_t kr = IOConnectCallMethod(self->_connect, kSelFrame, NULL, 0, NULL, 0, &number, &count, self->_raw, &size);
    if (kr == KERN_SUCCESS && number != self->_last) {
      self->_last = number;
      self->_frames++;
      const double mean = [self convert];
      [self expose:mean];
      const size_t w = self->_width / 2, h = self->_height / 2;
      CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
      CGContextRef ctx = CGBitmapContextCreate(self->_rgb, w, h, 8, w * 4, space,
                                               kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
      CGImageRef image = CGBitmapContextCreateImage(ctx);
      CGContextRelease(ctx);
      CGColorSpaceRelease(space);
      UIImage *ui = [UIImage imageWithCGImage:image];
      CGImageRelease(image);
      NSString *text = [NSString stringWithFormat:@"frame %llu  shown %llu  mean %.0f  exposure %llu  gain %llu", number,
                        self->_frames, mean, self->_exposure, self->_gain];
      dispatch_async(dispatch_get_main_queue(), ^{ self->_view.image = ui; self->_label.text = text; });
    } else if (kr != KERN_SUCCESS) {
      [self show:[NSString stringWithFormat:@"Waiting for frames (0x%x)", kr]];
    }
    [self tick];
  });
}

- (void)dealloc
{
  _running = NO;
  if (_connect) {
    IOConnectCallMethod(_connect, kSelStop, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    IOServiceClose(_connect);
  }
  free(_raw);
  free(_rgb);
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = [CameraController new];
  [self.window makeKeyAndVisible];
  return YES;
}
@end

int main(int argc, char *argv[])
{
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
  }
}
