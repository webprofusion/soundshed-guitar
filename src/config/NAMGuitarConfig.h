#pragma once

#define NAM_BRAND_COMPANY_STR "Soundshed"
#define NAM_BRAND_PRODUCT_STR "Neuron FX"
#define NAM_BRAND_COPYRIGHT_YEAR_STR "2025"
#define NAM_BRAND_DOMAIN_STR "com.soundshed.neuronfx"
#define NAM_BRAND_DISPLAY_STR NAM_BRAND_COMPANY_STR " " NAM_BRAND_PRODUCT_STR

#define PLUG_NAME NAM_BRAND_PRODUCT_STR
#define PLUG_MFR NAM_BRAND_COMPANY_STR
#define PLUG_VERSION_HEX 0x00010000
#define PLUG_VERSION_STR "0.1.0"
#define PLUG_UNIQUE_ID 'SNFX'
#define PLUG_MFR_ID 'Wbp'
#define PLUG_URL_STR "https://soundshed.com"
#define PLUG_EMAIL_STR "support@soundshed.com"
#define PLUG_COPYRIGHT_STR "Copyright " NAM_BRAND_COPYRIGHT_YEAR_STR " " NAM_BRAND_COMPANY_STR

#define PLUG_CHANNEL_IO "2-2"

#define PLUG_LATENCY 0
#define PLUG_TYPE 0
#define PLUG_DOES_MIDI_IN 0
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 1280
#define PLUG_HEIGHT 820
#define PLUG_MIN_WIDTH 640
#define PLUG_MAX_WIDTH 3840
#define PLUG_MIN_HEIGHT 480
#define PLUG_MAX_HEIGHT 2160
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0

#define BUNDLE_NAME NAM_BRAND_PRODUCT_STR
#define BUNDLE_MFR NAM_BRAND_COMPANY_STR
#define BUNDLE_DOMAIN NAM_BRAND_DOMAIN_STR

#define SHARED_RESOURCES_SUBPATH NAM_BRAND_PRODUCT_STR

#define PLUG_BG_COLOR 0x202020FF

#define VST3_SUBCATEGORY "Fx"

#ifdef __cplusplus
namespace namguitar::branding
{
inline constexpr const char* kCompany = NAM_BRAND_COMPANY_STR;
inline constexpr const char* kProduct = NAM_BRAND_PRODUCT_STR;
inline constexpr const char* kDisplay = NAM_BRAND_DISPLAY_STR;
inline constexpr const char* kDomain = NAM_BRAND_DOMAIN_STR;
}
#endif
