# VST3 wrapper target placeholder. The actual implementation will be provided
# during the customization phase once plugin sources are in place.

add_library(NAMGuitarFX_VST3 MODULE
  NAMGuitarFX_VST3.cpp
  ../../NAMGuitarPlugin.cpp
)

target_compile_definitions(NAMGuitarFX_VST3 PRIVATE VST3_API)

target_include_directories(NAMGuitarFX_VST3 PRIVATE
  ${iPlug2_SOURCE_DIR}/Dependencies/IPlug/VST3_SDK
  ${iPlug2_SOURCE_DIR}/IPlug/VST3
)

target_link_libraries(NAMGuitarFX_VST3 PRIVATE NAMGuitarFXCore)

set_target_properties(NAMGuitarFX_VST3 PROPERTIES
  OUTPUT_NAME NAMGuitarFX
  FOLDER NAMGuitarFX/Formats
)
