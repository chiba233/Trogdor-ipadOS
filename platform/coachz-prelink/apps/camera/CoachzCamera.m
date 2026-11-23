// SPDX-License-Identifier: BSD-2-Clause
// CoachzCamera.app: the rear camera on screen. It opens the kernel's CoachzCamera service (drivers/RamBlock/camera/CoachzCamera.cpp,
// /chosen/coachz-camera = 2), asks it to stream (the kernel powers the camera up for it and down when it stops), copies the newest
// frame (MIPI RAW10: the rear OV8856 SGRBG10 1640x1232, or the front OV5675 SBGGR10 1296x972 with /chosen/coachz-camera bit 0x800) about fifteen times a second and develops it at full size:
// black level subtracted, grey-world white balance, bilinear demosaic, sRGB gamma. Nothing in the sensor does white balance (ov8856.c
// writes its MWB gains only as one digital gain for all three), so the raw green, which two of every four photosites carry and which
// the sensor sees best, made the picture green. OV8856's black level has no value in mainline or libcamera, so it is estimated from
// each frame's darkest green photosites and shown. A simple exposure loop moves the mean towards 18 % grey with the sensor's exposure
// lines first and its analog gain after (ov8856.c ranges: exposure 6 to vts - 6, gain 128 to 2047, 128 = 1x).
#import <UIKit/UIKit.h>
#include <mach/mach.h>
#include <math.h>

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
  uint16_t *_plane;
  uint32_t *_rgb;
  uint32_t _width, _height, _stride, _bytes, _shiftX, _shiftY;
  uint64_t _exposure, _gain, _frames, _last;
  double _black, _gainR, _gainB;
  uint8_t _gamma[1024];
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
  uint64_t status[12] = {0};
  uint32_t count = 12;
  kr = IOConnectCallMethod(_connect, kSelStatus, NULL, 0, NULL, 0, status, &count, NULL, NULL);
  if (kr != KERN_SUCCESS || count < 10 || !status[6]) { [self show:[NSString stringWithFormat:@"Status failed: 0x%x", kr]]; return; }
  // The development below reads GRBG; the front OV5675's BGGR (and any other order) starts one row and/or column later.
  const uint32_t bayer = count >= 12 ? (uint32_t)status[10] : 1u;
  _shiftX = (bayer == 0 || bayer == 2) ? 1u : 0u;
  _shiftY = (bayer == 2 || bayer == 3) ? 1u : 0u;
  _width = (uint32_t)status[3]; _height = (uint32_t)status[4]; _stride = (uint32_t)status[5]; _bytes = (uint32_t)status[6];
  _exposure = status[8]; _gain = status[9];
  _raw = malloc(_bytes);
  _plane = malloc((size_t)_width * _height * sizeof(uint16_t));
  _rgb = calloc((size_t)_width * _height, 4);
  _black = 64.0; _gainR = 1.0; _gainB = 1.0;
  for (int i = 0; i < 1024; i++) {   // linear 0..1023 to sRGB 8 bit
    const double v = i / 1023.0, e = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    _gamma[i] = (uint8_t)lround(fmin(fmax(e, 0.0), 1.0) * 255.0);
  }
  IOConnectCallMethod(_connect, kSelStart, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
  _running = YES;
  [self tick];
}

// Unpacks RAW10 (four pixels in five bytes, the fifth holding each one's two low bits) into one 16-bit plane, and measures what the
// development needs: the channel means over all 2x2 cells (SGRBG: G R on even rows, B G on odd rows) and the darkest green cells.
- (void)unpackMeasure:(double *)rMean g:(double *)gMean b:(double *)bMean dark:(double *)dark
{
  const uint32_t w = _width, h = _height;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *row = _raw + (size_t)y * _stride;
    uint16_t *out = _plane + (size_t)y * w;
    for (uint32_t x = 0; x + 3 < w; x += 4, row += 5) {
      const uint8_t low = row[4];
      out[x] = (uint16_t)(row[0] << 2 | (low & 3u));
      out[x + 1] = (uint16_t)(row[1] << 2 | ((low >> 2) & 3u));
      out[x + 2] = (uint16_t)(row[2] << 2 | ((low >> 4) & 3u));
      out[x + 3] = (uint16_t)(row[3] << 2 | ((low >> 6) & 3u));
    }
  }
  if (_shiftY) {
    memmove(_plane, _plane + w, (size_t)(h - 1) * w * sizeof(uint16_t));
    memcpy(_plane + (size_t)(h - 1) * w, _plane + (size_t)(h - 3) * w, w * sizeof(uint16_t));
  }
  if (_shiftX) {
    for (uint32_t y = 0; y < h; y++) {
      uint16_t *line = _plane + (size_t)y * w;
      memmove(line, line + 1, (w - 1) * sizeof(uint16_t));
      line[w - 1] = line[w - 3];
    }
  }
  uint64_t sr = 0, sg = 0, sb = 0, n = 0;
  uint32_t hist[1024] = {0};
  for (uint32_t y = 0; y + 1 < h; y += 2) {
    const uint16_t *r0 = _plane + (size_t)y * w, *r1 = r0 + w;
    for (uint32_t x = 0; x + 1 < w; x += 2) {
      const uint32_t g = (r0[x] + r1[x + 1]) >> 1;
      sg += g; sr += r0[x + 1]; sb += r1[x]; n++;
      hist[g]++;
    }
  }
  *rMean = (double)sr / n; *gMean = (double)sg / n; *bMean = (double)sb / n;
  // The 0.2 % darkest green cells: the black level when anything in view is dark.
  uint64_t count = 0, want = n / 500;
  uint32_t level = 0;
  while (level < 1023 && count + hist[level] <= want) count += hist[level++];
  *dark = level;
}

static inline uint32_t clamp1023(int v) { return v < 0 ? 0u : v > 1023 ? 1023u : (uint32_t)v; }

- (double)convert
{
  double rMean, gMean, bMean, dark;
  [self unpackMeasure:&rMean g:&gMean b:&bMean dark:&dark];
  // Black level: follows the darkest cells slowly, never above 128 of 1023 (a dark scene, not a pedestal, beyond that).
  _black += (fmin(dark, 128.0) - _black) * 0.1;
  const double black = _black, gr = gMean - black, rr = rMean - black, br = bMean - black;
  if (gr > 4.0 && rr > 1.0 && br > 1.0) {   // grey world: the scene's average is grey
    _gainR += (fmin(fmax(gr / rr, 0.5), 4.0) - _gainR) * 0.2;
    _gainB += (fmin(fmax(gr / br, 0.5), 4.0) - _gainB) * 0.2;
  }
  const uint32_t w = _width, h = _height;
  const int bl = (int)lround(black), kr = (int)lround(_gainR * 256.0), kb = (int)lround(_gainB * 256.0);
  // Scales the range above black back to 0..1023 before gamma.
  const int span = 1023 - bl > 64 ? 1023 - bl : 64, kn = (int)lround(1023.0 * 256.0 / span);
  const uint8_t *gamma = _gamma;
  for (uint32_t y = 1; y + 1 < h; y++) {
    const uint16_t *up = _plane + (size_t)(y - 1) * w, *c = up + w, *dn = c + w;
    uint32_t *out = _rgb + (size_t)y * w;
    const bool oddRow = y & 1u;
    for (uint32_t x = 1; x + 1 < w; x++) {
      int r, g, b;
      const int here = c[x], lr = (c[x - 1] + c[x + 1]) >> 1, ud = (up[x] + dn[x]) >> 1;
      const int cross = (c[x - 1] + c[x + 1] + up[x] + dn[x]) >> 2, diag = (up[x - 1] + up[x + 1] + dn[x - 1] + dn[x + 1]) >> 2;
      const bool oddCol = x & 1u;
      if (!oddRow && !oddCol) { g = here; r = lr; b = ud; }          // G on an R row
      else if (oddRow && oddCol) { g = here; b = lr; r = ud; }       // G on a B row
      else if (!oddRow) { r = here; g = cross; b = diag; }           // R
      else { b = here; g = cross; r = diag; }                        // B
      r = ((((r - bl) * kr) >> 8) * kn) >> 8;
      g = (((g - bl) * kn) >> 8);
      b = ((((b - bl) * kb) >> 8) * kn) >> 8;
      out[x] = 0xff000000u | (uint32_t)gamma[clamp1023(r)] << 16 | (uint32_t)gamma[clamp1023(g)] << 8 | gamma[clamp1023(b)];
    }
  }
  // Linear green above black on an 8-bit scale, for the exposure loop.
  return fmax(gr, 0.0) / 4.0;
}

- (void)expose:(double)mean
{
  // Towards 18 % grey in linear terms (46 of 255; about 118 after the sRGB curve), a quarter of the way each frame; exposure lines
  // before gain.
  const double want = 46.0, ratio = mean > 1.0 ? want / mean : 4.0;
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
      const size_t w = self->_width, h = self->_height;
      CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
      CGContextRef ctx = CGBitmapContextCreate(self->_rgb, w, h, 8, w * 4, space,
                                               kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
      CGImageRef image = CGBitmapContextCreateImage(ctx);
      CGContextRelease(ctx);
      CGColorSpaceRelease(space);
      UIImage *ui = [UIImage imageWithCGImage:image];
      CGImageRelease(image);
      NSString *text = [NSString stringWithFormat:@"frame %llu  shown %llu  mean %.0f  exposure %llu  gain %llu  black %.0f  R %.2f  B %.2f",
                        number, self->_frames, mean, self->_exposure, self->_gain, self->_black, self->_gainR, self->_gainB];
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
  free(_plane);
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
