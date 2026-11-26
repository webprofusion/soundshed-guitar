/*=============================================================================
   Skia's Metal shaders, compiled again off the main thread at launch: see
   shader_warmup.h.

   Distributed under the MIT License (https://opensource.org/licenses/MIT),
   as Elements is.
=============================================================================*/
// Skia on Metal only: the Quartz build has no shaders.
#if defined(ARTIST_SKIA)

#include "shader_warmup.h"

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#if ! __has_feature(objc_arc)
# error "ARC is off"
#endif

namespace nanoq::ios
{
namespace
{
   // What Skia m148 writes for a Metal pipeline (GrPersistentCacheUtils::
   // PackCachedShaders, and GrMtlPipelineStateBuilder's pipeline data), read
   // here without Skia's private headers. A cache from another Skia is
   // discarded before it gets here (shader_cache's header names the
   // milestone); anything that still does not parse is skipped, and Skia
   // compiles it as it would have.
   constexpr int32_t    k_cache_version = 12;
   constexpr uint32_t   k_msl_tag = (uint32_t('M') << 24) | (uint32_t('S') << 16) | (uint32_t('L') << 8) | uint32_t(' ');
   constexpr std::size_t k_interface_size = 3; // SkSL::Program::Interface: three bools

   constexpr std::size_t align4(std::size_t n) { return (n + 3) & ~std::size_t(3); }

   // SkReadBuffer's encoding: 32-bit little-endian words, byte arrays as a
   // length and the bytes padded to a word.
   struct reader
   {
      unsigned char const* data;
      std::size_t          size;
      std::size_t          pos = 0;
      bool                 ok = true;

      bool skip(std::size_t n)
      {
         if (!ok || n > size - pos)
            return ok = false;
         pos += n;
         return true;
      }

      uint32_t u32()
      {
         uint32_t v = 0;
         if (ok && size - pos >= 4)
            std::memcpy(&v, data + pos, 4);
         skip(4);
         return ok ? v : 0;
      }

      int32_t i32() { return int32_t(u32()); }

      bool boolean()
      {
         auto const v = u32();
         if (v > 1)
            ok = false;
         return v == 1;
      }

      // Returns the bytes in place; `n` their length.
      unsigned char const* bytes(std::size_t& n)
      {
         n = u32();
         auto const* p = data + pos;
         return skip(align4(n)) ? p : nullptr;
      }

      // SkWriteBuffer::writeString: the length, then the characters and a
      // terminating zero, padded.
      void skip_string()
      {
         auto const n = u32();
         skip(align4(std::size_t(n) + 1));
      }
   };

   NSString* as_source(unsigned char const* p, std::size_t n)
   {
      return [[NSString alloc] initWithBytes : p length : n encoding : NSUTF8StringEncoding];
   }

   // GrMtlPipelineStateBuilder's read_pipeline_data. `labelled`: Skia built
   // with SK_ENABLE_MTL_DEBUG_INFO, which puts a description first.
   MTLRenderPipelineDescriptor* read_pipeline(reader r, bool labelled)
   {
      if (labelled)
         r.skip_string();

      auto pipeline = [MTLRenderPipelineDescriptor new];
      auto vertices = [MTLVertexDescriptor new];
      auto const vertex_binding = r.u32();
      auto const instance_binding = r.u32();
      if (vertex_binding > 30 || instance_binding > 30)
         return nil;

      NSUInteger attribute = 0;
      auto const read_attributes = [&](MTLVertexStepFunction step, uint32_t binding) {
         auto const count = r.i32();
         if (!r.ok || count < 0 || attribute + NSUInteger(count) > 31)
            return false;
         for (int i = 0; i < count; ++i)
         {
            MTLVertexAttributeDescriptor* a = vertices.attributes[attribute++];
            a.format = MTLVertexFormat(r.i32());
            a.offset = r.u32();
            a.bufferIndex = r.u32();
         }
         if (count != 0)
         {
            MTLVertexBufferLayoutDescriptor* layout = vertices.layouts[binding];
            layout.stepFunction = step;
            layout.stepRate = 1;
            layout.stride = r.u32();
         }
         return r.ok;
      };
      if (!read_attributes(MTLVertexStepFunctionPerVertex, vertex_binding) ||
          !read_attributes(MTLVertexStepFunctionPerInstance, instance_binding))
         return nil;
      pipeline.vertexDescriptor = vertices;

      auto colour = [MTLRenderPipelineColorAttachmentDescriptor new];
      colour.pixelFormat = MTLPixelFormat(r.i32());
      colour.blendingEnabled = r.boolean();
      if (colour.blendingEnabled)
      {
         colour.sourceRGBBlendFactor = MTLBlendFactor(r.i32());
         colour.destinationRGBBlendFactor = MTLBlendFactor(r.i32());
         colour.rgbBlendOperation = MTLBlendOperation(r.i32());
         colour.sourceAlphaBlendFactor = MTLBlendFactor(r.i32());
         colour.destinationAlphaBlendFactor = MTLBlendFactor(r.i32());
         colour.alphaBlendOperation = MTLBlendOperation(r.i32());
      }
      colour.writeMask = r.boolean() ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
      pipeline.colorAttachments[0] = colour;
      pipeline.stencilAttachmentPixelFormat = MTLPixelFormat(r.i32());

      // All of it, and nothing after: otherwise this was the other layout.
      return r.ok && r.pos == r.size ? pipeline : nil;
   }

   // What Skia compiles with (GrCompileMtlShaderLibrary): Metal's cache is
   // keyed by the source and these, so they must match.
   MTLCompileOptions* compile_options()
   {
      auto options = [MTLCompileOptions new];
      options.languageVersion = MTLLanguageVersion2_0;
#if (TARGET_OS_IOS && __IPHONE_OS_VERSION_MIN_REQUIRED >= 180000)
      options.mathMode = MTLMathModeFast;
#else
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wdeprecated-declarations"
      options.fastMathEnabled = YES;
# pragma clang diagnostic pop
#endif
      return options;
   }

   // One entry, as GrMtlPipelineStateBuilder::PrecompileShaders compiles it,
   // the render pipeline state too. False if it did not parse or compile.
   bool warm(id<MTLDevice> device, MTLCompileOptions* options, std::string const& entry)
   {
      reader r{reinterpret_cast<unsigned char const*>(entry.data()), entry.size()};
      if (r.i32() != k_cache_version || r.u32() != k_msl_tag)
         return false;

      NSString* source[2] = {};
      for (auto& s : source)
      {
         std::size_t n = 0;
         auto const* p = r.bytes(n);
         if (!p)
            return false;
         s = as_source(p, n);
         r.skip(align4(k_interface_size));
      }
      if (!r.ok || !source[0] || !source[1])
         return false;

      MTLRenderPipelineDescriptor* pipeline = nil;
      if (r.boolean())
      {
         if (r.boolean())
            r.skip(4 * 4); // the four program settings
         auto const names = r.i32();
         for (int i = 0; r.ok && i < names; ++i)
         {
            std::size_t n = 0;
            r.bytes(n);
         }
         r.boolean(); // secondary colour output
         if (r.ok && r.pos < r.size)
         {
            std::size_t n = 0;
            if (auto const* p = r.bytes(n))
            {
               pipeline = read_pipeline(reader{p, n}, false);
               if (!pipeline)
                  pipeline = read_pipeline(reader{p, n}, true);
            }
         }
      }

      NSError* error = nil;
      id<MTLLibrary> vertex = [device newLibraryWithSource : source[0] options : options error : &error];
      id<MTLLibrary> fragment = [device newLibraryWithSource : source[1] options : options error : &error];
      if (!vertex || !fragment)
         return false;
      if (!pipeline)
         return true; // the libraries alone: the bigger part of the time

      pipeline.vertexFunction = [vertex newFunctionWithName : @"vertexMain"];
      pipeline.fragmentFunction = [fragment newFunctionWithName : @"fragmentMain"];
      if (!pipeline.vertexFunction || !pipeline.fragmentFunction)
         return false;
      return [device newRenderPipelineStateWithDescriptor : pipeline error : &error] != nil;
   }
}

struct shader_warmup::state
{
   id<MTLDevice>              device;
   std::vector<std::string>   entries;
   std::atomic<std::size_t>   next{0};
   std::atomic<bool>          stop{false};
   std::atomic<int>           failed{0};
   double                     started = CACurrentMediaTime();

   mutable std::mutex         mutex;
   std::size_t                finished = 0;
   double                     last = 0;

   void run()
   {
      auto* options = compile_options();
      for (;;)
      {
         auto const i = next.fetch_add(1);
         if (i >= entries.size() || stop.load())
            return;
         bool ok;
         @autoreleasepool
         {
            ok = warm(device, options, entries[i]);
         }
         if (!ok)
            ++failed;

         std::lock_guard<std::mutex> lock(mutex);
         ++finished;
         last = CACurrentMediaTime();
      }
   }
};

shader_warmup::shader_warmup(id<MTLDevice> device, std::vector<std::string> entries)
   : _state(std::make_shared<state>())
{
   _state->device = device;
   _state->entries = std::move(entries);
   _state->last = _state->started;
   if (_state->entries.empty())
      return;

   // A thread a core (two on an iPad mini 2). There, with Metal's cache
   // empty and the amp running, the 235 pipelines of a typical cache take
   // 25 to 30 s; the window is drawn on the CPU meanwhile.
   auto const threads = std::max<NSUInteger>(1, std::min<NSUInteger>(
      NSProcessInfo.processInfo.activeProcessorCount, _state->entries.size()));
   auto queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
   for (NSUInteger t = 0; t < threads; ++t)
   {
      auto s = _state;
      dispatch_async(queue, ^{ s->run(); });
   }
}

shader_warmup::~shader_warmup()
{
   _state->stop = true;
}

bool shader_warmup::finished() const
{
   std::lock_guard<std::mutex> lock(_state->mutex);
   return _state->finished == _state->entries.size();
}

std::size_t shader_warmup::size() const
{
   return _state->entries.size();
}

int shader_warmup::failed() const
{
   return _state->failed.load();
}

double shader_warmup::idle_seconds() const
{
   std::lock_guard<std::mutex> lock(_state->mutex);
   return CACurrentMediaTime() - _state->last;
}

double shader_warmup::elapsed_ms() const
{
   std::lock_guard<std::mutex> lock(_state->mutex);
   return (_state->last - _state->started) * 1000;
}
}

#endif // ARTIST_SKIA
