// NAMFactoryForce.cpp
// This file forces the linker to include the NAM DSP factory registration objects.
// Without this, the static factory::Helper objects in wavenet.cpp, lstm.cpp, and
// convnet.cpp may be discarded by the linker since they are not directly referenced.

// Include cassert before NAM headers - upstream NAM activations.h uses assert without including it
#include <cassert>

#include "NAM/wavenet.h"
#include "NAM/lstm.h"
#include "NAM/convnet.h"
#include "NAM/dsp.h"

namespace nam
{
  namespace factory
  {
    // Force references to symbols that trigger static initialization
    // The actual initialization happens via static Helper objects in each .cpp file

    // These volatile pointers prevent the compiler from optimizing away the references.
    // We take the address of the Factory functions which ensures the translation units
    // containing the static Helper registrations are linked.
    static volatile auto wavenet_factory_ptr = &nam::wavenet::Factory;
    static volatile auto lstm_factory_ptr = &nam::lstm::Factory;
    static volatile auto convnet_factory_ptr = &nam::convnet::Factory;

    // This function MUST be called during initialization to force linkage of the factory objects.
    // Even though the function appears to do nothing, the act of calling it ensures the
    // static variables above are not optimized away, which in turn forces the linker to
    // include the translation units containing the static Helper registrations.
    void ForceFactoryRegistration()
    {
      // Touch the factory functions to prevent dead-stripping
      // The volatile keyword prevents the compiler from optimizing these away
      (void)wavenet_factory_ptr;
      (void)lstm_factory_ptr;
      (void)convnet_factory_ptr;
    }

  } // namespace factory
} // namespace nam
