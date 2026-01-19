/**
 * VST3 Debug Host
 * A minimal VST3 host for diagnosing plugin initialization issues.
 * 
 * This tool loads a VST3 plugin and reports detailed information about
 * each step of the initialization process, making it easier to identify
 * where crashes or errors occur.
 */

#include <Windows.h>
#include <objbase.h>
#include <iostream>
#include <string>
#include <iomanip>
#include <sstream>
#include <cmath>

// VST3 SDK headers
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/gui/iplugview.h"

using namespace Steinberg;

// Function pointer types for VST3 module entry points
typedef bool (PLUGIN_API *InitModuleFunc)();
typedef bool (PLUGIN_API *ExitModuleFunc)();
typedef IPluginFactory* (PLUGIN_API *GetPluginFactoryFunc)();

// Helper to convert TUID to string
std::string tuidToString(const TUID& tuid) {
    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        ss << std::setw(2) << (int)(unsigned char)tuid[i];
        if (i == 3 || i == 5 || i == 7 || i == 9) ss << "-";
    }
    return ss.str();
}

// Helper to convert FIDString to readable string
std::string fidToString(FIDString str) {
    if (!str) return "(null)";
    return std::string(str);
}

// Simple host application implementation
class DebugHostApplication : public Vst::IHostApplication {
public:
    tresult PLUGIN_API getName(Vst::String128 name) override {
        const char16_t hostName[] = u"VST3 Debug Host";
        memcpy(name, hostName, sizeof(hostName));
        return kResultOk;
    }
    
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        *obj = nullptr;
        return kNotImplemented;
    }
    
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, Vst::IHostApplication::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    
    uint32 PLUGIN_API addRef() override { return ++refCount; }
    uint32 PLUGIN_API release() override { 
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return refCount;
    }
    
private:
    uint32 refCount = 1;
};

class VST3DebugHost {
public:
    VST3DebugHost() : hModule(nullptr), factory(nullptr), component(nullptr), 
                      processor(nullptr), controller(nullptr) {}
    
    ~VST3DebugHost() {
        cleanup();
    }
    
    bool run(const std::string& pluginPath) {
        std::cout << "========================================\n";
        std::cout << "VST3 Debug Host - Plugin Diagnostics\n";
        std::cout << "========================================\n\n";
        
        std::cout << "Plugin path: " << pluginPath << "\n\n";
        
        // Step 1: Load DLL
        if (!loadModule(pluginPath)) return false;
        
        // Step 2: Initialize module
        if (!initializeModule()) return false;
        
        // Step 3: Get plugin factory
        if (!getFactory()) return false;
        
        // Step 4: Enumerate factory info
        enumerateFactoryInfo();
        
        // Step 5: Create component
        if (!createComponent()) return false;
        
        // Step 6: Initialize component
        if (!initializeComponent()) return false;
        
        // Step 7: Query processor interface
        queryProcessor();
        
        // Step 8: Get/create edit controller
        queryEditController();
        
        // Step 9: Try to create editor view (this often crashes)
        tryCreateEditorView();
        
        // Step 10: Try to activate
        tryActivate();
        
        std::cout << "\n========================================\n";
        std::cout << "Plugin loaded successfully!\n";
        std::cout << "========================================\n";
        
        return true;
    }
    
private:
    HMODULE hModule;
    IPluginFactory* factory;
    Vst::IComponent* component;
    Vst::IAudioProcessor* processor;
    Vst::IEditController* controller;
    DebugHostApplication hostApp;
    
    void step(const char* name) {
        std::cout << "[STEP] " << name << "... ";
    }
    
    void ok() {
        std::cout << "OK\n";
    }
    
    void fail(const char* reason) {
        std::cout << "FAILED: " << reason << "\n";
    }
    
    void info(const char* msg) {
        std::cout << "  [INFO] " << msg << "\n";
    }
    
    bool loadModule(const std::string& path) {
        step("Loading DLL");
        
        __try {
            hModule = LoadLibraryA(path.c_str());
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during LoadLibrary");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return false;
        }
        
        if (!hModule) {
            DWORD error = GetLastError();
            fail("LoadLibrary failed");
            std::cout << "  Error code: " << error << "\n";
            
            // Try to get more info
            char* msgBuf = nullptr;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                          nullptr, error, 0, (LPSTR)&msgBuf, 0, nullptr);
            if (msgBuf) {
                std::cout << "  Message: " << msgBuf << "\n";
                LocalFree(msgBuf);
            }
            return false;
        }
        
        ok();
        return true;
    }
    
    bool initializeModule() {
        step("Getting InitDll entry point");
        
        auto initFunc = (InitModuleFunc)GetProcAddress(hModule, "InitDll");
        if (!initFunc) {
            info("InitDll not found (optional, continuing)");
            return true;
        }
        ok();
        
        step("Calling InitDll");
        __try {
            bool result = initFunc();
            if (!result) {
                fail("InitDll returned false");
                return false;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during InitDll");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return false;
        }
        ok();
        
        return true;
    }
    
    bool getFactory() {
        step("Getting GetPluginFactory entry point");
        
        auto getFactoryFunc = (GetPluginFactoryFunc)GetProcAddress(hModule, "GetPluginFactory");
        if (!getFactoryFunc) {
            fail("GetPluginFactory not found");
            return false;
        }
        ok();
        
        step("Calling GetPluginFactory");
        __try {
            factory = getFactoryFunc();
            if (!factory) {
                fail("GetPluginFactory returned null");
                return false;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during GetPluginFactory");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return false;
        }
        ok();
        
        return true;
    }
    
    void enumerateFactoryInfo() {
        std::cout << "\n--- Factory Information ---\n";
        
        PFactoryInfo factoryInfo;
        if (factory->getFactoryInfo(&factoryInfo) == kResultOk) {
            std::cout << "  Vendor: " << factoryInfo.vendor << "\n";
            std::cout << "  URL: " << factoryInfo.url << "\n";
            std::cout << "  Email: " << factoryInfo.email << "\n";
            std::cout << "  Flags: 0x" << std::hex << factoryInfo.flags << std::dec << "\n";
        }
        
        int32 classCount = factory->countClasses();
        std::cout << "  Class count: " << classCount << "\n\n";
        
        for (int32 i = 0; i < classCount; ++i) {
            PClassInfo classInfo;
            if (factory->getClassInfo(i, &classInfo) == kResultOk) {
                std::cout << "  Class " << i << ":\n";
                std::cout << "    CID: " << tuidToString(classInfo.cid) << "\n";
                std::cout << "    Name: " << classInfo.name << "\n";
                std::cout << "    Category: " << classInfo.category << "\n";
                std::cout << "    Cardinality: " << classInfo.cardinality << "\n";
            }
        }
        
        // Try IPluginFactory2
        IPluginFactory2* factory2 = nullptr;
        if (factory->queryInterface(IPluginFactory2::iid, (void**)&factory2) == kResultOk && factory2) {
            std::cout << "\n  Extended class info (IPluginFactory2):\n";
            for (int32 i = 0; i < classCount; ++i) {
                PClassInfo2 classInfo2;
                if (factory2->getClassInfo2(i, &classInfo2) == kResultOk) {
                    std::cout << "  Class " << i << " extended:\n";
                    std::cout << "    SubCategories: " << classInfo2.subCategories << "\n";
                    std::cout << "    Vendor: " << classInfo2.vendor << "\n";
                    std::cout << "    Version: " << classInfo2.version << "\n";
                    std::cout << "    SDK Version: " << classInfo2.sdkVersion << "\n";
                }
            }
            factory2->release();
        }
        
        std::cout << "----------------------------\n\n";
    }
    
    bool createComponent() {
        step("Finding audio processor class");
        
        TUID processorCID = {0};
        bool found = false;
        
        int32 classCount = factory->countClasses();
        for (int32 i = 0; i < classCount; ++i) {
            PClassInfo classInfo;
            if (factory->getClassInfo(i, &classInfo) == kResultOk) {
                if (strcmp(classInfo.category, kVstAudioEffectClass) == 0) {
                    memcpy(processorCID, classInfo.cid, sizeof(TUID));
                    found = true;
                    info(classInfo.name);
                    break;
                }
            }
        }
        
        if (!found) {
            fail("No audio processor class found");
            return false;
        }
        ok();
        
        step("Creating component instance");
        __try {
            tresult result = factory->createInstance(processorCID, Vst::IComponent::iid, (void**)&component);
            if (result != kResultOk || !component) {
                fail("createInstance failed");
                std::cout << "  Result: " << result << "\n";
                return false;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during createInstance");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return false;
        }
        ok();
        
        return true;
    }
    
    bool initializeComponent() {
        step("Initializing component");
        __try {
            tresult result = component->initialize(&hostApp);
            if (result != kResultOk) {
                fail("initialize failed");
                std::cout << "  Result: " << result << "\n";
                return false;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during initialize");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return false;
        }
        ok();
        
        // Get bus info
        std::cout << "\n--- Bus Configuration ---\n";
        int32 numInputs = component->getBusCount(Vst::kAudio, Vst::kInput);
        int32 numOutputs = component->getBusCount(Vst::kAudio, Vst::kOutput);
        std::cout << "  Audio inputs: " << numInputs << "\n";
        std::cout << "  Audio outputs: " << numOutputs << "\n";
        
        for (int32 i = 0; i < numInputs; ++i) {
            Vst::BusInfo busInfo;
            if (component->getBusInfo(Vst::kAudio, Vst::kInput, i, busInfo) == kResultOk) {
                char name[128];
                for (int j = 0; j < 128 && busInfo.name[j]; ++j) name[j] = (char)busInfo.name[j];
                name[127] = 0;
                std::cout << "    Input " << i << ": " << name << " (" << busInfo.channelCount << " ch)\n";
            }
        }
        
        for (int32 i = 0; i < numOutputs; ++i) {
            Vst::BusInfo busInfo;
            if (component->getBusInfo(Vst::kAudio, Vst::kOutput, i, busInfo) == kResultOk) {
                char name[128];
                for (int j = 0; j < 128 && busInfo.name[j]; ++j) name[j] = (char)busInfo.name[j];
                name[127] = 0;
                std::cout << "    Output " << i << ": " << name << " (" << busInfo.channelCount << " ch)\n";
            }
        }
        std::cout << "--------------------------\n\n";
        
        return true;
    }
    
    void queryProcessor() {
        step("Querying IAudioProcessor interface");
        __try {
            tresult result = component->queryInterface(Vst::IAudioProcessor::iid, (void**)&processor);
            if (result != kResultOk || !processor) {
                fail("Component doesn't support IAudioProcessor");
                return;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during queryInterface");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return;
        }
        ok();
        
        // Setup processing
        step("Setting up audio processing");
        __try {
            Vst::ProcessSetup setup;
            setup.processMode = Vst::kRealtime;
            setup.symbolicSampleSize = Vst::kSample32;
            setup.maxSamplesPerBlock = 512;
            setup.sampleRate = 44100.0;
            
            tresult result = processor->setupProcessing(setup);
            if (result != kResultOk) {
                fail("setupProcessing failed");
                std::cout << "  Result: " << result << "\n";
                return;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during setupProcessing");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return;
        }
        ok();
    }
    
    void queryEditController() {
        step("Querying IEditController interface");
        
        // First check if component implements IEditController directly (SingleComponent)
        __try {
            tresult result = component->queryInterface(Vst::IEditController::iid, (void**)&controller);
            if (result == kResultOk && controller) {
                info("Component implements IEditController directly (SingleComponent)");
                ok();
                printControllerInfo();
                return;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during queryInterface for IEditController");
            return;
        }
        
        // Otherwise, get separate controller
        TUID controllerCID;
        tresult result = component->getControllerClassId(controllerCID);
        if (result != kResultOk) {
            info("No separate edit controller");
            return;
        }
        
        std::cout << "\n";
        step("Creating separate edit controller");
        __try {
            result = factory->createInstance(controllerCID, Vst::IEditController::iid, (void**)&controller);
            if (result != kResultOk || !controller) {
                fail("Failed to create edit controller");
                return;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during controller creation");
            return;
        }
        ok();
        
        step("Initializing edit controller");
        __try {
            result = controller->initialize(&hostApp);
            if (result != kResultOk) {
                fail("Controller initialize failed");
                return;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during controller initialize");
            return;
        }
        ok();
        
        printControllerInfo();
    }
    
    void printControllerInfo() {
        if (!controller) return;
        
        std::cout << "\n--- Parameter Information ---\n";
        int32 paramCount = controller->getParameterCount();
        std::cout << "  Parameter count: " << paramCount << "\n";
        
        for (int32 i = 0; i < paramCount && i < 20; ++i) {
            Vst::ParameterInfo paramInfo;
            if (controller->getParameterInfo(i, paramInfo) == kResultOk) {
                char title[128];
                for (int j = 0; j < 128 && paramInfo.title[j]; ++j) title[j] = (char)paramInfo.title[j];
                title[127] = 0;
                std::cout << "    [" << i << "] " << title;
                std::cout << " (id=" << paramInfo.id << ", default=" << paramInfo.defaultNormalizedValue << ")\n";
            }
        }
        if (paramCount > 20) {
            std::cout << "    ... and " << (paramCount - 20) << " more parameters\n";
        }
        std::cout << "-----------------------------\n";
    }
    
    void tryCreateEditorView() {
        if (!controller) return;
        
        step("Creating editor view");
        IPlugView* view = nullptr;
        __try {
            view = controller->createView(Steinberg::Vst::ViewType::kEditor);
            if (!view) {
                info("No editor view available (plugin may not have UI)");
                return;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during createView");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return;
        }
        ok();
        
        step("Checking editor view platform support");
        __try {
            // Check if it supports Windows platform
            if (view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultOk) {
                info("Supports HWND (Windows)");
            } else {
                info("Does not support HWND");
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during isPlatformTypeSupported");
        }
        ok();
        
        step("Getting editor size");
        ViewRect rect = {0, 0, 800, 600};
        __try {
            if (view->getSize(&rect) == kResultOk) {
                std::cout << "OK (" << (rect.right - rect.left) << " x " << (rect.bottom - rect.top) << ")\n";
            } else {
                info("getSize failed, using defaults");
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during getSize");
        }
        
        // Create a hidden window to attach the view
        step("Creating test window");
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "VST3DebugHostWindow";
        RegisterClassA(&wc);
        
        HWND hwnd = CreateWindowA("VST3DebugHostWindow", "VST3 Debug Host",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left + 50, rect.bottom - rect.top + 50,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        
        if (!hwnd) {
            fail("CreateWindow failed");
            view->release();
            return;
        }
        ok();
        
        // Show window before attaching - some WebView2 implementations need this
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        
        // Pump messages briefly before attach to ensure window is fully ready
        MSG msg;
        for (int i = 0; i < 10; i++) {
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            Sleep(10);
        }
        
        step("Attaching view to window (THIS IS WHERE CRASHES OFTEN OCCUR)");
        bool attachSuccess = false;
        __try {
            tresult result = view->attached(hwnd, kPlatformTypeHWND);
            
            // Pump messages during and after attach to allow async WebView2 initialization
            for (int i = 0; i < 50; i++) {
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                Sleep(10);
            }
            
            if (result != kResultOk) {
                fail("view->attached failed");
                std::cout << "  Result: " << result << "\n";
                DestroyWindow(hwnd);
                view->release();
                return;
            }
            attachSuccess = true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during view->attached");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            DestroyWindow(hwnd);
            view->release();
            return;
        }
        
        if (!attachSuccess) {
            return;
        }
        ok();
        
        step("Showing window and processing messages");
        // Window is already shown, just update
        UpdateWindow(hwnd);
        
        // Process messages for longer to let the UI fully initialize and test interactions
        DWORD startTime = GetTickCount();
        const DWORD displayDuration = 15000; // 15 seconds to allow UI to stabilize
        int messageCount = 0;
        
        std::cout << "\n  [INFO] Processing window messages for " << (displayDuration/1000) << " seconds...\n";
        
        __try {
            while (GetTickCount() - startTime < displayDuration) {
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                    messageCount++;
                }
                Sleep(16); // ~60fps update rate
                
                // Progress indicator every second
                DWORD elapsed = GetTickCount() - startTime;
                if (elapsed > 0 && (elapsed % 1000) < 20) {
                    std::cout << "  [PROGRESS] " << (elapsed/1000) << "s - " << messageCount << " messages processed\n";
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            std::cout << "\n";
            fail("Exception during window message processing!");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            std::cout << "  Messages processed before crash: " << messageCount << "\n";
            
            // Try to clean up gracefully
            __try {
                view->removed();
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                std::cout << "  [WARN] Exception during emergency view->removed()\n";
            }
            
            DestroyWindow(hwnd);
            UnregisterClassA("VST3DebugHostWindow", GetModuleHandle(nullptr));
            view->release();
            return;
        }
        
        std::cout << "  [INFO] Message loop completed, " << messageCount << " total messages\n";
        ok();
        
        // Hide window before detaching to reduce chances of accessing invalid UI
        ShowWindow(hwnd, SW_HIDE);
        Sleep(100); // Brief pause for pending messages to complete
        
        step("Detaching view");
        __try {
            view->removed();
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during view->removed");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
        }
        ok();
        
        // Clean up - process any remaining messages first
        while (PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE)) {
            DispatchMessage(&msg);
        }
        
        DestroyWindow(hwnd);
        Sleep(50); // Brief pause before unregistering class
        UnregisterClassA("VST3DebugHostWindow", GetModuleHandle(nullptr));
        
        step("Releasing editor view");
        view->release();
        ok();
    }
    
    void tryActivate() {
        if (!component) return;
        
        step("Activating buses");
        __try {
            // Activate all audio buses
            int32 numInputs = component->getBusCount(Vst::kAudio, Vst::kInput);
            int32 numOutputs = component->getBusCount(Vst::kAudio, Vst::kOutput);
            
            for (int32 i = 0; i < numInputs; ++i) {
                component->activateBus(Vst::kAudio, Vst::kInput, i, true);
            }
            for (int32 i = 0; i < numOutputs; ++i) {
                component->activateBus(Vst::kAudio, Vst::kOutput, i, true);
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during bus activation");
            return;
        }
        ok();
        
        step("Setting component active");
        __try {
            tresult result = component->setActive(true);
            if (result != kResultOk) {
                fail("setActive failed");
                std::cout << "  Result: " << result << "\n";
                return;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            fail("Exception during setActive");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            return;
        }
        ok();
        
        // Run audio processing for a few seconds
        runAudioProcessingTest();
        
        // Deactivate for cleanup
        step("Deactivating component");
        component->setActive(false);
        ok();
    }
    
    void runAudioProcessingTest() {
        if (!processor) {
            info("No processor available, skipping audio test");
            return;
        }
        
        step("Running audio processing test (3 seconds)");
        
        const int32 blockSize = 512;
        const double sampleRate = 44100.0;
        const int numChannels = 2;
        const double testDurationSeconds = 10.0;
        const int totalBlocks = static_cast<int>((sampleRate * testDurationSeconds) / blockSize);
        
        // Allocate audio buffers
        float* inputBufferData[numChannels];
        float* outputBufferData[numChannels];
        
        for (int ch = 0; ch < numChannels; ++ch) {
            inputBufferData[ch] = new float[blockSize];
            outputBufferData[ch] = new float[blockSize];
        }
        
        // Setup process data
        Vst::ProcessData processData;
        processData.processMode = Vst::kRealtime;
        processData.symbolicSampleSize = Vst::kSample32;
        processData.numSamples = blockSize;
        processData.numInputs = 1;
        processData.numOutputs = 1;
        processData.inputParameterChanges = nullptr;
        processData.outputParameterChanges = nullptr;
        processData.inputEvents = nullptr;
        processData.outputEvents = nullptr;
        processData.processContext = nullptr;
        
        Vst::AudioBusBuffers inputBus;
        inputBus.numChannels = numChannels;
        inputBus.silenceFlags = 0;
        inputBus.channelBuffers32 = inputBufferData;
        
        Vst::AudioBusBuffers outputBus;
        outputBus.numChannels = numChannels;
        outputBus.silenceFlags = 0;
        outputBus.channelBuffers32 = outputBufferData;
        
        processData.inputs = &inputBus;
        processData.outputs = &outputBus;
        
        int successfulBlocks = 0;
        int failedBlocks = 0;
        DWORD startTime = GetTickCount();
        
        std::cout << "\n";
        
        __try {
            for (int block = 0; block < totalBlocks; ++block) {
                // Fill input with a simple sine wave test signal
                float phase = static_cast<float>(block * blockSize) / static_cast<float>(sampleRate);
                for (int i = 0; i < blockSize; ++i) {
                    float sample = 0.1f * sinf(2.0f * 3.14159265f * 440.0f * (phase + i / static_cast<float>(sampleRate)));
                    for (int ch = 0; ch < numChannels; ++ch) {
                        inputBufferData[ch][i] = sample;
                        outputBufferData[ch][i] = 0.0f;
                    }
                }
                
                // Process audio
                tresult result = processor->process(processData);
                
                if (result == kResultOk) {
                    successfulBlocks++;
                } else {
                    failedBlocks++;
                    if (failedBlocks == 1) {
                        std::cout << "  [WARN] First process() failure at block " << block << ", result: " << result << "\n";
                    }
                }
                
                // Print progress every second
                if (block > 0 && block % static_cast<int>(sampleRate / blockSize) == 0) {
                    DWORD elapsed = GetTickCount() - startTime;
                    std::cout << "  [PROGRESS] " << (elapsed / 1000) << "s elapsed, " 
                              << successfulBlocks << " blocks processed OK, "
                              << failedBlocks << " failed\n";
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            std::cout << "\n";
            fail("Exception during audio processing!");
            std::cout << "  Exception code: 0x" << std::hex << GetExceptionCode() << std::dec << "\n";
            std::cout << "  Blocks completed before crash: " << successfulBlocks << "\n";
            
            // Cleanup
            for (int ch = 0; ch < numChannels; ++ch) {
                delete[] inputBufferData[ch];
                delete[] outputBufferData[ch];
            }
            return;
        }
        
        DWORD totalTime = GetTickCount() - startTime;
        
        // Cleanup
        for (int ch = 0; ch < numChannels; ++ch) {
            delete[] inputBufferData[ch];
            delete[] outputBufferData[ch];
        }
        
        std::cout << "  [RESULT] Audio processing test completed:\n";
        std::cout << "    Total time: " << totalTime << "ms\n";
        std::cout << "    Successful blocks: " << successfulBlocks << "/" << totalBlocks << "\n";
        std::cout << "    Failed blocks: " << failedBlocks << "\n";
        
        if (failedBlocks == 0) {
            ok();
        } else {
            std::cout << "  [WARN] Some audio blocks failed to process\n";
        }
    }
    
    void cleanup() {
        if (controller) {
            // Don't release if it's the same as component (SingleComponent)
            Vst::IEditController* testController = nullptr;
            if (component && component->queryInterface(Vst::IEditController::iid, (void**)&testController) == kResultOk) {
                testController->release(); // Release the query ref
                // Don't release controller separately - it's the same object
            } else if (controller) {
                controller->terminate();
                controller->release();
            }
            controller = nullptr;
        }
        
        if (processor) {
            processor->release();
            processor = nullptr;
        }
        
        if (component) {
            component->terminate();
            component->release();
            component = nullptr;
        }
        
        if (factory) {
            factory->release();
            factory = nullptr;
        }
        
        if (hModule) {
            auto exitFunc = (ExitModuleFunc)GetProcAddress(hModule, "ExitDll");
            if (exitFunc) {
                exitFunc();
            }
            FreeLibrary(hModule);
            hModule = nullptr;
        }
    }
};

int runHost(const char* pluginPath) {
    VST3DebugHost host;
    if (!host.run(pluginPath)) {
        std::cout << "\n*** Plugin loading failed! ***\n";
        std::cout << "Check the error messages above for details.\n";
        return 1;
    }
    return 0;
}

int mainImpl(int argc, char* argv[]) {
    std::cout << "\n";
    
    const char* pluginPath;
    const char* defaultPath = "C:\\Work\\GIT\\soundshed-guitar\\src\\build\\SoundshedGuitar.vst3\\Contents\\x86_64-win\\SoundshedGuitar.vst3";

    if (argc < 2) {
        // Default to our plugin
        pluginPath = defaultPath;
        std::cout << "No plugin path specified, using default:\n";
        std::cout << "  " << pluginPath << "\n\n";
    } else {
        pluginPath = argv[1];
    }
    
    return runHost(pluginPath);
}

int main(int argc, char* argv[]) {
    // Initialize COM for WebView2 support
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        printf("Failed to initialize COM: 0x%08X\n", hr);
        return 1;
    }
    
    int result = 0;
    
    // Set up SEH to catch any unhandled exceptions
    __try {
        result = mainImpl(argc, argv);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("\n*** FATAL EXCEPTION ***\n");
        printf("Exception code: 0x%08X\n", GetExceptionCode());
        printf("The plugin caused an unhandled exception.\n");
        result = 1;
    }
    
    printf("\nPress Enter to exit...");
    getchar();
    
    CoUninitialize();
    
    return result;
}
