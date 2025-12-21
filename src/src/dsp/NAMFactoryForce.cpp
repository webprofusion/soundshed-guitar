// NAMFactoryForce.cpp
// This file forces the linker to include the NAM DSP factory registration objects.
// Without this, the static factory::Helper objects in wavenet.cpp, lstm.cpp, and
// convnet.cpp may be discarded by the linker since they are not directly referenced.

#include "NAM/wavenet.h"
#include "NAM/lstm.h"
#include "NAM/convnet.h"

namespace nam
{
namespace factory
{
// Force references to symbols that trigger static initialization
// The actual initialization happens via static Helper objects in each .cpp file
namespace
{
// These volatile pointers prevent the compiler from optimizing away the references.
// We take the address of the Factory functions which ensures the translation units
// containing the static Helper registrations are linked.
volatile auto wavenet_factory_ptr = &nam::wavenet::Factory;
volatile auto lstm_factory_ptr = &nam::lstm::Factory;
volatile auto convnet_factory_ptr = &nam::convnet::Factory;

// Alternative: Call this from any function that gets compiled to ensure linkage
void ForceFactoryRegistration()
{
  // Touch the factory functions to prevent dead-stripping
  (void)wavenet_factory_ptr;
  (void)lstm_factory_ptr;
  (void)convnet_factory_ptr;
}
} // namespace
} // namespace factory
} // namespace nam
