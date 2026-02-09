#include "GuitarFXPlugin.h"
#include "IPlug_include_in_plug_src.h"

iplug::Plugin *MakePlug(const iplug::InstanceInfo &info)
{
  return new guitarfx::GuitarFXPlugin(info);
}
