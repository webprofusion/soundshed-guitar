# AAX wrapper placeholder. Actual SDK integration will be added later; this
# target is currently a stub for scaffolding purposes.

add_library(GuitarFX_AAX MODULE GuitarFX_AAX.cpp)

target_link_libraries(GuitarFX_AAX PRIVATE GuitarFXCore)

set_target_properties(GuitarFX_AAX PROPERTIES
  OUTPUT_NAME GuitarFX
  FOLDER GuitarFX/Formats
)
