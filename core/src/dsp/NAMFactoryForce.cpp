// NAMFactoryForce.cpp
// Ensure NAM architecture parsers from separate translation units are linked.

#include "NAM/convnet.h"
#include "NAM/container.h"
#include "NAM/dsp.h"
#include "NAM/lstm.h"
#include "NAM/wavenet/model.h"

namespace nam
{
namespace factory
{
static volatile auto convnet_create_config = &nam::convnet::create_config;
static volatile auto container_create_config = &nam::container::create_config;
static volatile auto linear_create_config = &nam::linear::create_config;
static volatile auto lstm_create_config = &nam::lstm::create_config;
static volatile auto wavenet_create_config = &nam::wavenet::create_config;

void ForceFactoryRegistration()
{
  (void)convnet_create_config;
  (void)container_create_config;
  (void)linear_create_config;
  (void)lstm_create_config;
  (void)wavenet_create_config;
}

} // namespace factory
} // namespace nam
