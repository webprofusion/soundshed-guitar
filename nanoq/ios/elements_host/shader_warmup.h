/*=============================================================================
   Skia's Metal shaders, compiled again off the main thread at launch.

   Skia keeps each pipeline's Metal source between launches (shader_cache, in
   base_view.mm), but Metal's own cache of what it compiled from that source
   lives in the app's Caches folder, which iOS empties when it likes. With it
   gone, the first draw compiles every pipeline it meets on the main thread,
   inside Skia's flush: 7 to 12 s of an empty window on an iPad mini 2.

   This compiles the stored pipelines, libraries and render pipeline states
   both, on background threads in the order earlier launches met them, with
   the options Skia compiles them with, so that Metal's cache answers Skia's
   own compile when the GPU draws. Until all are compiled the view draws on
   the CPU (see base_view.mm's render_cpu).

   Distributed under the MIT License (https://opensource.org/licenses/MIT),
   as Elements is.
=============================================================================*/
#pragma once

#import <Metal/Metal.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace nanoq::ios
{
   class shader_warmup
   {
   public:

      // `entries`: Skia's persistent cache data, one per pipeline, in the
      // order first met. Starts compiling at once.
      shader_warmup(id<MTLDevice> device, std::vector<std::string> entries);

      // Stops handing out work; a compile already running finishes on its
      // thread, which holds what it needs.
      ~shader_warmup();

      shader_warmup(shader_warmup const&) = delete;
      shader_warmup& operator=(shader_warmup const&) = delete;

      // Whether every entry is compiled (or failed).
      bool finished() const;

      std::size_t size() const;
      int failed() const;

      // From construction to the last entry compiled, so far.
      double elapsed_ms() const;

      // Since the last entry was compiled (or construction).
      double idle_seconds() const;

   private:

      struct state;
      std::shared_ptr<state> _state;
   };
}
