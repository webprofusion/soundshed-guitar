# Audio Unit wrapper placeholder. macOS specific sources will be introduced
# during customization. This target is skipped automatically on non-Apple
# platforms.

add_library(GuitarFX_AU MODULE GuitarFX_AU.cpp)

target_link_libraries(GuitarFX_AU PRIVATE GuitarFXCore)

set_target_properties(GuitarFX_AU PROPERTIES
  OUTPUT_NAME GuitarFX
  FOLDER GuitarFX/Formats
)
