#pragma once

#include "GuitarFXConfig.h"

#define PLUG_CLASS_NAME guitarfx::GuitarFXPlugin
#define PLUG_HOST_RESIZE 1

#define AUV2_ENTRY GuitarFX_Entry
#define AUV2_ENTRY_STR "GuitarFX_Entry"
#define AUV2_FACTORY GuitarFX_Factory
#define AUV2_VIEW_CLASS GuitarFX_View
#define AUV2_VIEW_CLASS_STR "GuitarFX_View"

#define AAX_TYPE_IDS 'GtFx'
#define AAX_TYPE_IDS_AUDIOSUITE 'GtFa'
#define AAX_PLUG_MFR_STR "GtFx"
#define AAX_PLUG_NAME_STR PLUG_NAME "\nGFX"
#define AAX_PLUG_CATEGORY_STR "Effect"
#define AAX_DOES_AUDIOSUITE 0

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define IGRAPHICS_NANOVG 1
#define IGRAPHICS_GL2 1
#define IGRAPHICS_WEBVIEW 1
