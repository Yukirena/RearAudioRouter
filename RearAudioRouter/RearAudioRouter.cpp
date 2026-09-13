// RearAudioRouter.cpp : 애플리케이션에 대한 진입점을 정의합니다.
//

#include "framework.h"
#include "RearAudioRouter.h"
#include <commctrl.h>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <string>
#include <vector>

#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <bit>

#include <thread>
#include <atomic>
#include <algorithm>

#include <shellapi.h>
#include <shlobj.h>

#include <avrt.h>


#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")

#pragma comment(lib, "Shell32.lib")

#pragma comment(lib, "Avrt.lib")

#define IDC_COMBO_INPUT     1001
#define IDC_COMBO_OUTPUT    1002
#define IDC_RADIO_REAR      1003
#define IDC_BUTTON_START    1004

#define MAX_LOADSTRING 100

#define WM_AUDIO_STOPPED (WM_APP + 1)

#define WM_TRAYICON (WM_APP + 2)

#define ID_TRAY_OPEN       2001
#define ID_TRAY_STARTSTOP  2002
#define ID_TRAY_EXIT       2003

#define IDC_CHECK_AUTOSTART 1005

// 전역 변수:
HINSTANCE hInst;                                // 현재 인스턴스입니다.
WCHAR szTitle[MAX_LOADSTRING];                  // 제목 표시줄 텍스트입니다.
WCHAR szWindowClass[MAX_LOADSTRING];            // 기본 창 클래스 이름입니다.

HWND g_hComboInput = nullptr;
HWND g_hComboOutput = nullptr;
HWND g_hRadioRear = nullptr;
HWND g_hButtonStart = nullptr;

std::vector<std::wstring> g_inputDeviceIds;
std::vector<std::wstring> g_outputDeviceIds;

std::thread g_audioThread;
std::atomic<bool> g_audioRunning = false;

NOTIFYICONDATA g_nid = {};

std::wstring g_configPath;

HANDLE g_audioStopEvent = nullptr;

HWND g_hCheckAutoStart = nullptr;
bool g_startedFromAutoStart = false;

// 이 코드 모듈에 포함된 함수의 선언을 전달합니다:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

void SetControlsRunning(bool running)
{
    EnableWindow(g_hComboInput, !running);
    EnableWindow(g_hComboOutput, !running);
    EnableWindow(g_hRadioRear, !running);

    SetWindowText(
        g_hButtonStart,
        running ? L"Stop" : L"Start"
    );
}

int GetChannelIndex(DWORD channelMask, DWORD speaker)
{
    if ((channelMask & speaker) == 0)
        return -1;

    int index = 0;

    for (DWORD bit = 1; bit < speaker; bit <<= 1)
    {
        if (channelMask & bit)
            ++index;
    }

    return index;
}

bool ValidateOutputDevice(HWND hWnd)
{
    int selectedIndex = (int)SendMessage(
        g_hComboOutput,
        CB_GETCURSEL,
        0,
        0
    );

    if (selectedIndex == CB_ERR ||
        selectedIndex < 0 ||
        selectedIndex >= (int)g_outputDeviceIds.size())
    {
        MessageBox(
            hWnd,
            L"출력 장치를 선택해 주세요.",
            L"RearAudioRouter",
            MB_OK | MB_ICONWARNING
        );

        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator)
    );

    if (FAILED(hr))
    {
        MessageBox(
            hWnd,
            L"오디오 장치 열거기를 생성하지 못했습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    IMMDevice* device = nullptr;

    hr = enumerator->GetDevice(
        g_outputDeviceIds[selectedIndex].c_str(),
        &device
    );

    enumerator->Release();

    if (FAILED(hr))
    {
        MessageBox(
            hWnd,
            L"선택한 출력 장치를 열 수 없습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    IAudioClient* audioClient = nullptr;

    hr = device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(&audioClient)
    );

    device->Release();

    if (FAILED(hr))
    {
        MessageBox(
            hWnd,
            L"WASAPI 오디오 클라이언트를 생성하지 못했습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;

    hr = audioClient->GetMixFormat(&mixFormat);

    if (FAILED(hr))
    {
        audioClient->Release();

        MessageBox(
            hWnd,
            L"출력 장치의 오디오 형식을 가져오지 못했습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    WORD channelCount = mixFormat->nChannels;

    if (channelCount < 4)
    {
        wchar_t message[256];

        swprintf_s(
            message,
            L"이 장치는 현재 %u채널로 설정되어 있습니다.\n\n"
            L"RearAudioRouter는 4채널 이상의 출력 장치가 필요합니다.",
            channelCount
        );

        CoTaskMemFree(mixFormat);
        audioClient->Release();

        MessageBox(
            hWnd,
            message,
            L"RearAudioRouter",
            MB_OK | MB_ICONWARNING
        );

        return false;
    }

    if (mixFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        mixFormat->cbSize < 22)
    {
        CoTaskMemFree(mixFormat);
        audioClient->Release();

        MessageBox(
            hWnd,
            L"이 장치에서 멀티채널 스피커 위치 정보를 가져올 수 없습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONWARNING
        );

        return false;
    }

    WAVEFORMATEXTENSIBLE* extensible =
        reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);

    DWORD channelMask = extensible->dwChannelMask;

    int rearLeftIndex =
        GetChannelIndex(channelMask, SPEAKER_BACK_LEFT);

    int rearRightIndex =
        GetChannelIndex(channelMask, SPEAKER_BACK_RIGHT);

    if (rearLeftIndex < 0 || rearRightIndex < 0)
    {
        wchar_t message[256];

        swprintf_s(
            message,
            L"이 장치는 %u채널이지만 Rear L/R 채널이 없습니다.\n\n"
            L"Channel Mask: 0x%08X",
            channelCount,
            channelMask
        );

        CoTaskMemFree(mixFormat);
        audioClient->Release();

        MessageBox(
            hWnd,
            message,
            L"RearAudioRouter",
            MB_OK | MB_ICONWARNING
        );

        return false;
    }

    //
    // 실제 Shared Mode 초기화 테스트
    //
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        0,
        0,
        mixFormat,
        nullptr
    );

    if (FAILED(hr))
    {
        wchar_t message[256];

        swprintf_s(
            message,
            L"WASAPI Shared Mode로 장치를 열지 못했습니다.\n\n"
            L"HRESULT: 0x%08X",
            (unsigned int)hr
        );

        CoTaskMemFree(mixFormat);
        audioClient->Release();

        MessageBox(
            hWnd,
            message,
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    CoTaskMemFree(mixFormat);
    audioClient->Release();

    return true;
}

bool ValidateInputDevice(HWND hWnd)
{
    int selectedIndex = (int)SendMessage(
        g_hComboInput,
        CB_GETCURSEL,
        0,
        0
    );

    if (selectedIndex == CB_ERR ||
        selectedIndex < 0 ||
        selectedIndex >= (int)g_inputDeviceIds.size())
    {
        MessageBox(
            hWnd,
            L"입력 장치를 선택해 주세요.",
            L"RearAudioRouter",
            MB_OK | MB_ICONWARNING
        );

        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator)
    );

    if (FAILED(hr))
    {
        MessageBox(
            hWnd,
            L"오디오 장치 열거기를 생성하지 못했습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    IMMDevice* device = nullptr;

    hr = enumerator->GetDevice(
        g_inputDeviceIds[selectedIndex].c_str(),
        &device
    );

    enumerator->Release();

    if (FAILED(hr))
    {
        MessageBox(
            hWnd,
            L"선택한 입력 장치를 열 수 없습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    IAudioClient* audioClient = nullptr;

    hr = device->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(&audioClient)
    );

    device->Release();

    if (FAILED(hr))
    {
        MessageBox(
            hWnd,
            L"입력 장치의 WASAPI 오디오 클라이언트를 생성하지 못했습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;

    hr = audioClient->GetMixFormat(&mixFormat);

    if (FAILED(hr))
    {
        audioClient->Release();

        MessageBox(
            hWnd,
            L"입력 장치의 오디오 형식을 가져오지 못했습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    if (mixFormat->nChannels < 2)
    {
        wchar_t message[256];

        swprintf_s(
            message,
            L"입력 장치가 %u채널입니다.\n\n"
            L"RearAudioRouter는 최소 2채널 입력이 필요합니다.",
            mixFormat->nChannels
        );

        CoTaskMemFree(mixFormat);
        audioClient->Release();

        MessageBox(
            hWnd,
            message,
            L"RearAudioRouter",
            MB_OK | MB_ICONWARNING
        );

        return false;
    }

    //
    // Shared Capture 초기화 테스트
    //
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        0,
        0,
        mixFormat,
        nullptr
    );

    if (FAILED(hr))
    {
        wchar_t message[256];

        swprintf_s(
            message,
            L"WASAPI Shared Capture로 입력 장치를 열지 못했습니다.\n\n"
            L"HRESULT: 0x%08X",
            (unsigned int)hr
        );

        CoTaskMemFree(mixFormat);
        audioClient->Release();

        MessageBox(
            hWnd,
            message,
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return false;
    }

    CoTaskMemFree(mixFormat);
    audioClient->Release();

    return true;
}

std::wstring GetDeviceFriendlyName(IMMDevice* device)
{
    IPropertyStore* propertyStore = nullptr;

    HRESULT hr = device->OpenPropertyStore(
        STGM_READ,
        &propertyStore
    );

    if (FAILED(hr))
        return L"(Unknown device)";

    PROPVARIANT value;
    PropVariantInit(&value);

    hr = propertyStore->GetValue(
        PKEY_Device_FriendlyName,
        &value
    );

    std::wstring name = L"(Unknown device)";

    if (SUCCEEDED(hr) &&
        value.vt == VT_LPWSTR &&
        value.pwszVal != nullptr)
    {
        name = value.pwszVal;
    }

    PropVariantClear(&value);
    propertyStore->Release();

    return name;
}

void EnumerateAudioDevices(
    EDataFlow dataFlow,
    HWND comboBox,
    std::vector<std::wstring>& deviceIds)
{
    SendMessage(comboBox, CB_RESETCONTENT, 0, 0);
    deviceIds.clear();

    IMMDeviceEnumerator* enumerator = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator)
    );

    if (FAILED(hr))
        return;

    IMMDeviceCollection* collection = nullptr;

    hr = enumerator->EnumAudioEndpoints(
        dataFlow,
        DEVICE_STATE_ACTIVE,
        &collection
    );

    if (FAILED(hr))
    {
        enumerator->Release();
        return;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* device = nullptr;

        if (FAILED(collection->Item(i, &device)))
            continue;

        LPWSTR deviceId = nullptr;

        if (SUCCEEDED(device->GetId(&deviceId)))
        {
            std::wstring name = GetDeviceFriendlyName(device);

            SendMessage(
                comboBox,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(name.c_str())
            );

            deviceIds.emplace_back(deviceId);

            CoTaskMemFree(deviceId);
        }

        device->Release();
    }

    collection->Release();
    enumerator->Release();

    if (count > 0)
    {
        SendMessage(comboBox, CB_SETCURSEL, 0, 0);
    }
}

void AudioThreadProc(
    HWND hWnd,
    std::wstring inputDeviceId,
    std::wstring outputDeviceId)
{
    HRESULT hr = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    if (FAILED(hr))
    {
        g_audioRunning = false;
        PostMessage(hWnd, WM_AUDIO_STOPPED, 1, 0);
        return;
    }

    bool hadError = false;

    IMMDeviceEnumerator* enumerator = nullptr;

    IMMDevice* inputDevice = nullptr;
    IMMDevice* outputDevice = nullptr;

    IAudioClient* inputClient = nullptr;
    IAudioClient* outputClient = nullptr;

    IAudioCaptureClient* captureClient = nullptr;
    IAudioRenderClient* renderClient = nullptr;

    WAVEFORMATEX* outputFormat = nullptr;

    HANDLE captureEvent = nullptr;
    HANDLE renderEvent = nullptr;

    HANDLE mmcssHandle = nullptr;
    DWORD mmcssTaskIndex = 0;

    bool inputStarted = false;
    bool outputStarted = false;

    //
    // 오디오 스레드를 MMCSS의 Pro Audio 작업으로 등록
    //
    mmcssHandle = AvSetMmThreadCharacteristicsW(
        L"Pro Audio",
        &mmcssTaskIndex
    );

    do
    {
        //
        // ------------------------------------------------
        // 장치 열기
        // ------------------------------------------------
        //
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator)
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        hr = enumerator->GetDevice(
            inputDeviceId.c_str(),
            &inputDevice
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        hr = enumerator->GetDevice(
            outputDeviceId.c_str(),
            &outputDevice
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        hr = inputDevice->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&inputClient)
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        hr = outputDevice->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&outputClient)
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        //
        // ------------------------------------------------
        // 출력 Mix Format
        // ------------------------------------------------
        //
        hr = outputClient->GetMixFormat(
            &outputFormat
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        if (outputFormat->wFormatTag !=
            WAVE_FORMAT_EXTENSIBLE)
        {
            hadError = true;
            break;
        }

        WAVEFORMATEXTENSIBLE* outputExt =
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(
                outputFormat
                );

        if (!IsEqualGUID(
            outputExt->SubFormat,
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
        {
            hadError = true;
            break;
        }

        const UINT32 outputChannels =
            outputFormat->nChannels;

        DWORD channelMask =
            outputExt->dwChannelMask;

        int rearLeft =
            GetChannelIndex(
                channelMask,
                SPEAKER_BACK_LEFT
            );

        int rearRight =
            GetChannelIndex(
                channelMask,
                SPEAKER_BACK_RIGHT
            );

        if (rearLeft < 0 ||
            rearRight < 0)
        {
            hadError = true;
            break;
        }

        //
        // ------------------------------------------------
        // 입력 형식
        //
        // 항상:
        // 출력과 동일한 sample rate
        // stereo
        // float32
        // ------------------------------------------------
        //
        WAVEFORMATEXTENSIBLE inputFormat = {};

        inputFormat.Format.wFormatTag =
            WAVE_FORMAT_EXTENSIBLE;

        inputFormat.Format.nChannels = 2;

        inputFormat.Format.nSamplesPerSec =
            outputFormat->nSamplesPerSec;

        inputFormat.Format.wBitsPerSample = 32;

        inputFormat.Format.nBlockAlign =
            2 * sizeof(float);

        inputFormat.Format.nAvgBytesPerSec =
            inputFormat.Format.nSamplesPerSec *
            inputFormat.Format.nBlockAlign;

        inputFormat.Format.cbSize =
            sizeof(WAVEFORMATEXTENSIBLE) -
            sizeof(WAVEFORMATEX);

        inputFormat.Samples.wValidBitsPerSample =
            32;

        inputFormat.dwChannelMask =
            SPEAKER_FRONT_LEFT |
            SPEAKER_FRONT_RIGHT;

        inputFormat.SubFormat =
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        //
        // ------------------------------------------------
        // WASAPI 이벤트 생성
        // ------------------------------------------------
        //
        captureEvent = CreateEvent(
            nullptr,
            FALSE,
            FALSE,
            nullptr
        );

        renderEvent = CreateEvent(
            nullptr,
            FALSE,
            FALSE,
            nullptr
        );

        if (!captureEvent ||
            !renderEvent)
        {
            hadError = true;
            break;
        }

        //
        // ------------------------------------------------
        // Input Shared Capture
        // ------------------------------------------------
        //
        DWORD inputFlags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        hr = inputClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            inputFlags,
            0,
            0,
            &inputFormat.Format,
            nullptr
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        hr = inputClient->SetEventHandle(
            captureEvent
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        //
        // ------------------------------------------------
        // Output Shared Render
        // ------------------------------------------------
        //
        DWORD outputFlags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        hr = outputClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            outputFlags,
            0,
            0,
            outputFormat,
            nullptr
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        hr = outputClient->SetEventHandle(
            renderEvent
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        //
        // ------------------------------------------------
        // Capture / Render 인터페이스
        // ------------------------------------------------
        //
        hr = inputClient->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&captureClient)
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        hr = outputClient->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(&renderClient)
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        UINT32 outputBufferFrames = 0;

        hr = outputClient->GetBufferSize(
            &outputBufferFrames
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        //
        // ------------------------------------------------
        // 고정 크기 Stereo Ring Buffer
        // ------------------------------------------------
        //
        struct StereoFrame
        {
            float left;
            float right;
        };

        //
        // 500 ms 분량.
        // 정상 동작 중에는 실제로 이렇게 많이 쌓이지 않는다.
        //
        const size_t ringCapacity =
            outputFormat->nSamplesPerSec / 2;

        std::vector<StereoFrame> ringBuffer(
            ringCapacity
        );

        size_t ringRead = 0;
        size_t ringWrite = 0;
        size_t ringCount = 0;

        auto PushFrame =
            [&](float left, float right)
            {
                //
                // 혹시 buffer가 가득 찼으면
                // 가장 오래된 프레임 하나 버림.
                //
                if (ringCount == ringCapacity)
                {
                    ringRead =
                        (ringRead + 1) %
                        ringCapacity;

                    --ringCount;
                }

                ringBuffer[ringWrite].left =
                    left;

                ringBuffer[ringWrite].right =
                    right;

                ringWrite =
                    (ringWrite + 1) %
                    ringCapacity;

                ++ringCount;
            };

        auto PopFrame =
            [&](StereoFrame& frame) -> bool
            {
                if (ringCount == 0)
                    return false;

                frame =
                    ringBuffer[ringRead];

                ringRead =
                    (ringRead + 1) %
                    ringCapacity;

                --ringCount;

                return true;
            };

        //
        // Capture packet을 ring buffer로 모두 가져오는 함수
        //
        auto DrainCapture = [&]() -> bool
            {
                UINT32 packetFrames = 0;

                HRESULT localHr =
                    captureClient->GetNextPacketSize(
                        &packetFrames
                    );

                if (FAILED(localHr))
                    return false;

                while (packetFrames > 0)
                {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;

                    localHr =
                        captureClient->GetBuffer(
                            &data,
                            &frames,
                            &flags,
                            nullptr,
                            nullptr
                        );

                    if (FAILED(localHr))
                        return false;

                    bool silent =
                        (flags &
                            AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                    if (silent)
                    {
                        for (UINT32 i = 0;
                            i < frames;
                            ++i)
                        {
                            PushFrame(
                                0.0f,
                                0.0f
                            );
                        }
                    }
                    else
                    {
                        float* input =
                            reinterpret_cast<float*>(
                                data
                                );

                        for (UINT32 i = 0;
                            i < frames;
                            ++i)
                        {
                            PushFrame(
                                input[i * 2],
                                input[i * 2 + 1]
                            );
                        }
                    }

                    localHr =
                        captureClient->ReleaseBuffer(
                            frames
                        );

                    if (FAILED(localHr))
                        return false;

                    localHr =
                        captureClient->GetNextPacketSize(
                            &packetFrames
                        );

                    if (FAILED(localHr))
                        return false;
                }

                return true;
            };

        //
        // ------------------------------------------------
        // 먼저 Capture만 시작
        // ------------------------------------------------
        //
        hr = inputClient->Start();

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        inputStarted = true;

        //
        // 20ms 또는 output buffer 하나 분량 중
        // 더 큰 쪽까지 입력을 미리 모은다.
        //
        const size_t prebufferFrames =
            (std::max)(
                static_cast<size_t>(
                    outputFormat->nSamplesPerSec / 50
                    ),
                static_cast<size_t>(
                    outputBufferFrames
                    )
                );

        while (
            g_audioRunning &&
            ringCount < prebufferFrames)
        {
            HANDLE waits[] =
            {
                g_audioStopEvent,
                captureEvent
            };

            DWORD waitResult =
                WaitForMultipleObjects(
                    2,
                    waits,
                    FALSE,
                    INFINITE
                );

            if (waitResult == WAIT_OBJECT_0)
            {
                //
                // 정상 Stop
                //
                break;
            }

            if (waitResult ==
                WAIT_OBJECT_0 + 1)
            {
                if (!DrainCapture())
                {
                    hadError = true;
                    break;
                }
            }
            else
            {
                hadError = true;
                break;
            }
        }

        if (!g_audioRunning ||
            hadError)
        {
            break;
        }

        //
        // ------------------------------------------------
        // 출력 버퍼를 Rear 데이터로 미리 채움
        // ------------------------------------------------
        //
        BYTE* initialData = nullptr;

        hr = renderClient->GetBuffer(
            outputBufferFrames,
            &initialData
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        float* initialOutput =
            reinterpret_cast<float*>(
                initialData
                );

        std::fill(
            initialOutput,
            initialOutput +
            static_cast<size_t>(
                outputBufferFrames
                ) *
            outputChannels,
            0.0f
        );

        for (UINT32 frame = 0;
            frame < outputBufferFrames;
            ++frame)
        {
            StereoFrame stereo = {};

            if (!PopFrame(stereo))
                break;

            float* currentFrame =
                initialOutput +
                static_cast<size_t>(frame) *
                outputChannels;

            currentFrame[rearLeft] =
                stereo.left;

            currentFrame[rearRight] =
                stereo.right;
        }

        hr = renderClient->ReleaseBuffer(
            outputBufferFrames,
            0
        );

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        //
        // 이제 Render 시작
        //
        hr = outputClient->Start();

        if (FAILED(hr))
        {
            hadError = true;
            break;
        }

        outputStarted = true;

        //
        // ------------------------------------------------
        // Event-driven main loop
        // ------------------------------------------------
        //
        while (g_audioRunning)
        {
            HANDLE waits[] =
            {
                g_audioStopEvent,
                captureEvent,
                renderEvent
            };

            DWORD waitResult =
                WaitForMultipleObjects(
                    3,
                    waits,
                    FALSE,
                    INFINITE
                );

            //
            // Stop
            //
            if (waitResult ==
                WAIT_OBJECT_0)
            {
                break;
            }

            //
            // Capture
            //
            if (waitResult ==
                WAIT_OBJECT_0 + 1)
            {
                if (!DrainCapture())
                {
                    hadError = true;
                    break;
                }

                continue;
            }

            //
            // Render
            //
            if (waitResult ==
                WAIT_OBJECT_0 + 2)
            {
                UINT32 padding = 0;

                hr = outputClient->GetCurrentPadding(
                    &padding
                );

                if (FAILED(hr))
                {
                    hadError = true;
                    break;
                }

                UINT32 availableFrames =
                    outputBufferFrames -
                    padding;

                if (availableFrames == 0)
                    continue;

                UINT32 framesToWrite =
                    static_cast<UINT32>(
                        (std::min)(
                            static_cast<size_t>(
                                availableFrames
                                ),
                            ringCount
                            )
                        );

                if (framesToWrite == 0)
                    continue;

                BYTE* outputData = nullptr;

                hr = renderClient->GetBuffer(
                    framesToWrite,
                    &outputData
                );

                if (FAILED(hr))
                {
                    hadError = true;
                    break;
                }

                float* output =
                    reinterpret_cast<float*>(
                        outputData
                        );

                //
                // Front와 나머지 채널은 모두 0.
                // Shared mixer에서는 다른 앱의 Front가
                // 그대로 함께 믹싱된다.
                //
                std::fill(
                    output,
                    output +
                    static_cast<size_t>(
                        framesToWrite
                        ) *
                    outputChannels,
                    0.0f
                );

                for (UINT32 frame = 0;
                    frame < framesToWrite;
                    ++frame)
                {
                    StereoFrame stereo = {};

                    if (!PopFrame(stereo))
                        break;

                    float* currentFrame =
                        output +
                        static_cast<size_t>(
                            frame
                            ) *
                        outputChannels;

                    currentFrame[rearLeft] =
                        stereo.left;

                    currentFrame[rearRight] =
                        stereo.right;
                }

                hr = renderClient->ReleaseBuffer(
                    framesToWrite,
                    0
                );

                if (FAILED(hr))
                {
                    hadError = true;
                    break;
                }

                continue;
            }

            //
            // Wait 자체 실패
            //
            hadError = true;
            break;
        }

    } while (false);

    //
    // ------------------------------------------------
    // Cleanup
    // ------------------------------------------------
    //
    if (inputStarted && inputClient)
        inputClient->Stop();

    if (outputStarted && outputClient)
        outputClient->Stop();

    if (captureClient)
        captureClient->Release();

    if (renderClient)
        renderClient->Release();

    if (inputClient)
        inputClient->Release();

    if (outputClient)
        outputClient->Release();

    if (inputDevice)
        inputDevice->Release();

    if (outputDevice)
        outputDevice->Release();

    if (enumerator)
        enumerator->Release();

    if (outputFormat)
        CoTaskMemFree(outputFormat);

    if (captureEvent)
        CloseHandle(captureEvent);

    if (renderEvent)
        CloseHandle(renderEvent);

    if (mmcssHandle)
        AvRevertMmThreadCharacteristics(
            mmcssHandle
        );

    CoUninitialize();

    g_audioRunning = false;

    PostMessage(
        hWnd,
        WM_AUDIO_STOPPED,
        hadError ? 1 : 0,
        0
    );
}

void StartAudioRouting(HWND hWnd)
{
    int inputIndex = (int)SendMessage(
        g_hComboInput,
        CB_GETCURSEL,
        0,
        0
    );

    int outputIndex = (int)SendMessage(
        g_hComboOutput,
        CB_GETCURSEL,
        0,
        0
    );

    if (inputIndex == CB_ERR ||
        outputIndex == CB_ERR)
    {
        return;
    }

    if (inputIndex < 0 ||
        inputIndex >= (int)g_inputDeviceIds.size() ||
        outputIndex < 0 ||
        outputIndex >= (int)g_outputDeviceIds.size())
    {
        return;
    }

    if (g_audioStopEvent)
    {
        CloseHandle(g_audioStopEvent);
        g_audioStopEvent = nullptr;
    }

    g_audioStopEvent = CreateEvent(
        nullptr,
        TRUE,       // manual reset
        FALSE,
        nullptr
    );

    if (!g_audioStopEvent)
    {
        MessageBox(
            hWnd,
            L"오디오 종료 이벤트를 생성하지 못했습니다.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return;
    }

    std::wstring inputId =
        g_inputDeviceIds[inputIndex];

    std::wstring outputId =
        g_outputDeviceIds[outputIndex];

    g_audioRunning = true;

    SetControlsRunning(true);

    g_audioThread = std::thread(
        AudioThreadProc,
        hWnd,
        inputId,
        outputId
    );
}

void StopAudioRouting()
{
    if (!g_audioRunning &&
        !g_audioThread.joinable())
    {
        return;
    }

    g_audioRunning = false;

    if (g_audioStopEvent)
        SetEvent(g_audioStopEvent);

    if (g_audioThread.joinable())
        g_audioThread.join();

    if (g_audioStopEvent)
    {
        CloseHandle(g_audioStopEvent);
        g_audioStopEvent = nullptr;
    }
}

bool IsAutoStartEnabled()
{
    HKEY hKey = nullptr;

    if (RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        KEY_READ,
        &hKey) != ERROR_SUCCESS)
    {
        return false;
    }

    wchar_t value[MAX_PATH * 2] = {};
    DWORD valueSize = sizeof(value);

    LONG result = RegQueryValueExW(
        hKey,
        L"RearAudioRouter",
        nullptr,
        nullptr,
        reinterpret_cast<LPBYTE>(value),
        &valueSize
    );

    RegCloseKey(hKey);

    return result == ERROR_SUCCESS;
}

bool SetAutoStartEnabled(bool enabled)
{
    HKEY hKey = nullptr;

    LONG result = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        nullptr,
        0,
        KEY_WRITE,
        nullptr,
        &hKey,
        nullptr
    );

    if (result != ERROR_SUCCESS)
        return false;

    if (enabled)
    {
        wchar_t exePath[MAX_PATH] = {};

        GetModuleFileNameW(
            nullptr,
            exePath,
            MAX_PATH
        );

        std::wstring command =
            L"\"" +
            std::wstring(exePath) +
            L"\" --autostart";

        result = RegSetValueExW(
            hKey,
            L"RearAudioRouter",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(
                command.c_str()
                ),
            static_cast<DWORD>(
                (command.size() + 1) *
                sizeof(wchar_t)
                )
        );
    }
    else
    {
        result = RegDeleteValueW(
            hKey,
            L"RearAudioRouter"
        );

        if (result == ERROR_FILE_NOT_FOUND)
            result = ERROR_SUCCESS;
    }

    RegCloseKey(hKey);

    return result == ERROR_SUCCESS;
}

void CreateMainControls(HWND hWnd)
{
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    // Input label
    HWND hInputLabel = CreateWindow(
        L"STATIC",
        L"Input",
        WS_CHILD | WS_VISIBLE,
        20, 20, 200, 20,
        hWnd,
        nullptr,
        nullptr,
        nullptr);

    // Input combo
    g_hComboInput = CreateWindow(
        L"COMBOBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        CBS_DROPDOWNLIST | WS_VSCROLL,
        20, 45, 340, 200,
        hWnd,
        (HMENU)IDC_COMBO_INPUT,
        nullptr,
        nullptr);

    // Output label
    HWND hOutputLabel = CreateWindow(
        L"STATIC",
        L"Output",
        WS_CHILD | WS_VISIBLE,
        20, 90, 200, 20,
        hWnd,
        nullptr,
        nullptr,
        nullptr);

    // Output combo
    g_hComboOutput = CreateWindow(
        L"COMBOBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        CBS_DROPDOWNLIST | WS_VSCROLL,
        20, 115, 340, 200,
        hWnd,
        (HMENU)IDC_COMBO_OUTPUT,
        nullptr,
        nullptr);

    // Destination label
    HWND hDestinationLabel = CreateWindow(
        L"STATIC",
        L"Destination",
        WS_CHILD | WS_VISIBLE,
        20, 165, 200, 20,
        hWnd,
        nullptr,
        nullptr,
        nullptr);

    // Rear L/R radio button
    g_hRadioRear = CreateWindow(
        L"BUTTON",
        L"Rear L/R",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        BS_AUTORADIOBUTTON,
        20, 190, 150, 24,
        hWnd,
        (HMENU)IDC_RADIO_REAR,
        nullptr,
        nullptr);

    SendMessage(g_hRadioRear, BM_SETCHECK, BST_CHECKED, 0);

    g_hCheckAutoStart = CreateWindow(
        L"BUTTON",
        L"Start with Windows",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        20, 235, 180, 24,
        hWnd,
        (HMENU)IDC_CHECK_AUTOSTART,
        nullptr,
        nullptr
    );

    SendMessage(
        g_hCheckAutoStart,
        WM_SETFONT,
        (WPARAM)hFont,
        TRUE
    );

    // Start button
    g_hButtonStart = CreateWindow(
        L"BUTTON",
        L"Start",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        BS_PUSHBUTTON,
        260, 235, 100, 32,
        hWnd,
        (HMENU)IDC_BUTTON_START,
        nullptr,
        nullptr);

    HWND controls[] =
    {
        hInputLabel,
        g_hComboInput,
        hOutputLabel,
        g_hComboOutput,
        hDestinationLabel,
        g_hRadioRear,
        g_hButtonStart
    };

    for (HWND control : controls)
    {
        SendMessage(control, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    EnumerateAudioDevices(
        eCapture,
        g_hComboInput,
        g_inputDeviceIds
    );

    EnumerateAudioDevices(
        eRender,
        g_hComboOutput,
        g_outputDeviceIds
    );

    SendMessage(
        g_hCheckAutoStart,
        BM_SETCHECK,
        IsAutoStartEnabled()
        ? BST_CHECKED
        : BST_UNCHECKED,
        0
    );
}

void AddTrayIcon(HWND hWnd)
{
    g_nid = {};
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    g_nid.hIcon = LoadIcon(
        GetModuleHandle(nullptr),
        MAKEINTRESOURCE(IDI_REARAUDIOROUTER)
    );

    wcscpy_s(
        g_nid.szTip,
        L"RearAudioRouter"
    );

    Shell_NotifyIcon(
        NIM_ADD,
        &g_nid
    );
}

void RemoveTrayIcon()
{
    if (g_nid.hWnd != nullptr)
    {
        Shell_NotifyIcon(
            NIM_DELETE,
            &g_nid
        );

        g_nid = {};
    }
}

void HideToTray(HWND hWnd)
{
    ShowWindow(
        hWnd,
        SW_HIDE
    );
}

void RestoreFromTray(HWND hWnd)
{
    ShowWindow(
        hWnd,
        SW_SHOW
    );

    ShowWindow(
        hWnd,
        SW_RESTORE
    );

    SetForegroundWindow(
        hWnd
    );
}

void ShowTrayMenu(HWND hWnd)
{
    HMENU menu = CreatePopupMenu();

    if (!menu)
        return;

    AppendMenu(
        menu,
        MF_STRING,
        ID_TRAY_OPEN,
        L"Open"
    );

    AppendMenu(
        menu,
        MF_STRING,
        ID_TRAY_STARTSTOP,
        g_audioRunning
        ? L"Stop"
        : L"Start"
    );

    AppendMenu(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    AppendMenu(
        menu,
        MF_STRING,
        ID_TRAY_EXIT,
        L"Exit"
    );

    POINT pt;
    GetCursorPos(&pt);

    SetForegroundWindow(hWnd);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        pt.x,
        pt.y,
        0,
        hWnd,
        nullptr
    );

    DestroyMenu(menu);
}

std::wstring GetConfigPath()
{
    PWSTR appDataPath = nullptr;

    HRESULT hr = SHGetKnownFolderPath(
        FOLDERID_RoamingAppData,
        0,
        nullptr,
        &appDataPath
    );

    if (FAILED(hr))
        return L"RearAudioRouter.ini";

    std::wstring folder =
        std::wstring(appDataPath) +
        L"\\RearAudioRouter";

    CoTaskMemFree(appDataPath);

    CreateDirectoryW(
        folder.c_str(),
        nullptr
    );

    return folder + L"\\config.ini";
}

bool SelectDeviceById(
    HWND comboBox,
    const std::vector<std::wstring>& deviceIds,
    const std::wstring& wantedId)
{
    for (size_t i = 0; i < deviceIds.size(); ++i)
    {
        if (deviceIds[i] == wantedId)
        {
            SendMessage(
                comboBox,
                CB_SETCURSEL,
                static_cast<WPARAM>(i),
                0
            );

            return true;
        }
    }

    return false;
}

void SaveSettings(bool running)
{
    if (g_configPath.empty())
        return;

    int inputIndex = (int)SendMessage(
        g_hComboInput,
        CB_GETCURSEL,
        0,
        0
    );

    int outputIndex = (int)SendMessage(
        g_hComboOutput,
        CB_GETCURSEL,
        0,
        0
    );

    if (inputIndex >= 0 &&
        inputIndex < (int)g_inputDeviceIds.size())
    {
        WritePrivateProfileStringW(
            L"Audio",
            L"InputDevice",
            g_inputDeviceIds[inputIndex].c_str(),
            g_configPath.c_str()
        );
    }

    if (outputIndex >= 0 &&
        outputIndex < (int)g_outputDeviceIds.size())
    {
        WritePrivateProfileStringW(
            L"Audio",
            L"OutputDevice",
            g_outputDeviceIds[outputIndex].c_str(),
            g_configPath.c_str()
        );
    }

    WritePrivateProfileStringW(
        L"Audio",
        L"Running",
        running ? L"1" : L"0",
        g_configPath.c_str()
    );
}

bool LoadSettings()
{
    if (g_configPath.empty())
        return false;

    wchar_t inputId[1024] = {};
    wchar_t outputId[1024] = {};

    GetPrivateProfileStringW(
        L"Audio",
        L"InputDevice",
        L"",
        inputId,
        1024,
        g_configPath.c_str()
    );

    GetPrivateProfileStringW(
        L"Audio",
        L"OutputDevice",
        L"",
        outputId,
        1024,
        g_configPath.c_str()
    );

    if (inputId[0] != L'\0')
    {
        SelectDeviceById(
            g_hComboInput,
            g_inputDeviceIds,
            inputId
        );
    }

    if (outputId[0] != L'\0')
    {
        SelectDeviceById(
            g_hComboOutput,
            g_outputDeviceIds,
            outputId
        );
    }

    int running = GetPrivateProfileIntW(
        L"Audio",
        L"Running",
        0,
        g_configPath.c_str()
    );

    return running != 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: 여기에 코드를 입력합니다.

    HRESULT hr = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED
    );

    if (FAILED(hr))
    {
        MessageBox(
            nullptr,
            L"COM initialization failed.",
            L"RearAudioRouter",
            MB_OK | MB_ICONERROR
        );

        return 0;
    }

    if (wcsstr(lpCmdLine, L"--autostart") != nullptr)
    {
        g_startedFromAutoStart = true;
    }

    // 전역 문자열을 초기화합니다.
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_REARAUDIOROUTER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // 애플리케이션 초기화를 수행합니다:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_REARAUDIOROUTER));

    MSG msg;

    // 기본 메시지 루프입니다:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    CoUninitialize();

    return (int) msg.wParam;
}

//
//  함수: MyRegisterClass()
//
//  용도: 창 클래스를 등록합니다.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_REARAUDIOROUTER));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_BTNFACE+1);
 // wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_REARAUDIOROUTER);
    wcex.lpszMenuName   = nullptr;
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   함수: InitInstance(HINSTANCE, int)
//
//   용도: 인스턴스 핸들을 저장하고 주 창을 만듭니다.
//
//   주석:
//
//        이 함수를 통해 인스턴스 핸들을 전역 변수에 저장하고
//        주 프로그램 창을 만든 다음 표시합니다.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // 인스턴스 핸들을 전역 변수에 저장합니다.

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, 400, 340, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   if (g_startedFromAutoStart)
   {
       ShowWindow(hWnd, SW_HIDE);
   }
   else
   {
       ShowWindow(hWnd, nCmdShow);
   }

   UpdateWindow(hWnd);

   return TRUE;
}

//
//  함수: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  용도: 주 창의 메시지를 처리합니다.
//
//  WM_COMMAND  - 애플리케이션 메뉴를 처리합니다.
//  WM_PAINT    - 주 창을 그립니다.
//  WM_DESTROY  - 종료 메시지를 게시하고 반환합니다.
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        CreateMainControls(hWnd);
        AddTrayIcon(hWnd);

        g_configPath = GetConfigPath();

        bool shouldStart =
            LoadSettings();

        if (shouldStart)
        {
            StartAudioRouting(hWnd);
        }

        break;
    }
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // 메뉴 선택을 구문 분석합니다:
            switch (wmId)
            {
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            case IDC_BUTTON_START:
                if (g_audioRunning)
                {
                    StopAudioRouting();
                    SetControlsRunning(false);

                    SaveSettings(false);
                }
                else
                {
                    if (!ValidateInputDevice(hWnd))
                        break;
                    if (!ValidateOutputDevice(hWnd))
                        break;
                    StartAudioRouting(hWnd);

                    SaveSettings(true);
                }
                break;
            case ID_TRAY_OPEN:
                RestoreFromTray(hWnd);
                break;

            case ID_TRAY_STARTSTOP:
                if (g_audioRunning)
                {
                    StopAudioRouting();
                    SetControlsRunning(false);

                    SaveSettings(false);
                }
                else
                {
                    if (!ValidateInputDevice(hWnd))
                        break;
                    if (!ValidateOutputDevice(hWnd))
                        break;
                    StartAudioRouting(hWnd);

                    SaveSettings(true);
                }
                break;
            case IDC_COMBO_INPUT:
            case IDC_COMBO_OUTPUT:
                if (HIWORD(wParam) == CBN_SELCHANGE)
                {
                    SaveSettings(g_audioRunning);
                }
                break;
            case ID_TRAY_EXIT:
                DestroyWindow(hWnd);
                break;
            case IDC_CHECK_AUTOSTART:
                if (HIWORD(wParam) == BN_CLICKED)
                {
                    bool enabled =
                        SendMessage(
                            g_hCheckAutoStart,
                            BM_GETCHECK,
                            0,
                            0
                        ) == BST_CHECKED;

                    if (!SetAutoStartEnabled(enabled))
                    {
                        MessageBox(
                            hWnd,
                            L"Windows 시작 프로그램 설정을 변경하지 못했습니다.",
                            L"RearAudioRouter",
                            MB_OK | MB_ICONERROR
                        );
                    }
                }
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
    case WM_AUDIO_STOPPED:
    {
        if (g_audioThread.joinable())
            g_audioThread.join();

        if (g_audioStopEvent)
        {
            CloseHandle(g_audioStopEvent);
            g_audioStopEvent = nullptr;
        }

        SetControlsRunning(false);

        if (wParam != 0)
        {
            MessageBox(
                hWnd,
                L"오디오 스트림이 예기치 않게 종료되었습니다.",
                L"RearAudioRouter",
                MB_OK | MB_ICONERROR
            );
        }

        break;
    }
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            // TODO: 여기에 그리기 코드를 추가합니다...
            EndPaint(hWnd, &ps);
        }
        break;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            HideToTray(hWnd);
            return 0;
        }
        break;
    case WM_TRAYICON:
        switch (lParam)
        {
        case WM_LBUTTONDBLCLK:
            RestoreFromTray(hWnd);
            break;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu(hWnd);
            break;
        }

        break;
    case WM_DESTROY:
    {
        bool wasRunning = g_audioRunning;

        SaveSettings(wasRunning);

        StopAudioRouting();
        RemoveTrayIcon();

        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// 정보 대화 상자의 메시지 처리기입니다.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
