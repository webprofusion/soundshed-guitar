/*=============================================================================
   Elements' host layer for UIKit (iOS and iPadOS).

   Modelled on Elements' Cocoa host (lib/host/macos/base_view.mm) and
   compiled into Soundshed Nano Q in its place on iOS: QPlug compiles the
   host layer into each plugin from Q_PLUG_ELEMENTS_HOST_SOURCES, and
   nanoq/CMakeLists.txt points that at this folder. See
   docs/plans/nano-q-ios.md.

   Drawing is Artist's Skia backend on the GPU (Ganesh on Metal), as
   Elements' Cocoa host draws with Skia: the view's layer is a CAMetalLayer.
   Or, built with Artist's Quartz 2D backend, on the CPU: a UIView's
   drawRect: context is a CGContext, flipped (origin at the top left) as the
   Cocoa view is. Either way the window is kept between draws (a GPU surface,
   or a bitmap), and only what changed is drawn into it. Input:

   - The first finger is the left mouse button: the cursor moves to it, then
     click down, drag, click up. Other fingers are ignored while it is down.
   - A finger held still for half a second is a right click. The left click
     it began is released far outside the view first, so the control it
     landed on takes it as a press dragged away and does nothing.
   - Two fingers pan as a scroll wheel does, in the natural direction.
   - A hardware keyboard (iOS 13.4 and later) sends keys and text.

   Distributed under the MIT License (https://opensource.org/licenses/MIT),
   as Elements is.
=============================================================================*/
#include <elements/base_view.hpp>
#include <elements/support/error_handler.hpp>
#include <artist/resources.hpp>
#include <artist/font.hpp>
#include <infra/assert.hpp>
#include <infra/filesystem.hpp>
#import <UIKit/UIKit.h>
#import <CoreText/CoreText.h>
#include <cmath>
#include <map>
#include <vector>

#if defined(ARTIST_SKIA)
# import <Metal/Metal.h>
# import <QuartzCore/CAMetalLayer.h>
# include <SkCanvas.h>
# include <SkColorSpace.h>
# include <SkImage.h>
# include <SkSurface.h>
# include <ganesh/GrBackendSurface.h>
# include <ganesh/GrDirectContext.h>
# include <ganesh/SkSurfaceGanesh.h>
# include <ganesh/mtl/GrMtlBackendContext.h>
# include <ganesh/mtl/GrMtlBackendSurface.h>
# include <ganesh/mtl/GrMtlDirectContext.h>
# include <ganesh/mtl/GrMtlTypes.h>
#elif !defined(ARTIST_QUARTZ_2D)
# error The UIKit host draws with Artist's Skia or Quartz 2D backend only
#endif

#if ! __has_feature(objc_arc)
# error "ARC is off"
#endif

namespace ph = cycfi::elements;
namespace fs = cycfi::fs;
using key_map = std::map<ph::key_code, ph::key_action>;
using cycfi::artist::canvas;

///////////////////////////////////////////////////////////////////////////////
// Resources: the app bundle, which on iOS is flat, its resources beside the
// executable.
namespace
{
   fs::path resource_path()
   {
      return fs::path{[[[NSBundle mainBundle] resourcePath] fileSystemRepresentation]};
   }

   void activate_font(fs::path const& font_path)
   {
      auto furl = [NSURL fileURLWithPath : [NSString stringWithUTF8String : font_path.c_str()]];
      CYCFI_ASSERT(furl, "Error: Unexpected missing font.");

      CFErrorRef error = nullptr;
      if (!CTFontManagerRegisterFontsForURL(
            (__bridge CFURLRef) furl, kCTFontManagerScopeProcess, &error))
      {
         // Already registered is benign: the font is there.
         auto code = error ? CFErrorGetCode(error) : 0;
         if (error)
            CFRelease(error);
         if (code != kCTFontManagerErrorAlreadyRegistered)
            ph::error_handler::get().on_resource_error(
               ph::error_id::font_load_failed,
               "font registration failed: " + font_path.string());
      }
   }

   struct resource_setter
   {
      resource_setter()
      {
         auto path = resource_path();
         cycfi::artist::add_search_path(path);

         // The fonts beside the executable, which Elements' other hosts find
         // in their bundle's Resources. An app could list them under
         // UIAppFonts instead; registering them here keeps the one rule.
         std::error_code ec;
         for (fs::directory_iterator it{path, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec))
            if (it->path().extension() == ".ttf" || it->path().extension() == ".otf")
               activate_font(it->path());
      }
   };
}

namespace cycfi::elements
{
   void init_resources()
   {
      static resource_setter init_;
   }
}

namespace cycfi::artist
{
   void init_paths()
   {
      add_search_path(resource_path());
   }

   // Declared in font.hpp
   fs::path get_user_fonts_directory()
   {
      return resource_path();
   }
}

namespace cycfi::elements
{
   // Defined in key.mm
   key_code    translate_key(long hid_usage);
   int         translate_flags(UIKeyModifierFlags flags);
}

namespace
{
   // How long a still finger takes to become a right click, and how far it
   // may wander first, in points.
   constexpr double  long_press_seconds = 0.5;
   constexpr float   long_press_slop = 10;

   // Where a cancelled left click is released: outside any view.
   constexpr float   far_away = -1e6f;

   bool handle_key(key_map& keys, ph::base_view& _view, ph::key_info k)
   {
      using ph::key_action;
      bool repeated = false;

      if (k.action == key_action::release && keys[k.key] == key_action::release)
         return false;

      if (k.action == key_action::press && keys[k.key] == key_action::press)
         repeated = true;

      keys[k.key] = k.action;

      if (repeated)
         k.action = key_action::repeat;

      return _view.key(k);
   }
}

///////////////////////////////////////////////////////////////////////////////
// ElementsView Interface

// The classes are named per plugin, as on macOS (see the Cocoa host): the
// Objective-C runtime keeps one flat namespace of class names for the
// whole process.
#if !defined(ELEMENTS_CLASS_PREFIX)
# define ELEMENTS_CLASS_PREFIX
#endif

#define ELEMENTS_PASTE_(a, b) a##b
#define ELEMENTS_PASTE(a, b) ELEMENTS_PASTE_(a, b)
#define ELEMENTS_VIEW_CLASS ELEMENTS_PASTE(ELEMENTS_CLASS_PREFIX, ElementsView)
#define ELEMENTS_PROXY_CLASS ELEMENTS_PASTE(ELEMENTS_CLASS_PREFIX, ElementsTickProxy)

// The display link holds its target strongly, so it targets this, which
// holds the view weakly, and the view can go away while the link runs.
@interface ELEMENTS_PROXY_CLASS : NSObject
@property (nonatomic, weak) id target;
@end

#if ELEMENTS_PASTE(ELEMENTS_CLASS_PREFIX, 1) != 1
@compatibility_alias ElementsTickProxy ELEMENTS_PROXY_CLASS;
#endif

@interface ELEMENTS_VIEW_CLASS : UIView
{
   CADisplayLink*                   _link;
   key_map                          _keys;
   ph::base_view*                   _view;
   float                            _scale;

   // The finger that is the mouse, and where it is.
   UITouch* __weak                  _touch;
   CGPoint                          _touch_start;
   CGPoint                          _cursor;
   bool                             _long_press_fired;
   NSTimer*                         _long_press;

   // The two-finger pan, as scroll wheel deltas.
   UIPanGestureRecognizer*          _pan;
   CGPoint                          _pan_last;

   // What was invalidated since the last draw: each area apart, or the
   // whole view.
   std::vector<CGRect>              _pending_rects;
   bool                             _pending_full;

#if defined(ARTIST_SKIA)
   // Skia on Metal, and the window as last drawn (see render).
   id<MTLDevice>                    _device;
   id<MTLCommandQueue>              _queue;
   sk_sp<GrDirectContext>           _gr_context;
   sk_sp<SkSurface>                 _surface;
   bool                             _needs_render;
#else
   // The window as last drawn (see drawRect:).
   CGContextRef                     _backing;
#endif
}

- (void) on_tick;
- (CGPoint) cursor_point;
- (void) invalidate_all;
- (void) invalidate_rect : (CGRect) area;

@end

#if ELEMENTS_PASTE(ELEMENTS_CLASS_PREFIX, 1) != 1
@compatibility_alias ElementsView ELEMENTS_VIEW_CLASS;
#endif

@implementation ElementsTickProxy

- (void) on_tick : (CADisplayLink*) link
{
   ElementsView* target = self.target;
   if (target)
      [target on_tick];
   else
      [link invalidate];
}

@end

@implementation ElementsView

#if defined(ARTIST_SKIA)
+ (Class) layerClass
{
   return [CAMetalLayer class];
}
#endif

- (void) elements_init : (ph::base_view*) view_
{
   cycfi::elements::init_resources();

   _view = view_;
   _scale = [UIScreen mainScreen].scale;
   self.contentScaleFactor = _scale;
   self.opaque = YES;
   _pending_full = true;
#if defined(ARTIST_SKIA)
   _device = MTLCreateSystemDefaultDevice();
   _queue = [_device newCommandQueue];
   if (!_device || !_queue)
   {
      ph::error_handler::get().on_resource_error(
         ph::error_id::no_gpu_device, "No Metal device found");
      throw std::runtime_error{"No Metal device found"};
   }
   auto layer = (CAMetalLayer*) self.layer;
   layer.device = _device;
   layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
   // The window is copied in from a surface of our own (see render).
   layer.framebufferOnly = NO;
   layer.opaque = YES;
   layer.contentsScale = _scale;

   GrMtlBackendContext backend{};
   backend.fDevice.retain((__bridge GrMTLHandle) _device);
   backend.fQueue.retain((__bridge GrMTLHandle) _queue);
   _gr_context = GrDirectContexts::MakeMetal(backend);
   if (!_gr_context)
   {
      ph::error_handler::get().on_resource_error(
         ph::error_id::no_gpu_device, "Failed to create Skia Metal GrDirectContext");
      throw std::runtime_error{"Failed to create Skia Metal GrDirectContext"};
   }
   _needs_render = true;
#else
   self.contentMode = UIViewContentModeRedraw;
   // drawRect: covers all of the area it is given, from its own bitmap.
   self.clearsContextBeforeDrawing = NO;
#endif
   self.backgroundColor = [UIColor blackColor];
   self.multipleTouchEnabled = YES;
   self.clipsToBounds = YES;

   // Elements' poll, which runs its io_context (posted work, timers) and
   // ends stale tracking: once a frame, where the Cocoa host ticks every
   // 10 ms.
   auto proxy = [ElementsTickProxy new];
   proxy.target = self;
   _link = [CADisplayLink displayLinkWithTarget : proxy selector : @selector(on_tick:)];
   [_link addToRunLoop : [NSRunLoop mainRunLoop] forMode : NSRunLoopCommonModes];

   _pan = [[UIPanGestureRecognizer alloc] initWithTarget : self action : @selector(on_pan:)];
   _pan.minimumNumberOfTouches = 2;
   _pan.maximumNumberOfTouches = 2;
   _pan.cancelsTouchesInView = NO;
   [self addGestureRecognizer : _pan];

   // Deferred so on_open runs after construction, as on macOS.
   __weak ElementsView* weak_self = self;
   dispatch_async(dispatch_get_main_queue(), ^{
      ElementsView* strong_self = weak_self;
      if (strong_self && strong_self->_view)
      {
         auto b = [strong_self bounds].size;
         strong_self->_view->on_open({float(b.width), float(b.height)}, strong_self->_scale);
      }
   });
}

- (void) dealloc
{
   [_link invalidate];
   [_long_press invalidate];
   _view = nullptr;
#if defined(ARTIST_SKIA)
   _surface.reset();
   if (_gr_context)
      _gr_context->releaseResourcesAndAbandonContext();
   _gr_context.reset();
#else
   CGContextRelease(_backing);
#endif
}

- (void) on_tick
{
   if (_view)
      _view->poll();
#if defined(ARTIST_SKIA)
   if (_needs_render)
      [self render];
#endif
}

// The C++ view is gone. A host may keep this UIView alive past it.
- (void) elements_detach
{
   _view = nullptr;
   [_link invalidate];
   _link = nil;
   [self cancel_long_press];
}

- (float) hdpi_scale
{
   return _scale;
}

- (CGPoint) cursor_point
{
   return _cursor;
}

- (void) setFrame : (CGRect) frame
{
   auto const old = self.bounds.size;
   [super setFrame : frame];
   [self size_changed : old];
}

- (void) setBounds : (CGRect) bounds
{
   auto const old = self.bounds.size;
   [super setBounds : bounds];
   [self size_changed : old];
}

- (void) size_changed : (CGSize) old
{
   auto const size = self.bounds.size;
   if (size.width != old.width || size.height != old.height)
   {
      // UIKit redraws a resized view itself: all of it, not the areas pending.
      _pending_full = true;
#if defined(ARTIST_SKIA)
      _needs_render = true;
#endif
      if (_view)
         _view->on_size_change({float(size.width), float(size.height)});
   }
}

- (void) didMoveToWindow
{
   [super didMoveToWindow];
   if (!self.window)
   {
      if (_view)
         _view->end_focus();
      return;
   }

   auto scale = float(self.window.screen.scale);
   if (scale != _scale)
   {
      _scale = scale;
      _pending_full = true;
      self.contentScaleFactor = scale;
#if defined(ARTIST_SKIA)
      self.layer.contentsScale = scale;
      _needs_render = true;
#endif
      if (_view)
         _view->on_scale_change(_scale);
   }
   [self becomeFirstResponder];
   if (_view)
      _view->begin_focus();
}

- (BOOL) canBecomeFirstResponder
{
   return YES;
}

#if defined(ARTIST_SKIA)

// Draws an area of the window into the surface that keeps it, clipped to it
// (Elements draws only what meets the clip).
- (void) draw_surface : (CGRect) area
{
   SkCanvas* sk = _surface->getCanvas();
   sk->save();
   sk->scale(_scale, _scale);
   sk->clipRect(SkRect::MakeXYWH(area.origin.x, area.origin.y, area.size.width, area.size.height));
   sk->drawColor(SK_ColorBLACK, SkBlendMode::kSrc);
   {
      auto cnv = canvas{sk};
      _view->draw(cnv);
   }
   sk->restore();
}

// Once a frame (from on_tick), when something was invalidated: the areas
// invalidated are drawn, each apart, into a GPU surface that keeps the
// window, which is then copied to the layer's next drawable and presented.
// A drawable comes with no contents of its own, so the whole window goes to
// it each time: a copy on the GPU. What is drawn is only what changed, since
// the CPU still records every draw. The whole view, a resize or a scale
// change draws it all, as do more than a few areas (their union).
- (void) render
{
   _needs_render = false;
   if (!_view || !_gr_context)
      return;
   // No GPU work in the background: iOS refuses it.
   if ([UIApplication sharedApplication].applicationState == UIApplicationStateBackground)
   {
      _needs_render = true;
      return;
   }

   auto layer = (CAMetalLayer*) self.layer;
   auto const size = self.bounds.size;
   int const w = int(std::ceil(size.width * _scale));
   int const h = int(std::ceil(size.height * _scale));
   if (w <= 0 || h <= 0)
      return;
   if (int(layer.drawableSize.width) != w || int(layer.drawableSize.height) != h)
      layer.drawableSize = CGSizeMake(w, h);

   if (!_surface || _surface->width() != w || _surface->height() != h)
   {
      _surface = SkSurfaces::RenderTarget(
         _gr_context.get(), skgpu::Budgeted::kYes,
         SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
         0, kTopLeft_GrSurfaceOrigin, nullptr);
      _pending_full = true;
      if (!_surface)
         return;
   }

   auto rects = std::move(_pending_rects);
   _pending_rects.clear();
   if (_pending_full)
   {
      [self draw_surface : self.bounds];
   }
   else if (rects.size() > 8)
   {
      auto all = rects.front();
      for (auto const& r : rects)
         all = CGRectUnion(all, r);
      [self draw_surface : all];
   }
   else
   {
      for (auto const& r : rects)
         [self draw_surface : r];
   }
   _pending_full = false;

   id<CAMetalDrawable> drawable = [layer nextDrawable];
   if (!drawable)
   {
      // Drawn into the surface all the same: the copy waits for a drawable.
      _needs_render = true;
      return;
   }
   GrMtlTextureInfo info;
   info.fTexture.retain((__bridge GrMTLHandle) drawable.texture);
   auto target = SkSurfaces::WrapBackendRenderTarget(
      _gr_context.get(), GrBackendRenderTargets::MakeMtl(w, h, info),
      kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, nullptr, nullptr);
   if (target)
   {
      SkPaint copy;
      copy.setBlendMode(SkBlendMode::kSrc);
      target->getCanvas()->drawImage(_surface->makeImageSnapshot(), 0, 0, SkSamplingOptions{}, &copy);
      _gr_context->flushAndSubmit(target.get(), GrSyncCpu::kNo);
   }
   id<MTLCommandBuffer> present = [_queue commandBuffer];
   [present presentDrawable : drawable];
   [present commit];
}

#else

// The window as last drawn, at the screen's pixels, with UIKit's top-left
// origin in points. Made again (and all of it drawn) when the size or scale
// changes. False if there is nothing to make it for.
- (bool) ensure_backing
{
   auto const size = self.bounds.size;
   auto const w = size_t(std::ceil(size.width * _scale));
   auto const h = size_t(std::ceil(size.height * _scale));
   if (_backing && CGBitmapContextGetWidth(_backing) == w && CGBitmapContextGetHeight(_backing) == h)
      return true;

   CGContextRelease(_backing);
   _backing = nullptr;
   if (w == 0 || h == 0)
      return false;
   auto space = CGColorSpaceCreateDeviceRGB();
   _backing = CGBitmapContextCreate(
      nullptr, w, h, 8, 0, space, uint32_t(kCGImageAlphaNoneSkipFirst) | uint32_t(kCGBitmapByteOrder32Little));
   CGColorSpaceRelease(space);
   if (!_backing)
      return false;
   CGContextTranslateCTM(_backing, 0, CGFloat(h));
   CGContextScaleCTM(_backing, CGFloat(w) / size.width, -CGFloat(h) / size.height);
   _pending_full = true;
   return true;
}

// Draws an area of the window into the backing bitmap, clipped to it
// (Elements draws only what meets the clip).
- (void) draw_backing : (CGRect) area
{
   CGContextSaveGState(_backing);
   CGContextClipToRect(_backing, area);
   CGContextSetFillColorWithColor(_backing, [UIColor blackColor].CGColor);
   CGContextFillRect(_backing, area);
   {
      auto cnv = canvas{(cycfi::artist::canvas_impl*) _backing};
      _view->draw(cnv);
   }
   CGContextRestoreGState(_backing);
}

// UIKit merges every area invalidated in a frame into one rectangle, so a
// meter at each end of the window and a light in the middle cost a band
// across all of it; Quartz draws on the CPU, and the time that takes is time
// the touches wait. So each area invalidated is drawn apart, clipped to
// itself, into a bitmap that keeps the window, and the rectangle UIKit asks
// for is copied from it. The backing store cannot keep what lies between the
// areas: Core Animation double-buffers a layer and copies back only what is
// outside that rectangle, so inside it anything not drawn comes out black or
// stale. More than a few areas, or a draw UIKit asks for itself, draws the
// rectangle given; the whole view, a resize or a scale change draws it all.
- (void) drawRect : (CGRect) dirty
{
   if (!_view)
      return;
   auto context = UIGraphicsGetCurrentContext();
   auto rects = std::move(_pending_rects);
   _pending_rects.clear();

   if (![self ensure_backing])
   {
      _pending_full = false;
      auto cnv = canvas{(cycfi::artist::canvas_impl*) context};
      _view->draw(cnv);
      return;
   }

   UIGraphicsPushContext(_backing);
   if (_pending_full)
      [self draw_backing : self.bounds];
   else if (rects.empty() || rects.size() > 8)
      [self draw_backing : dirty];
   else
      for (auto const& r : rects)
         [self draw_backing : r];
   UIGraphicsPopContext();
   _pending_full = false;

   // Copy the rectangle UIKit asks for, pixel for pixel. The image shares
   // the bitmap's memory until the bitmap is next drawn into; it is released
   // before then, so nothing is copied but what is drawn here.
   auto const height = self.bounds.size.height;
   auto image = CGBitmapContextCreateImage(_backing);
   if (!image)
      return;
   auto const pixels = CGRectIntersection(
      CGRectIntegral(CGRectMake(
         dirty.origin.x * _scale, dirty.origin.y * _scale,
         dirty.size.width * _scale, dirty.size.height * _scale)),
      CGRectMake(0, 0, CGImageGetWidth(image), CGImageGetHeight(image)));
   auto part = CGRectIsEmpty(pixels) ? nullptr : CGImageCreateWithImageInRect(image, pixels);
   if (part)
   {
      auto const to = CGRectMake(
         pixels.origin.x / _scale, pixels.origin.y / _scale,
         pixels.size.width / _scale, pixels.size.height / _scale);
      CGContextSaveGState(context);
      CGContextSetBlendMode(context, kCGBlendModeCopy);
      CGContextSetInterpolationQuality(context, kCGInterpolationNone);
      // UIKit's context is flipped; an image draws upright in an unflipped one.
      CGContextTranslateCTM(context, 0, height);
      CGContextScaleCTM(context, 1, -1);
      CGContextDrawImage(
         context, CGRectMake(to.origin.x, height - CGRectGetMaxY(to), to.size.width, to.size.height),
         part);
      CGContextRestoreGState(context);
      CGImageRelease(part);
   }
   CGImageRelease(image);
}

#endif

- (void) invalidate_all
{
   _pending_full = true;
   _pending_rects.clear();
#if defined(ARTIST_SKIA)
   _needs_render = true;
#else
   [self setNeedsDisplay];
#endif
}

- (void) invalidate_rect : (CGRect) area
{
   // A pixel round it for antialiased edges, and areas that touch become one.
   area = CGRectIntegral(CGRectInset(area, -1, -1));
   if (!_pending_full)
   {
      bool merged = false;
      for (auto& r : _pending_rects)
      {
         if (CGRectIntersectsRect(CGRectInset(r, -4, -4), area))
         {
            r = CGRectUnion(r, area);
            merged = true;
            break;
         }
      }
      if (!merged)
         _pending_rects.push_back(area);
   }
#if defined(ARTIST_SKIA)
   _needs_render = true;
#else
   [self setNeedsDisplayInRect : area];
#endif
}

///////////////////////////////////////////////////////////////////////////////
// Touches

- (ph::mouse_button) button : (ph::mouse_button::what) what
                         at : (CGPoint) pos
                       down : (bool) down
                     clicks : (int) clicks
{
   return {down, clicks, what, 0, {float(pos.x), float(pos.y)}};
}

- (void) touchesBegan : (NSSet<UITouch*>*) touches withEvent : (UIEvent*) event
{
   if (!_view || _touch || event.allTouches.count > 1)
      return;

   UITouch* touch = [touches anyObject];
   _touch = touch;
   _touch_start = _cursor = [touch locationInView : self];
   _long_press_fired = false;

   _view->cursor({float(_cursor.x), float(_cursor.y)}, ph::cursor_tracking::entering);
   _view->click([self button : ph::mouse_button::left at : _cursor down : true
      clicks : int(touch.tapCount)]);

   _long_press = [NSTimer scheduledTimerWithTimeInterval : long_press_seconds
                                                 target : self
                                               selector : @selector(on_long_press:)
                                               userInfo : nil
                                                repeats : NO];
}

- (void) touchesMoved : (NSSet<UITouch*>*) touches withEvent : (UIEvent*) event
{
   if (!_view || !_touch || ![touches containsObject : _touch] || _long_press_fired)
      return;

   _cursor = [_touch locationInView : self];
   if (std::hypot(_cursor.x - _touch_start.x, _cursor.y - _touch_start.y) > long_press_slop)
      [self cancel_long_press];

   auto const p = ph::point{float(_cursor.x), float(_cursor.y)};
   _view->cursor(p, ph::cursor_tracking::hovering);
   _view->drag([self button : ph::mouse_button::left at : _cursor down : true
      clicks : int(_touch.tapCount)]);
}

- (void) touchesEnded : (NSSet<UITouch*>*) touches withEvent : (UIEvent*) event
{
   [self finish_touch : touches];
}

// A cancelled touch (a system gesture, a call) still lets go of the button,
// or the control under it would think it is held for ever.
- (void) touchesCancelled : (NSSet<UITouch*>*) touches withEvent : (UIEvent*) event
{
   [self finish_touch : touches];
}

- (void) finish_touch : (NSSet<UITouch*>*) touches
{
   if (!_touch || ![touches containsObject : _touch])
      return;

   [self cancel_long_press];
   auto touch = _touch;
   _touch = nil;
   if (!_view || _long_press_fired)
      return;

   _cursor = [touch locationInView : self];
   _view->click([self button : ph::mouse_button::left at : _cursor down : false
      clicks : int(touch.tapCount)]);
   _view->cursor({float(_cursor.x), float(_cursor.y)}, ph::cursor_tracking::leaving);
}

- (void) cancel_long_press
{
   [_long_press invalidate];
   _long_press = nil;
}

- (void) on_long_press : (NSTimer*) timer
{
   _long_press = nil;
   if (!_view || !_touch)
      return;

   _long_press_fired = true;
   _view->click([self button : ph::mouse_button::left at : CGPointMake(far_away, far_away)
      down : false clicks : 1]);
   _view->click([self button : ph::mouse_button::right at : _cursor down : true clicks : 1]);
   _view->click([self button : ph::mouse_button::right at : _cursor down : false clicks : 1]);
}

- (void) on_pan : (UIPanGestureRecognizer*) pan
{
   if (!_view)
      return;

   auto const t = [pan translationInView : self];
   if (pan.state == UIGestureRecognizerStateBegan)
   {
      _pan_last = t;

      // A second finger turns a press into a scroll: let go of the first,
      // away from whatever it pressed.
      if (_touch && !_long_press_fired)
      {
         [self cancel_long_press];
         _long_press_fired = true;
         _view->click([self button : ph::mouse_button::left at : CGPointMake(far_away, far_away)
            down : false clicks : 1]);
      }
      return;
   }
   if (pan.state != UIGestureRecognizerStateChanged)
      return;

   auto const at = [pan locationInView : self];
   auto const dx = float(t.x - _pan_last.x);
   auto const dy = float(t.y - _pan_last.y);
   _pan_last = t;
   if (dx != 0 || dy != 0)
      _view->scroll({dx, dy}, {float(at.x), float(at.y)});
}

///////////////////////////////////////////////////////////////////////////////
// A hardware keyboard. UIKey is iOS 13.4; before it, there are no keys.

- (bool) handle_presses : (NSSet<UIPress*>*) presses action : (ph::key_action) action
{
   if (!_view)
      return false;

   bool handled = false;
   if (@available(iOS 13.4, *))
   {
      for (UIPress* press in presses)
      {
         UIKey* key = press.key;
         if (!key)
            continue;

         auto const code = ph::translate_key(long(key.keyCode));
         auto const mods = ph::translate_flags(key.modifierFlags);
         bool const key_handled = handle_key(_keys, *_view, {code, action, mods});

         bool text_handled = false;
         if (action == ph::key_action::press && !key_handled
            && !(key.modifierFlags & (UIKeyModifierCommand | UIKeyModifierControl)))
         {
            NSString* chars = key.characters;
            for (NSUInteger i = 0, n = chars.length; i != n; ++i)
            {
               auto const codepoint = [chars characterAtIndex : i];
               if (codepoint < 32 || (codepoint > 126 && codepoint < 160))
                  continue;
               if ((codepoint & 0xff00) == 0xf700) // function keys
                  continue;
               text_handled = _view->text({codepoint, mods}) || text_handled;
            }
         }
         handled = handled || key_handled || text_handled;
      }
   }
   return handled;
}

- (void) pressesBegan : (NSSet<UIPress*>*) presses withEvent : (UIPressesEvent*) event
{
   if (![self handle_presses : presses action : ph::key_action::press])
      [super pressesBegan : presses withEvent : event];
}

- (void) pressesEnded : (NSSet<UIPress*>*) presses withEvent : (UIPressesEvent*) event
{
   if (![self handle_presses : presses action : ph::key_action::release])
      [super pressesEnded : presses withEvent : event];
}

- (void) pressesCancelled : (NSSet<UIPress*>*) presses withEvent : (UIPressesEvent*) event
{
   if (![self handle_presses : presses action : ph::key_action::release])
      [super pressesCancelled : presses withEvent : event];
}

@end // @implementation ElementsView

namespace cycfi::elements
{
   namespace
   {
      ElementsView* get_view(ph::host_view_handle h)
      {
         return (__bridge ElementsView*) h;
      }

      ElementsView* make_view(base_view* owner, CGRect frame)
      {
         ElementsView* content = [[ElementsView alloc] initWithFrame : frame];
         content.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
         [content elements_init : owner];
         return content;
      }
   }

   // The handle holds the view: a retain taken here, given back in the
   // destructor, as the Cocoa host's ARC bridge does.
   base_view::base_view(extent size_)
   {
      auto content = make_view(this, CGRectMake(0, 0, size_.x, size_.y));
      _view = (__bridge_retained host_view_handle) content;
   }

   base_view::base_view(host_window_handle h)
   {
      UIWindow* window_ = (__bridge UIWindow*) h;
      auto content = make_view(this, window_.bounds);
      _view = (__bridge_retained host_view_handle) content;
      [window_ addSubview : content];
   }

   base_view::base_view(host_view_handle h)
   {
      UIView* parent_view = (__bridge UIView*) h;
      auto frame = parent_view? parent_view.bounds : CGRectMake(0, 0, 100, 100);
      auto content = make_view(this, frame);
      _view = (__bridge_retained host_view_handle) content;
      if (parent_view)
         [parent_view addSubview : content];
   }

   base_view::~base_view()
   {
      ElementsView* ui_view = (__bridge_transfer ElementsView*) _view;
      [ui_view elements_detach];
      [ui_view removeFromSuperview];
      _view = nullptr;
   }

   point base_view::cursor_pos() const
   {
      auto p = [get_view(host()) cursor_point];
      return {float(p.x), float(p.y)};
   }

   extent base_view::size() const
   {
      auto bounds = [get_view(host()) bounds];
      return {float(bounds.size.width), float(bounds.size.height)};
   }

   void base_view::size(elements::extent size_)
   {
      auto ui_view = get_view(host());
      auto frame = ui_view.frame;
      frame.size = CGSizeMake(size_.x, size_.y);
      ui_view.frame = frame;
   }

   void base_view::refresh()
   {
      [get_view(host()) invalidate_all];
   }

   void base_view::refresh(rect area)
   {
      [get_view(host())
         invalidate_rect : CGRectMake(area.left, area.top, area.width(), area.height())];
   }

   // Nothing is dropped on the view (no UIDropInteraction), so the types
   // are only kept, for accepted_drop_types.
   void base_view::accept_drop_types(mime_types const& types)
   {
      _drop_types = types;
   }

   // The keyboard is UIKit's, with no composition drawn in the view, so
   // there is no caret to place anything at.
   void base_view::text_caret(rect /* bounds */)
   {
   }

   // The clipboard is the general pasteboard, and holds text only.
   std::vector<std::string> clipboard_types()
   {
      if ([UIPasteboard generalPasteboard].hasStrings)
         return {"text/plain"};
      return {};
   }

   bool clipboard_contains(std::string_view mime_type)
   {
      return mime_type == "text/plain" && [UIPasteboard generalPasteboard].hasStrings;
   }

   payload get_clipboard(mime_types const& types)
   {
      payload data;
      if (types.count("text/plain"))
      {
         if (NSString* text = [UIPasteboard generalPasteboard].string)
            data["text/plain"] = std::string{[text UTF8String]};
      }
      return data;
   }

   // Replaces what the pasteboard holds, as on the other hosts, with the
   // payload's text (and empties it for a payload with none).
   void set_clipboard(payload const& data)
   {
      auto* pasteboard = [UIPasteboard generalPasteboard];
      pasteboard.items = @[];
      if (!contains_plain_text(data))
         return;
      auto const text = get_plain_text(data);
      NSString* str = [[NSString alloc] initWithBytes : text.data()
                                               length : text.size()
                                             encoding : NSUTF8StringEncoding];
      if (str)
         pasteboard.string = str;
   }

   // No cursor to set on a touch screen. (An iPad's pointer takes its shape
   // from UIPointerInteraction, which is not wired up.)
   void set_cursor(cursor_type /* type */)
   {
   }

   // Touch scrolling follows the fingers: natural, as on a Mac by default.
   point scroll_direction()
   {
      return {+1.0f, +1.0f};
   }
}
