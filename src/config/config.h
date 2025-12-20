#pragma once

#include "NAMGuitarConfig.h"

#define PLUG_CLASS_NAME namguitar::NAMGuitarPlugin
#define PLUG_HOST_RESIZE 1

#define AUV2_ENTRY NAMGuitarFX_Entry
#define AUV2_ENTRY_STR "NAMGuitarFX_Entry"
#define AUV2_FACTORY NAMGuitarFX_Factory
#define AUV2_VIEW_CLASS NAMGuitarFX_View
#define AUV2_VIEW_CLASS_STR "NAMGuitarFX_View"

#define AAX_TYPE_IDS 'NmFx'
#define AAX_TYPE_IDS_AUDIOSUITE 'NmFa'
#define AAX_PLUG_MFR_STR "NrnG"
#define AAX_PLUG_NAME_STR PLUG_NAME "\nNAMG"
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
