# Platform specific plugin targets are defined here. Each format wraps the
# shared GuitarFX static library and links against the corresponding iPlug2
# implementations. The actual format-specific sources will be added during the
# customization step.

if(GUITARFX_ENABLE_VST3)
  add_subdirectory(vst3)
endif()

if(GUITARFX_ENABLE_APP)
  add_subdirectory(app)
endif()

if(APPLE AND GUITARFX_ENABLE_AU)
  add_subdirectory(au)
endif()

if(GUITARFX_ENABLE_AAX)
  add_subdirectory(aax)
endif()
