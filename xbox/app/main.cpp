// chiaki-ng for Xbox — UWP CoreApplication front-end (no XAML).
// Video: Media Foundation hardware H.264 decode -> D3D11 video processor -> swapchain
// Audio: chiaki opus decoder -> XAudio2
// Input: Windows.Gaming.Input gamepad -> DualSense state
// UI text: Direct2D / DirectWrite

#include <windows.h>
#include <unknwn.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <xaudio2.h>
#include <wincodec.h>
#include <wrl/client.h>
#undef DrawText

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <thread>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cerrno>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <cstdio>
#include <cstdarg>
#include <chrono>

extern "C" {
#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/discovery.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/controller.h>
#include <chiaki/base64.h>
}
#include "fg_luma.h"
#include "fg_motion.h"
#include "fg_smooth.h"
#include "fg_interp.h"
#include "fg_filter.h"
extern "C" {
}

using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::Gaming::Input;
using namespace winrt::Windows::Graphics::Display::Core;

static const UINT OUT_W = 1920, OUT_H = 1080;
// CODECAPI_AVLowLatencyMode
static const GUID kAVLowLatencyMode = { 0x9c27891a, 0xed7a, 0x40e1, { 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee } };

// ---------------------------------------------------------------- logging
static std::mutex g_log_mutex;
static std::deque<std::string> g_log_lines;
static FILE *g_log_file = nullptr;

static void AppLog(const char *fmt, ...)
{
	char buf[1024];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	std::lock_guard<std::mutex> l(g_log_mutex);
	g_log_lines.push_back(buf);
	while(g_log_lines.size() > 14) g_log_lines.pop_front();
	if(g_log_file) { fprintf(g_log_file, "%s\n", buf); fflush(g_log_file); }
	OutputDebugStringA(buf); OutputDebugStringA("\n");
}

static void ChiakiLogCbFn(ChiakiLogLevel level, const char *msg, void *)
{
	if(level == CHIAKI_LOG_VERBOSE) return;
	AppLog("%s", msg);
}

static std::wstring Widen(const std::string &s)
{
	if(s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

// ---------------------------------------------------------------- config
struct Profile { std::string name; std::string account_id_b64; };
struct Config { std::string host; std::vector<Profile> profiles; };
static const size_t kMaxProfiles = 6;

static std::string ReadFileStr(const std::wstring &path)
{
	FILE *f = _wfopen(path.c_str(), L"rb");
	if(!f) return {};
	std::string s; char b[4096]; size_t n;
	while((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
	fclose(f);
	return s;
}

static std::string JsonGet(const std::string &j, const char *key)
{
	std::string k = std::string("\"") + key + "\"";
	size_t p = j.find(k); if(p == std::string::npos) return {};
	p = j.find(':', p); if(p == std::string::npos) return {};
	p = j.find('"', p); if(p == std::string::npos) return {};
	size_t e = j.find('"', p + 1); if(e == std::string::npos) return {};
	return j.substr(p + 1, e - p - 1);
}

static std::wstring g_install_dir, g_local_dir;

// Optional starter config shipped in the package (Assets\config.json). The public build ships it empty.
static Config LoadBundledConfig()
{
	Config c;
	std::string j = ReadFileStr(g_install_dir + L"\\Assets\\config.json");
	c.host = JsonGet(j, "ps5_ip");
	if(c.host.find("PUT_") == 0) c.host.clear();
	size_t pos = j.find("\"profiles\"");
	while(pos != std::string::npos)
	{
		size_t n = j.find("\"name\"", pos);
		if(n == std::string::npos) break;
		size_t end = j.find('}', n);
		std::string obj = j.substr(n, end == std::string::npos ? std::string::npos : end - n);
		Profile p; p.name = JsonGet("{" + obj, "name"); p.account_id_b64 = JsonGet("{" + obj, "psn_account_id");
		if(!p.name.empty() && !p.account_id_b64.empty() && p.account_id_b64.find("PUT_") != 0) c.profiles.push_back(p);
		pos = end;
	}
	return c;
}

static std::wstring ConfigPath() { return g_local_dir + L"\\profiles.txt"; }

static void SaveConfig(const Config &c)
{
	FILE *f = _wfopen(ConfigPath().c_str(), L"w");
	if(!f) return;
	fprintf(f, "host=%s\n", c.host.c_str());
	for(auto &p : c.profiles) fprintf(f, "profile=%s|%s\n", p.name.c_str(), p.account_id_b64.c_str());
	fclose(f);
}

// Profiles live in the app's own storage, edited in the app. The first launch copies any starter config.
static Config LoadConfig()
{
	std::string t = ReadFileStr(ConfigPath());
	if(t.empty())
	{
		Config c = LoadBundledConfig();
		SaveConfig(c);
		return c;
	}
	Config c;
	size_t pos = 0;
	while(pos < t.size())
	{
		size_t e = t.find('\n', pos);
		std::string line = t.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
		pos = e == std::string::npos ? t.size() : e + 1;
		while(!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if(line.rfind("host=", 0) == 0) c.host = line.substr(5);
		else if(line.rfind("profile=", 0) == 0)
		{
			std::string v = line.substr(8);
			size_t bar = v.find('|');
			if(bar == std::string::npos) continue;
			Profile p; p.name = v.substr(0, bar); p.account_id_b64 = v.substr(bar + 1);
			if(!p.name.empty() && !p.account_id_b64.empty() && c.profiles.size() < kMaxProfiles) c.profiles.push_back(p);
		}
	}
	return c;
}

// Accepts a PSN account ID as base64 (like "AbCdEfGhIjk=") or as the plain number. Returns base64, or "" if invalid.
static std::string NormalizeAccountId(const std::string &in)
{
	std::string s;
	for(char ch : in) if(ch != ' ') s += ch;
	if(s.empty()) return {};
	bool digits = true;
	for(char ch : s) if(ch < '0' || ch > '9') { digits = false; break; }
	uint8_t b[CHIAKI_PSN_ACCOUNT_ID_SIZE] = {};
	if(digits && s.size() >= 5)
	{
		errno = 0;
		unsigned long long v = strtoull(s.c_str(), nullptr, 10);
		if(errno) return {};
		for(int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); // little-endian, like the PS5 uses
	}
	else
	{
		size_t outSize = sizeof(b);
		if(chiaki_base64_decode(s.c_str(), s.size(), b, &outSize) != CHIAKI_ERR_SUCCESS || outSize != sizeof(b)) return {};
	}
	char enc[32] = {};
	if(chiaki_base64_encode(b, sizeof(b), enc, sizeof(enc)) != CHIAKI_ERR_SUCCESS) return {};
	return enc;
}

static std::wstring g_profile_name = L"MAIN";
static std::wstring HostFileFor(const std::wstring &name) { return g_local_dir + L"\\registered_host_" + name + L".bin"; }
static std::wstring HostFile() { return HostFileFor(g_profile_name); }

static bool LoadHost(ChiakiRegisteredHost *h)
{
	FILE *f = _wfopen(HostFile().c_str(), L"rb");
	if(!f && g_profile_name == L"MAIN") f = _wfopen((g_local_dir + L"\\registered_host.bin").c_str(), L"rb"); // link from before profiles existed
	if(!f) return false;
	bool ok = fread(h, sizeof(*h), 1, f) == 1;
	fclose(f);
	return ok;
}

static void SaveHost(const ChiakiRegisteredHost *h)
{
	FILE *f = _wfopen(HostFile().c_str(), L"wb");
	if(!f) return;
	fwrite(h, sizeof(*h), 1, f);
	fclose(f);
}

// ---------------------------------------------------------------- app state
enum class State { Config, ProfileSelect, EditProfile, Keyboard, Help, PinEntry, Registering, Connecting, Streaming, Error };


class ChiakiXbox
{
public:
	// D3D / D2D
	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	ComPtr<ID3D11VideoDevice> vdev;
	ComPtr<ID3D11VideoContext> vctx;
	ComPtr<IDXGISwapChain1> swap;
	ComPtr<ID3D11Texture2D> backbuf;
	HANDLE frameWait = nullptr;
	ComPtr<ID2D1Factory3> d2dfac;
	ComPtr<ID2D1Device2> d2ddev;
	ComPtr<ID2D1DeviceContext2> d2dctx;
	ComPtr<ID2D1Bitmap1> d2dtarget;
	ComPtr<IDWriteFactory> dwfac;
	ComPtr<IDWriteTextFormat> fmtBig, fmtSmall;
	ComPtr<ID2D1SolidColorBrush> brushW, brushG, brushBg;

	// video processor
	ComPtr<ID3D11VideoProcessorEnumerator> vpenum;
	ComPtr<ID3D11VideoProcessor> vp;
	ComPtr<ID3D11VideoProcessorOutputView> vpout;
	ComPtr<ID3D11Texture2D> frameTex; // our copy of the latest decoded NV12 frame
	ComPtr<ID3D11VideoProcessorInputView> vpin;
	UINT frameW = 0, frameH = 0;
	DXGI_FORMAT frameFmt = DXGI_FORMAT_NV12;
	std::atomic<bool> wantHdrOutput{false};
	bool hdrOutputActive = false, hdrCapable = false;
	std::mutex frameMutex;
	bool haveFrame = false;

	// MF decoder
	ComPtr<IMFDXGIDeviceManager> dxgiMgr;
	UINT resetToken = 0;
	ComPtr<IMFTransform> decoder;
	bool decoderOutputSet = false;

	// audio
	ComPtr<IXAudio2> xa;
	IXAudio2MasteringVoice *master = nullptr;
	IXAudio2SourceVoice *srcVoice = nullptr;
	ChiakiOpusDecoder opus;

	// chiaki
	ChiakiLog log;
	ChiakiSession session;
	bool sessionActive = false;
	ChiakiRegist regist;
	bool registActive = false;
	ChiakiRegisteredHost host{};
	Config cfg;
	uint8_t accountId[CHIAKI_PSN_ACCOUNT_ID_SIZE]{};
	std::string hostStr;

	std::atomic<State> state{State::Config};
	std::string statusMsg;
	int pinDigits[8]{};
	int pinCursor = 0;
	uint64_t lastButtons = 0;

	// ------------------------------------------------------------ init
	void InitGraphics(CoreWindow const &window)
	{
		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
		D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, ARRAYSIZE(fl),
			D3D11_SDK_VERSION, &dev, nullptr, &ctx));
		ComPtr<ID3D10Multithread> mt;
		if(SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
		check_hresult(dev.As(&vdev));
		check_hresult(ctx.As(&vctx));

		ComPtr<IDXGIDevice1> dxgiDev; check_hresult(dev.As(&dxgiDev));
		ComPtr<IDXGIAdapter> adapter; check_hresult(dxgiDev->GetAdapter(&adapter));
		ComPtr<IDXGIFactory2> factory; check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));

		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.Width = OUT_W; sd.Height = OUT_H;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 2;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		check_hresult(factory->CreateSwapChainForCoreWindow(dev.Get(), (IUnknown *)winrt::get_abi(window), &sd, nullptr, &swap));
		check_hresult(swap->GetBuffer(0, IID_PPV_ARGS(&backbuf)));
		ComPtr<IDXGISwapChain2> swap2;
		if(SUCCEEDED(swap.As(&swap2)))
		{
			swap2->SetMaximumFrameLatency(1);
			frameWait = swap2->GetFrameLatencyWaitableObject();
		}
		ComPtr<IDXGIDevice1> dd1;
		if(SUCCEEDED(dev.As(&dd1))) dd1->SetMaximumFrameLatency(1);

		// D2D
		D2D1_FACTORY_OPTIONS fo{};
		check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory3), &fo, (void **)d2dfac.GetAddressOf()));
		check_hresult(d2dfac->CreateDevice(dxgiDev.Get(), &d2ddev));
		check_hresult(d2ddev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dctx));
		ComPtr<IDXGISurface> surf; check_hresult(backbuf.As(&surf));
		D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		check_hresult(d2dctx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &d2dtarget));
		d2dctx->SetTarget(d2dtarget.Get());
		check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)dwfac.GetAddressOf()));
		check_hresult(dwfac->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
			DWRITE_FONT_STRETCH_NORMAL, 44.0f, L"en-us", &fmtBig));
		check_hresult(dwfac->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
			DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us", &fmtSmall));
		d2dctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brushW);
		d2dctx->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.85f, 0.45f, 1), &brushG);
		d2dctx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &brushBg);

		// Media Foundation device manager
		check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL));
		check_hresult(MFCreateDXGIDeviceManager(&resetToken, &dxgiMgr));
		check_hresult(dxgiMgr->ResetDevice(dev.Get(), resetToken));
	}

	void InitAudio()
	{
		if(FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { AppLog("XAudio2Create failed"); return; }
		if(FAILED(xa->CreateMasteringVoice(&master))) AppLog("CreateMasteringVoice failed");
	}

	// ------------------------------------------------------------ video decode
	static bool DecoderAvailable(const GUID &sub)
	{
		MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, sub };
		IMFActivate **acts = nullptr; UINT32 count = 0;
		if(FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &acts, &count))) return false;
		for(UINT32 i = 0; i < count; i++) acts[i]->Release();
		CoTaskMemFree(acts);
		if(count) return true;
		acts = nullptr; count = 0;
		if(FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &acts, &count))) return false;
		for(UINT32 i = 0; i < count; i++) acts[i]->Release();
		CoTaskMemFree(acts);
		return count > 0;
	}

	GUID decoderSubtype = MFVideoFormat_H264;

	bool CreateDecoder()
	{
		decoder.Reset(); decoderOutputSet = false;
		MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, decoderSubtype };
		IMFActivate **acts = nullptr; UINT32 count = 0;
		HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &acts, &count);
		if(FAILED(hr) || count == 0) { AppLog("No video decoder found (hr=0x%08x)", hr); return false; }
		hr = acts[0]->ActivateObject(IID_PPV_ARGS(&decoder));
		for(UINT32 i = 0; i < count; i++) acts[i]->Release();
		CoTaskMemFree(acts);
		if(FAILED(hr)) { AppLog("Decoder activate failed 0x%08x", hr); return false; }

		ComPtr<IMFAttributes> attrs;
		if(SUCCEEDED(decoder->GetAttributes(&attrs)))
		{
			UINT32 d3dAware = 0;
			attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
			if(d3dAware)
			{
				hr = decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)dxgiMgr.Get());
				AppLog("Decoder D3D11 hardware mode: %s", SUCCEEDED(hr) ? "on" : "failed");
			}
			attrs->SetUINT32(kAVLowLatencyMode, TRUE);
		}

		ComPtr<IMFMediaType> it; MFCreateMediaType(&it);
		it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		it->SetGUID(MF_MT_SUBTYPE, decoderSubtype);
		MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
		MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, 60, 1);
		it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		hr = decoder->SetInputType(0, it.Get(), 0);
		if(FAILED(hr)) { AppLog("SetInputType failed 0x%08x", hr); return false; }
		SetDecoderOutputType();
		decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
		decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
		return true;
	}

	bool hdrRequested = false;
	void SetDecoderOutputType()
	{
		ComPtr<IMFMediaType> p010, nv12;
		for(DWORD i = 0;; i++)
		{
			ComPtr<IMFMediaType> ot;
			if(FAILED(decoder->GetOutputAvailableType(0, i, &ot))) break;
			GUID sub{}; ot->GetGUID(MF_MT_SUBTYPE, &sub);
			if(sub == MFVideoFormat_P010) { if(!p010) p010 = ot; }
			else if(sub == MFVideoFormat_NV12) { if(!nv12) nv12 = ot; }
		}
		ComPtr<IMFMediaType> pick = (hdrRequested && p010) ? p010 : (nv12 ? nv12 : p010);
		if(!pick) { AppLog("Decoder: no usable output type"); return; }
		if(SUCCEEDED(decoder->SetOutputType(0, pick.Get(), 0)))
		{
			UINT32 w = 0, h = 0; MFGetAttributeSize(pick.Get(), MF_MT_FRAME_SIZE, &w, &h);
			AppLog("Decoder output %s %ux%u", pick == p010 ? "P010 (10-bit)" : "NV12 (8-bit)", w, h);
			decoderOutputSet = true;
		}
	}

	void DecodeAndPresent(uint8_t *buf, size_t size)
	{
		if(!decoder) return;
		ComPtr<IMFSample> sample; MFCreateSample(&sample);
		ComPtr<IMFMediaBuffer> mb; MFCreateMemoryBuffer((DWORD)size, &mb);
		BYTE *p = nullptr; mb->Lock(&p, nullptr, nullptr); memcpy(p, buf, size); mb->Unlock();
		mb->SetCurrentLength((DWORD)size);
		sample->AddBuffer(mb.Get());
		HRESULT hr = decoder->ProcessInput(0, sample.Get(), 0);
		if(hr == MF_E_NOTACCEPTING) { DrainOutput(); hr = decoder->ProcessInput(0, sample.Get(), 0); }
		if(FAILED(hr)) { AppLog("ProcessInput failed 0x%08x", hr); return; }
		DrainOutput();
	}

	void DrainOutput()
	{
		for(;;)
		{
			MFT_OUTPUT_STREAM_INFO si{}; decoder->GetOutputStreamInfo(0, &si);
			MFT_OUTPUT_DATA_BUFFER ob{};
			ComPtr<IMFSample> own;
			if(!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
			{
				ComPtr<IMFMediaBuffer> b; MFCreateMemoryBuffer(si.cbSize, &b);
				MFCreateSample(&own); own->AddBuffer(b.Get());
				ob.pSample = own.Get();
			}
			DWORD status = 0;
			HRESULT hr = decoder->ProcessOutput(0, 1, &ob, &status);
			if(ob.pEvents) ob.pEvents->Release();
			if(hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
			if(hr == MF_E_TRANSFORM_STREAM_CHANGE) { SetDecoderOutputType(); continue; }
			if(FAILED(hr)) { AppLog("ProcessOutput failed 0x%08x", hr); return; }
			if(ob.pSample)
			{
				OnDecodedSample(ob.pSample);
				if(!own) ob.pSample->Release();
			}
		}
	}

	void OnDecodedSample(IMFSample *s)
	{
		ComPtr<IMFMediaBuffer> b;
		if(FAILED(s->GetBufferByIndex(0, &b))) return;
		ComPtr<IMFDXGIBuffer> db;
		if(FAILED(b.As(&db))) { AppLog("Decoded frame is not a GPU texture (software decode not supported yet)"); return; }
		ComPtr<ID3D11Texture2D> tex; UINT sub = 0;
		db->GetResource(IID_PPV_ARGS(&tex)); db->GetSubresourceIndex(&sub);
		D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);

		std::lock_guard<std::mutex> l(frameMutex);
		if(!frameTex || frameW != td.Width || frameH != td.Height)
		{
			frameW = td.Width; frameH = td.Height; frameFmt = td.Format;
			if(frameFmt == DXGI_FORMAT_P010 && !hdrOutputActive) wantHdrOutput = true;
			D3D11_TEXTURE2D_DESC nd{};
			nd.Width = td.Width; nd.Height = td.Height; nd.MipLevels = 1; nd.ArraySize = 1;
			nd.Format = td.Format; nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT;
			nd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER;
			frameTex.Reset(); vpin.Reset(); vp.Reset(); vpenum.Reset(); vpout.Reset();
			if(FAILED(dev->CreateTexture2D(&nd, nullptr, &frameTex))) { nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; dev->CreateTexture2D(&nd, nullptr, &frameTex); }
			SetupVideoProcessor();
		}
		ctx->CopySubresourceRegion(frameTex.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
		haveFrame = true;
		frameSeq++;
	}

	void SetupVideoProcessor()
	{
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
		cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		cd.InputWidth = frameW; cd.InputHeight = frameH;
		cd.OutputWidth = OUT_W; cd.OutputHeight = OUT_H;
		cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
		if(FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &vpenum))) { AppLog("VP enumerator failed"); return; }
		if(FAILED(vdev->CreateVideoProcessor(vpenum.Get(), 0, &vp))) { AppLog("VP create failed"); return; }
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{}; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		if(FAILED(vdev->CreateVideoProcessorInputView(frameTex.Get(), vpenum.Get(), &ivd, &vpin))) AppLog("VP input view failed");
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		if(FAILED(vdev->CreateVideoProcessorOutputView(backbuf.Get(), vpenum.Get(), &ovd, &vpout))) AppLog("VP output view failed");
		for(int i = 0; i < 2; i++)
		{
			fgVpOut[i].Reset();
			if(fgReady && FAILED(vdev->CreateVideoProcessorOutputView(fgRgb[i].Get(), vpenum.Get(), &ovd, &fgVpOut[i])))
				AppLog("Frame gen: VP output view failed");
		}
		RECT r{ 0, 0, (LONG)OUT_W, (LONG)OUT_H };
		vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
		ComPtr<ID3D11VideoContext1> vctx1;
		if(SUCCEEDED(vctx.As(&vctx1)))
		{
			vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, frameFmt == DXGI_FORMAT_P010
				? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
			vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), hdrOutputActive
				? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		}
		vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &r);
		vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &r);
	}

	// ------------------------------------------------------------ audio
	void OnAudioSettings(uint32_t channels, uint32_t rate)
	{
		if(!xa) return;
		if(srcVoice) { srcVoice->DestroyVoice(); srcVoice = nullptr; }
		WAVEFORMATEX wf{};
		wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = (WORD)channels; wf.nSamplesPerSec = rate;
		wf.wBitsPerSample = 16; wf.nBlockAlign = (WORD)(channels * 2); wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
		if(FAILED(xa->CreateSourceVoice(&srcVoice, &wf, 0, 2.0f, &voiceCb))) { AppLog("CreateSourceVoice failed"); return; }
		srcVoice->Start();
		AppLog("Audio: %u ch @ %u Hz", channels, rate);
	}

	struct VoiceCb : IXAudio2VoiceCallback
	{
		void STDMETHODCALLTYPE OnBufferEnd(void *ctx) override { delete[] (uint8_t *)ctx; }
		void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
		void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
		void STDMETHODCALLTYPE OnStreamEnd() override {}
		void STDMETHODCALLTYPE OnBufferStart(void *) override {}
		void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
		void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override {}
	} voiceCb;

	void OnAudioFrame(int16_t *buf, size_t samples)
	{
		if(!srcVoice) return;
		XAUDIO2_VOICE_STATE vs; srcVoice->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
		if(vs.BuffersQueued > 8) return; // drop to keep latency low
		XAUDIO2_VOICE_DETAILS d; srcVoice->GetVoiceDetails(&d);
		size_t bytes = samples * d.InputChannels * 2;
		uint8_t *copy = new uint8_t[bytes];
		memcpy(copy, buf, bytes);
		XAUDIO2_BUFFER xb{}; xb.AudioBytes = (UINT32)bytes; xb.pAudioData = copy; xb.pContext = copy;
		if(FAILED(srcVoice->SubmitSourceBuffer(&xb))) delete[] copy;
	}

	// ------------------------------------------------------------ chiaki
	void Start()
	{
		chiaki_log_init(&log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, ChiakiLogCbFn, this);
		ChiakiErrorCode err = chiaki_lib_init();
		if(err != CHIAKI_ERR_SUCCESS) { Fail(std::string("chiaki_lib_init failed: ") + chiaki_error_string(err)); return; }
		cfg = LoadConfig();
		AppLog("Console IP: %s, %d profile(s)", cfg.host.empty() ? "(not set)" : cfg.host.c_str(), (int)cfg.profiles.size());
		GoProfiles();
	}

	// ---- main page: the profiles, then "+ Add profile" (up to 6), "Console IP" and "Controls & help"
	int profileCursor = 0;
	bool CanAdd() { return cfg.profiles.size() < kMaxProfiles; }
	int ItemAdd() { return CanAdd() ? (int)cfg.profiles.size() : -1; }
	int ItemIp() { return (int)cfg.profiles.size() + (CanAdd() ? 1 : 0); }
	int ItemHelp() { return ItemIp() + 1; }
	int MainItems() { return ItemHelp() + 1; }

	void GoProfiles()
	{
		menuOpen = false;
		if(cfg.host.empty()) statusMsg = "Start by setting the Console IP of the PS5 you stream from";
		else if(cfg.profiles.empty()) statusMsg = "Add a profile: a name and the PSN account ID signed in on the PS5";
		else statusMsg = "Who's playing?";
		if(profileCursor >= MainItems()) profileCursor = 0;
		RefreshLinked();
		state = State::ProfileSelect;
	}

	void MainActivate()
	{
		int i = profileCursor;
		if(i < (int)cfg.profiles.size()) ChooseProfile(i);
		else if(i == ItemAdd()) OpenEditor(-1);
		else if(i == ItemIp()) OpenKeyboard(&cfg.host, "Console IP (the PS5 you stream from)", 1, 15, State::ProfileSelect);
		else if(i == ItemHelp()) { helpBack = State::ProfileSelect; state = State::Help; }
	}

	void ChooseProfile(int i)
	{
		if(cfg.host.empty()) { statusMsg = "Set the Console IP first"; return; }
		const Profile &p = cfg.profiles[i];
		g_profile_name = Widen(p.name);
		size_t outSize = sizeof(accountId);
		if(chiaki_base64_decode(p.account_id_b64.c_str(), p.account_id_b64.size(), accountId, &outSize) != CHIAKI_ERR_SUCCESS || outSize != sizeof(accountId))
		{
			Fail("The PSN account ID for " + p.name + " isn't valid - press B, then Y on the profile to edit it");
			return;
		}
		hostStr = cfg.host;
		AppLog("Profile: %s -> %s", p.name.c_str(), hostStr.c_str());
		if(LoadHost(&host)) Connect();
		else
		{
			memset(pinDigits, 0, sizeof(pinDigits)); pinCursor = 0;
			state = State::PinEntry;
			statusMsg = p.name + ": on the PS5 (signed in as this user) go to Settings > System > Remote Play > Link Device";
		}
	}

	// ---- profile editor
	int editIdx = -1, editSel = 0;
	std::string editName, editAcc, editMsg;
	int EditItems() { return editIdx >= 0 ? 4 : 3; } // name, account, save, (delete)
	enum { ED_NAME, ED_ACC, ED_SAVE, ED_DELETE, ED_CANCEL };
	int EditItemAt(int row) { return row; }

	void OpenEditor(int idx)
	{
		editIdx = idx; editSel = 0; editMsg.clear();
		if(idx >= 0) { editName = cfg.profiles[idx].name; editAcc = cfg.profiles[idx].account_id_b64; }
		else { editName.clear(); editAcc.clear(); }
		state = State::EditProfile;
	}

	void EditActivate()
	{
		switch(EditItemAt(editSel))
		{
			case ED_NAME: OpenKeyboard(&editName, "Profile name", 0, 16, State::EditProfile); break;
			case ED_ACC: OpenKeyboard(&editAcc, "PSN account ID (base64 or number)", 0, 24, State::EditProfile); break;
			case ED_SAVE:
			{
				if(editName.empty()) { editMsg = "Give the profile a name"; break; }
				for(size_t i = 0; i < cfg.profiles.size(); i++)
					if((int)i != editIdx && cfg.profiles[i].name == editName) { editMsg = "Another profile already has that name"; return; }
				std::string acc = NormalizeAccountId(editAcc);
				if(acc.empty()) { editMsg = "That PSN account ID isn't valid (base64 like AbCdEfGhIjk= or the number)"; break; }
				if(editIdx >= 0)
				{
					std::string old = cfg.profiles[editIdx].name;
					if(old != editName) MoveFileExW(HostFileFor(Widen(old)).c_str(), HostFileFor(Widen(editName)).c_str(), MOVEFILE_REPLACE_EXISTING);
					if(cfg.profiles[editIdx].account_id_b64 != acc) DeleteFileW(HostFileFor(Widen(editName)).c_str()); // new account: link again
					cfg.profiles[editIdx] = { editName, acc };
				}
				else if(CanAdd()) cfg.profiles.push_back({ editName, acc });
				SaveConfig(cfg);
				AppLog("Saved profile %s", editName.c_str());
				GoProfiles();
				break;
			}
			case ED_DELETE:
				if(editIdx >= 0)
				{
					DeleteFileW(HostFileFor(Widen(cfg.profiles[editIdx].name)).c_str());
					AppLog("Deleted profile %s", cfg.profiles[editIdx].name.c_str());
					cfg.profiles.erase(cfg.profiles.begin() + editIdx);
					SaveConfig(cfg);
					profileCursor = 0;
				}
				GoProfiles();
				break;
			default: GoProfiles(); break;
		}
	}

	// ---- on-screen keyboard
	std::string kbBuf, kbTitle;
	std::string *kbTarget = nullptr;
	int kbMode = 0;            // 0 text, 1 IP address
	int kbRow = 1, kbCol = 0;
	bool kbShift = false;
	size_t kbMax = 16;
	State kbBack = State::ProfileSelect;
	State helpBack = State::ProfileSelect;

	static const std::vector<std::string> &KbRows(int mode)
	{
		static const std::vector<std::string> text = { "1234567890", "qwertyuiop", "asdfghjkl=", "zxcvbnm+/-" };
		static const std::vector<std::string> ip = { "123", "456", "789", ".0" };
		return mode == 1 ? ip : text;
	}
	static const std::vector<std::string> &KbActions(int mode)
	{
		static const std::vector<std::string> text = { "Shift", "Space", "Delete", "Done" };
		static const std::vector<std::string> ip = { "Delete", "Done" };
		return mode == 1 ? ip : text;
	}
	int KbRowCount() { return (int)KbRows(kbMode).size() + 1; }
	int KbRowLen(int r) { const auto &rows = KbRows(kbMode); return r < (int)rows.size() ? (int)rows[r].size() : (int)KbActions(kbMode).size(); }

	void OpenKeyboard(std::string *target, const std::string &title, int mode, size_t maxLen, State back)
	{
		kbTarget = target; kbBuf = *target; kbTitle = title; kbMode = mode; kbMax = maxLen; kbBack = back;
		kbShift = false; kbRow = mode == 1 ? 0 : 1; kbCol = 0;
		state = State::Keyboard;
	}

	void KbDone()
	{
		std::string v = kbBuf;
		while(!v.empty() && v.back() == ' ') v.pop_back();
		while(!v.empty() && v.front() == ' ') v.erase(v.begin());
		*kbTarget = v;
		if(kbTarget == &cfg.host) { SaveConfig(cfg); AppLog("Console IP set to %s", cfg.host.c_str()); }
		if(kbBack == State::ProfileSelect) GoProfiles(); else state = kbBack;
	}

	void KbType(char c)
	{
		if(kbBuf.size() >= kbMax) return;
		if(kbShift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
		kbBuf += c;
	}

	void KbPress()
	{
		const auto &rows = KbRows(kbMode);
		if(kbRow < (int)rows.size()) { KbType(rows[kbRow][kbCol]); return; }
		const std::string &a = KbActions(kbMode)[kbCol];
		if(a == "Shift") kbShift = !kbShift;
		else if(a == "Space") KbType(' ');
		else if(a == "Delete") { if(!kbBuf.empty()) kbBuf.pop_back(); }
		else if(a == "Done") KbDone();
	}


	void Fail(const std::string &m) { statusMsg = m; AppLog("ERROR: %s", m.c_str()); state = State::Error; }

	void StartRegist()
	{
		uint32_t pin = 0;
		for(int i = 0; i < 8; i++) pin = pin * 10 + pinDigits[i];
		ChiakiRegistInfo info{};
		info.target = CHIAKI_TARGET_PS5_1;
		info.host = hostStr.c_str();
		info.broadcast = false;
		info.psn_online_id = nullptr;
		memcpy(info.psn_account_id, accountId, sizeof(accountId));
		info.pin = pin;
		info.console_pin = 0;
		info.holepunch_info = nullptr;
		info.rudp = nullptr;
		state = State::Registering; statusMsg = "Registering with PS5...";
		ChiakiErrorCode err = chiaki_regist_start(&regist, &log, &info, [](ChiakiRegistEvent *ev, void *user) {
			auto self = (ChiakiXbox *)user;
			if(ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS)
			{
				self->host = *ev->registered_host;
				SaveHost(&self->host);
				self->linkedCache.clear();
				AppLog("Registered with %s", self->host.server_nickname);
				self->wantConnect = true;
			}
			else
			{
				self->statusMsg = "Registration failed - check the PIN and account ID, then try again";
				self->state = State::PinEntry;
			}
			self->registDone = true;
		}, this);
		if(err != CHIAKI_ERR_SUCCESS) Fail(std::string("regist start failed: ") + chiaki_error_string(err));
		else registActive = true;
	}
	std::atomic<bool> wantConnect{false}, registDone{false}, resFallback{false};

	void Connect()
	{
		state = State::Connecting; statusMsg = "Connecting to PS5...";
		// wake it up in case it's in rest mode
		uint64_t cred = strtoull(std::string(host.rp_regist_key, strnlen(host.rp_regist_key, sizeof(host.rp_regist_key))).c_str(), nullptr, 16);
		chiaki_discovery_wakeup(&log, nullptr, hostStr.c_str(), cred, true);

		bool hevc = !setH264 && DecoderAvailable(MFVideoFormat_HEVC);
		bool hdr = false; // HDR off for now (picture was too bright); revisit later
		hdrRequested = hdr;
		decoderSubtype = hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
		AppLog("Video codec: %s", hdr ? "HEVC HDR (10-bit)" : hevc ? "HEVC (H.265)" : "H.264");
		if(!CreateDecoder()) { Fail("Could not start the video decoder"); return; }

		ChiakiConnectInfo ci{};
		ci.ps5 = true;
		ci.host = hostStr.c_str();
		memcpy(ci.regist_key, host.rp_regist_key, sizeof(ci.regist_key));
		memcpy(ci.morning, host.rp_key, sizeof(ci.morning));
		bool fg = setFramegen && fgReady;
		chiaki_connect_video_profile_preset(&ci.video_profile,
			setRes == 0 ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p : CHIAKI_VIDEO_RESOLUTION_PRESET_1080p,
			fg ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60);
		streamIs30 = fg;
		fgHavePrev = false; fgShowRealNext = false; seqSeen = frameSeq; haveShown = false; lastShown = nullptr;
		AppLog("Connect: %ls %s %d kbps, frame gen %s", ResName(setRes), hevc ? "HEVC" : "H.264", kBitrates[setBitrate], fg ? "ON (PS5 30fps -> 60fps)" : "off");
		ci.video_profile.codec = hdr ? CHIAKI_CODEC_H265_HDR : hevc ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
		ci.video_profile.bitrate = kBitrates[setBitrate]; // kbps (chiaki default for 1080p is 15000)
		ci.video_profile_auto_downgrade = true;
		ci.enable_keyboard = false;
		ci.enable_dualsense = true;
		ci.auto_regist = false;
		ci.holepunch_session = nullptr;
		ci.rudp_sock = nullptr;
		memcpy(ci.psn_account_id, accountId, sizeof(accountId));
		ci.packet_loss_max = 0.05;
		ci.enable_idr_on_fec_failure = true;

		ChiakiErrorCode err = chiaki_session_init(&session, &ci, &log);
		if(err != CHIAKI_ERR_SUCCESS) { Fail(std::string("session init failed: ") + chiaki_error_string(err)); return; }

		chiaki_opus_decoder_init(&opus, &log);
		chiaki_opus_decoder_set_cb(&opus,
			[](uint32_t ch, uint32_t rate, void *u) { ((ChiakiXbox *)u)->OnAudioSettings(ch, rate); },
			[](int16_t *b, size_t n, void *u) { ((ChiakiXbox *)u)->OnAudioFrame(b, n); }, this);
		ChiakiAudioSink sink; chiaki_opus_decoder_get_sink(&opus, &sink);
		chiaki_session_set_audio_sink(&session, &sink);

		chiaki_session_set_video_sample_cb(&session, [](uint8_t *buf, size_t size, int32_t, bool, void *u) -> bool {
			((ChiakiXbox *)u)->DecodeAndPresent(buf, size);
			return true;
		}, this);

		chiaki_session_set_event_cb(&session, [](ChiakiEvent *ev, void *u) {
			auto self = (ChiakiXbox *)u;
			switch(ev->type)
			{
				case CHIAKI_EVENT_CONNECTED: AppLog("Connected!"); self->state = State::Streaming; break;
				case CHIAKI_EVENT_RUMBLE: self->Rumble(ev->rumble.left, ev->rumble.right); break;
				case CHIAKI_EVENT_LOGIN_PIN_REQUEST: AppLog("PS5 asked for a login PIN (not supported yet)"); break;
				case CHIAKI_EVENT_QUIT:
					self->statusMsg = std::string("Disconnected: ") + chiaki_quit_reason_string(ev->quit.reason) +
						(ev->quit.reason_str ? std::string(" - ") + ev->quit.reason_str : "");
					AppLog("%s", self->statusMsg.c_str());
					if(ev->quit.reason_str && strstr(ev->quit.reason_str, "AvCap") && self->setRes != 1)
						self->resFallback = true;   // PS5 refused this resolution -> retry at 1080p
					self->state = State::Error;
					break;
				default: break;
			}
		}, this);

		err = chiaki_session_start(&session);
		if(err != CHIAKI_ERR_SUCCESS) { Fail(std::string("session start failed: ") + chiaki_error_string(err)); return; }
		sessionActive = true;
	}

	void StopSession()
	{
		if(!sessionActive) return;
		chiaki_session_stop(&session);
		chiaki_session_join(&session);
		chiaki_session_fini(&session);
		chiaki_opus_decoder_fini(&opus);
		sessionActive = false;
	}


	void Rumble(uint8_t left, uint8_t right)
	{
		auto pads = Gamepad::Gamepads();
		if(pads.Size() == 0) return;
		GamepadVibration v{}; v.LeftMotor = left / 255.0; v.RightMotor = right / 255.0;
		pads.GetAt(0).Vibration(v);
	}
	// ------------------------------------------------------------ input
	// Streaming controls (same idea as chiaki4ps5, Xbox Menu = PS Options):
	//   hold Menu + D-pad Up   = settings menu       hold Menu + D-pad Down = PS button
	//   tap Menu               = Options             View                   = touchpad click
	bool menuHeld = false, menuUsed = false, holdUntilRelease = false;
	int psPulseFrames = 0, optionsPulseFrames = 0;

	void PollInput()
	{
		if(resFallback.exchange(false) && state.load() == State::Error)
		{
			AppLog("PS5 can't stream %ls here, falling back to 1080p", ResName(setRes));
			StopSession(); setRes = 1; SaveSettings();
			if(LoadHost(&host)) { state = State::Connecting; statusMsg = "Your PS5 can't stream that resolution - reconnecting at 1080p"; Connect(); }
			return;
		}
		auto pads = Gamepad::Gamepads();
		if(pads.Size() == 0) return;
		GamepadReading r = pads.GetAt(0).GetCurrentReading();
		auto B = r.Buttons;
		auto has = [&](GamepadButtons b) { return (B & b) == b; };
		uint64_t now = (uint64_t)B;
		uint64_t prev = lastButtons;
		lastButtons = now;
		auto pressed = [=](GamepadButtons b) { return ((uint64_t)B & (uint64_t)b) == (uint64_t)b && !((prev & (uint64_t)b) == (uint64_t)b); };

		switch(state.load())
		{
			case State::ProfileSelect:
			{
				int n = MainItems();
				if(pressed(GamepadButtons::DPadUp)) profileCursor = (profileCursor + n - 1) % n;
				if(pressed(GamepadButtons::DPadDown)) profileCursor = (profileCursor + 1) % n;
				if(pressed(GamepadButtons::A)) MainActivate();
				if(pressed(GamepadButtons::Y) && profileCursor < (int)cfg.profiles.size()) OpenEditor(profileCursor);
				if(profileCursor >= (int)cfg.profiles.size())
				{
					// toolbar buttons: Left/Right move between them
					int first = (int)cfg.profiles.size();
					if(pressed(GamepadButtons::DPadLeft) && profileCursor > first) profileCursor--;
					if(pressed(GamepadButtons::DPadRight) && profileCursor < MainItems() - 1) profileCursor++;
				}
				return;
			}
			case State::EditProfile:
			{
				int n = EditItems();
				if(pressed(GamepadButtons::DPadUp)) editSel = (editSel + n - 1) % n;
				if(pressed(GamepadButtons::DPadDown)) editSel = (editSel + 1) % n;
				if(pressed(GamepadButtons::A)) EditActivate();
				if(pressed(GamepadButtons::B)) GoProfiles();
				return;
			}
			case State::Keyboard:
			{
				int rows = KbRowCount();
				if(pressed(GamepadButtons::DPadUp)) { kbRow = (kbRow + rows - 1) % rows; kbCol = std::min(kbCol, KbRowLen(kbRow) - 1); }
				if(pressed(GamepadButtons::DPadDown)) { kbRow = (kbRow + 1) % rows; kbCol = std::min(kbCol, KbRowLen(kbRow) - 1); }
				if(pressed(GamepadButtons::DPadLeft)) kbCol = (kbCol + KbRowLen(kbRow) - 1) % KbRowLen(kbRow);
				if(pressed(GamepadButtons::DPadRight)) kbCol = (kbCol + 1) % KbRowLen(kbRow);
				if(pressed(GamepadButtons::A)) KbPress();
				if(pressed(GamepadButtons::X)) { if(!kbBuf.empty()) kbBuf.pop_back(); }
				if(pressed(GamepadButtons::Y) && kbMode == 0) KbType(' ');
				if((pressed(GamepadButtons::LeftShoulder) || pressed(GamepadButtons::RightShoulder)) && kbMode == 0) kbShift = !kbShift;
				if(pressed(GamepadButtons::Menu)) KbDone();
				if(pressed(GamepadButtons::B)) { if(kbBack == State::ProfileSelect) GoProfiles(); else state = kbBack; }
				return;
			}
			case State::Help:
				if(pressed(GamepadButtons::A) || pressed(GamepadButtons::B)) { if(helpBack == State::ProfileSelect) GoProfiles(); else state = helpBack; }
				return;
			case State::PinEntry:
				if(pressed(GamepadButtons::B)) { GoProfiles(); return; }
				if(pressed(GamepadButtons::DPadLeft)) pinCursor = (pinCursor + 7) % 8;
				if(pressed(GamepadButtons::DPadRight)) pinCursor = (pinCursor + 1) % 8;
				if(pressed(GamepadButtons::DPadUp)) pinDigits[pinCursor] = (pinDigits[pinCursor] + 1) % 10;
				if(pressed(GamepadButtons::DPadDown)) pinDigits[pinCursor] = (pinDigits[pinCursor] + 9) % 10;
				if(pressed(GamepadButtons::A)) StartRegist();
				return;
			case State::Error:
				if(pressed(GamepadButtons::A)) { StopSession(); if(LoadHost(&host)) Connect(); else GoProfiles(); }
				if(pressed(GamepadButtons::B)) { StopSession(); GoProfiles(); }
				if(pressed(GamepadButtons::Y))
				{
					StopSession();
					DeleteFileW(HostFile().c_str());
					if(g_profile_name == L"MAIN") DeleteFileW((g_local_dir + L"\\registered_host.bin").c_str());
					memset(pinDigits, 0, sizeof(pinDigits)); pinCursor = 0;
					state = State::PinEntry; statusMsg = "Enter the new PIN from your PS5";
				}
				return;
			case State::Connecting:
				if(pressed(GamepadButtons::B)) { StopSession(); GoProfiles(); }
				return;
			case State::Streaming: break;
			default: return;
		}
		if(!sessionActive) return;

		ChiakiControllerState cs; chiaki_controller_state_set_idle(&cs);
		bool menuBtn = has(GamepadButtons::Menu);

		// Menu combos
		if(menuBtn && !menuHeld) menuUsed = false;
		if(menuBtn && pressed(GamepadButtons::DPadUp))
		{
			menuOpen = !menuOpen; menuUsed = true; holdUntilRelease = true; menuCursor = 0;
			AppLog(menuOpen ? "Menu opened (Menu+Up)" : "Menu closed (Menu+Up)");
		}
		if(menuBtn && pressed(GamepadButtons::DPadDown) && !menuOpen) { psPulseFrames = 12; menuUsed = true; AppLog("PS button sent (Menu+Down)"); }
		if(!menuBtn && menuHeld && !menuUsed && !menuOpen) optionsPulseFrames = 6; // a plain tap of Menu = Options
		menuHeld = menuBtn;

		if(menuOpen)
		{
			if(!holdUntilRelease || B == GamepadButtons::None) holdUntilRelease = false;
			if(!holdUntilRelease && !menuBtn) MenuInput(pressed);
			// don't send game input while the menu is open (PS pulse from the menu still goes through)
		}
		else if(holdUntilRelease)
		{
			if(B == GamepadButtons::None) holdUntilRelease = false;
		}
		else
		{
			uint32_t b = 0;
			if(has(GamepadButtons::A)) b |= CHIAKI_CONTROLLER_BUTTON_CROSS;
			if(has(GamepadButtons::B)) b |= CHIAKI_CONTROLLER_BUTTON_MOON;
			if(has(GamepadButtons::X)) b |= CHIAKI_CONTROLLER_BUTTON_BOX;
			if(has(GamepadButtons::Y)) b |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
			if(!menuBtn)
			{
				if(has(GamepadButtons::DPadLeft)) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
				if(has(GamepadButtons::DPadRight)) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
				if(has(GamepadButtons::DPadUp)) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
				if(has(GamepadButtons::DPadDown)) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
			}
			if(has(GamepadButtons::LeftShoulder)) b |= CHIAKI_CONTROLLER_BUTTON_L1;
			if(has(GamepadButtons::RightShoulder)) b |= CHIAKI_CONTROLLER_BUTTON_R1;
			if(has(GamepadButtons::LeftThumbstick)) b |= CHIAKI_CONTROLLER_BUTTON_L3;
			if(has(GamepadButtons::RightThumbstick)) b |= CHIAKI_CONTROLLER_BUTTON_R3;
			if(has(GamepadButtons::View)) b |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
			cs.buttons = b;
			cs.l2_state = (uint8_t)(r.LeftTrigger * 255.0);
			cs.r2_state = (uint8_t)(r.RightTrigger * 255.0);
			cs.left_x = (int16_t)(r.LeftThumbstickX * 32767.0);
			cs.left_y = (int16_t)(-r.LeftThumbstickY * 32767.0);
			cs.right_x = (int16_t)(r.RightThumbstickX * 32767.0);
			cs.right_y = (int16_t)(-r.RightThumbstickY * 32767.0);
		}
		if(optionsPulseFrames > 0) { optionsPulseFrames--; cs.buttons |= CHIAKI_CONTROLLER_BUTTON_OPTIONS; }
		if(psPulseFrames > 0) { psPulseFrames--; cs.buttons |= CHIAKI_CONTROLLER_BUTTON_PS; }
		if(sessionActive) chiaki_session_set_controller_state(&session, &cs);
	}


	// ------------------------------------------------------------ HDR output
	static bool QueryHdrCapable()
	{
		try
		{
			auto hdmi = HdmiDisplayInformation::GetForCurrentView();
			if(!hdmi) return false;
			for(auto const &m : hdmi.GetSupportedDisplayModes())
				if(m.IsSmpte2084Supported()) return true;
		}
		catch(...) {}
		return false;
	}

	void SetHdrOutput(bool on)
	{
		if(on == hdrOutputActive) return;
		// switch the TV mode (HDMI) — async, the console handles it
		try
		{
			auto hdmi = HdmiDisplayInformation::GetForCurrentView();
			if(on)
			{
				auto cur = hdmi.GetCurrentDisplayMode();
				HdmiDisplayMode best{ nullptr };
				for(auto const &m : hdmi.GetSupportedDisplayModes())
				{
					if(!m.IsSmpte2084Supported()) continue;
					if(m.ResolutionWidthInRawPixels() == cur.ResolutionWidthInRawPixels() &&
						m.ResolutionHeightInRawPixels() == cur.ResolutionHeightInRawPixels() &&
						std::abs(m.RefreshRate() - cur.RefreshRate()) < 1.0) { best = m; break; }
					if(!best) best = m;
				}
				int total = 0, hdrModes = 0;
				for(auto const &m : hdmi.GetSupportedDisplayModes()) { total++; if(m.IsSmpte2084Supported()) hdrModes++; }
				AppLog("HDMI: current %ux%u @ %.2fHz, %d modes, %d with HDR10", cur.ResolutionWidthInRawPixels(),
					cur.ResolutionHeightInRawPixels(), cur.RefreshRate(), total, hdrModes);
				if(!best) best = cur; // force: ask for HDR10 on the current mode
				hdmi.RequestSetCurrentDisplayModeAsync(best, HdmiDisplayHdrOption::Eotf2084).Completed(
					[](auto const &op, winrt::Windows::Foundation::AsyncStatus st) {
						bool ok = st == winrt::Windows::Foundation::AsyncStatus::Completed && op.GetResults();
						AppLog("TV HDR10 mode switch: %s", ok ? "accepted" : "refused by Xbox");
					});
			}
			else hdmi.SetDefaultDisplayModeAsync();
		}
		catch(winrt::hresult_error const &e) { AppLog("HDR mode switch failed: %ls", e.message().c_str()); if(on) return; }

		std::lock_guard<std::mutex> l(frameMutex);
		ctx->ClearState();
		d2dctx->SetTarget(nullptr);
		d2dtarget.Reset(); vpout.Reset(); vpin.Reset(); vp.Reset(); vpenum.Reset(); backbuf.Reset();
		ctx->Flush();
		HRESULT hr = swap->ResizeBuffers(2, OUT_W, OUT_H, on ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
		if(FAILED(hr)) AppLog("ResizeBuffers failed 0x%08x", hr);
		swap->GetBuffer(0, IID_PPV_ARGS(&backbuf));
		ComPtr<IDXGISwapChain3> sc3;
		if(SUCCEEDED(swap.As(&sc3)))
			sc3->SetColorSpace1(on ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		hdrOutputActive = on;
		if(!on)
		{
			ComPtr<IDXGISurface> surf; backbuf.As(&surf);
			D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
				D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
			d2dctx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &d2dtarget);
			d2dctx->SetTarget(d2dtarget.Get());
		}
		if(frameTex) SetupVideoProcessor();
		AppLog("HDR output %s", on ? "ON (HDR10)" : "off");
	}

	// ------------------------------------------------------------ settings (same set as chiaki4ps5)
	int setRestore = 1;   // detail restore: 0 off, 1 low, 2 high
	int setDeband = 1;    // 0/1
	int setSharpen = 4;   // 0..10
	int setBoost = 1;     // colour boost: 0 off, 1 low, 2 high
	int setFramegen = 0;  // 0 off, 1 30 -> 60
	int setFps = 0;       // FPS counter
	int setRes = 1;       // 0 720p, 1 1080p
	int setBitrate = 3;   // index into kBitrates
	int setH264 = 0;      // 0 HEVC, 1 H.264
	static constexpr int kBitrates[6] = { 10000, 15000, 20000, 30000, 40000, 50000 };
	static const wchar_t *ResName(int r) { static const wchar_t *n[] = { L"720p", L"1080p" }; return n[r & 1]; }

	static int SettingInt(const std::string &t, const char *key, int def, int lo, int hi)
	{
		std::string k = std::string("\n") + key + "=";
		size_t p = ("\n" + t).find(k);
		if(p == std::string::npos) return def;
		int v = atoi(t.c_str() + p + strlen(key) + 1);
		return v < lo || v > hi ? def : v;
	}
	void LoadSettings()
	{
		std::string t = ReadFileStr(g_local_dir + L"\\settings.txt");
		setRestore = SettingInt(t, "restore", 1, 0, 2);
		setDeband = SettingInt(t, "deband", 1, 0, 1);
		setSharpen = SettingInt(t, "sharpen", 4, 0, 10);
		setBoost = SettingInt(t, "boost", 1, 0, 2);
		setFramegen = SettingInt(t, "framegen", 0, 0, 1);
		setFps = SettingInt(t, "fps", SettingInt(t, "stats", 0, 0, 1), 0, 1);
		setRes = SettingInt(t, "res", 1, 0, 1);
		setBitrate = SettingInt(t, "bitrate", 3, 0, 5);
		setH264 = SettingInt(t, "h264", SettingInt(t, "hevc", 1, 0, 1) ? 0 : 1, 0, 1);
	}
	void SaveSettings()
	{
		FILE *f = _wfopen((g_local_dir + L"\\settings.txt").c_str(), L"w");
		if(!f) return;
		fprintf(f, "restore=%d\ndeband=%d\nsharpen=%d\nboost=%d\nframegen=%d\nfps=%d\nres=%d\nbitrate=%d\nh264=%d\n",
			setRestore, setDeband, setSharpen, setBoost, setFramegen, setFps, setRes, setBitrate, setH264);
		fclose(f);
		filtersDirty = true;
	}

	// ------------------------------------------------------------ GPU pipeline: frame gen + picture filters
	// Every decoded frame: video processor -> RGB texture. With frame gen on (30fps stream) the GPU builds an
	// in-between frame, shown first; the real frame follows on the next vsync. Whatever is shown goes through the
	// picture filters into the back buffer.
	std::atomic<uint64_t> frameSeq{0};
	uint64_t seqSeen = 0;
	int fgCur = 0;
	bool fgHavePrev = false, fgReady = false, fgShowRealNext = false, filtersDirty = true, haveShown = false;
	bool streamIs30 = false;
	static const UINT QW = OUT_W / 4, QH = OUT_H / 4, BW = (QW + 3) / 4, BH = (QH + 3) / 4;
	ComPtr<ID3D11Texture2D> fgRgb[2], fgLuma[2], fgMvRaw, fgMv, fgOut, filtOut;
	ComPtr<ID3D11ShaderResourceView> fgRgbSrv[2], fgLumaSrv[2], fgMvRawSrv, fgMvSrv, fgOutSrv;
	ComPtr<ID3D11UnorderedAccessView> fgLumaUav[2], fgMvRawUav, fgMvUav, fgOutUav, filtOutUav;
	ID3D11ShaderResourceView *lastShown = nullptr;
	ComPtr<ID3D11VideoProcessorOutputView> fgVpOut[2];
	ComPtr<ID3D11ComputeShader> csLuma, csMotion, csSmooth, csInterp, csFilter;
	ComPtr<ID3D11Buffer> fgCb, filterCb;
	ComPtr<ID3D11SamplerState> fgSampler;

	bool MakeTex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, ComPtr<ID3D11Texture2D> &t)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
		d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
		return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t));
	}

	void InitFrameGen()
	{
		const UINT SRV = D3D11_BIND_SHADER_RESOURCE, UAV = D3D11_BIND_UNORDERED_ACCESS;
		bool ok = true;
		for(int i = 0; i < 2; i++)
		{
			ok = ok && MakeTex(OUT_W, OUT_H, DXGI_FORMAT_R8G8B8A8_UNORM, SRV | D3D11_BIND_RENDER_TARGET, fgRgb[i]);
			ok = ok && SUCCEEDED(dev->CreateShaderResourceView(fgRgb[i].Get(), nullptr, &fgRgbSrv[i]));
			ok = ok && MakeTex(QW, QH, DXGI_FORMAT_R32_FLOAT, SRV | UAV, fgLuma[i]);
			ok = ok && SUCCEEDED(dev->CreateShaderResourceView(fgLuma[i].Get(), nullptr, &fgLumaSrv[i]));
			ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(fgLuma[i].Get(), nullptr, &fgLumaUav[i]));
		}
		ok = ok && MakeTex(BW, BH, DXGI_FORMAT_R16G16_FLOAT, SRV | UAV, fgMvRaw);
		ok = ok && SUCCEEDED(dev->CreateShaderResourceView(fgMvRaw.Get(), nullptr, &fgMvRawSrv));
		ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(fgMvRaw.Get(), nullptr, &fgMvRawUav));
		ok = ok && MakeTex(BW, BH, DXGI_FORMAT_R16G16_FLOAT, SRV | UAV, fgMv);
		ok = ok && SUCCEEDED(dev->CreateShaderResourceView(fgMv.Get(), nullptr, &fgMvSrv));
		ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(fgMv.Get(), nullptr, &fgMvUav));
		ok = ok && MakeTex(OUT_W, OUT_H, DXGI_FORMAT_R8G8B8A8_UNORM, SRV | UAV, fgOut);
		ok = ok && SUCCEEDED(dev->CreateShaderResourceView(fgOut.Get(), nullptr, &fgOutSrv));
		ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(fgOut.Get(), nullptr, &fgOutUav));
		ok = ok && MakeTex(OUT_W, OUT_H, DXGI_FORMAT_R8G8B8A8_UNORM, UAV, filtOut);
		ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(filtOut.Get(), nullptr, &filtOutUav));

		ok = ok && SUCCEEDED(dev->CreateComputeShader(g_fg_luma, sizeof(g_fg_luma), nullptr, &csLuma));
		ok = ok && SUCCEEDED(dev->CreateComputeShader(g_fg_motion, sizeof(g_fg_motion), nullptr, &csMotion));
		ok = ok && SUCCEEDED(dev->CreateComputeShader(g_fg_smooth, sizeof(g_fg_smooth), nullptr, &csSmooth));
		ok = ok && SUCCEEDED(dev->CreateComputeShader(g_fg_interp, sizeof(g_fg_interp), nullptr, &csInterp));
		ok = ok && SUCCEEDED(dev->CreateComputeShader(g_fg_filter, sizeof(g_fg_filter), nullptr, &csFilter));

		struct { UINT fw, fh, qw, qh, bw, bh; float k, pad; } c = { OUT_W, OUT_H, QW, QH, BW, BH, 6.0f, 0 };
		D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(c); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = &c;
		ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, &sd, &fgCb));
		D3D11_BUFFER_DESC fb{}; fb.ByteWidth = 32; fb.Usage = D3D11_USAGE_DEFAULT; fb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ok = ok && SUCCEEDED(dev->CreateBuffer(&fb, nullptr, &filterCb));

		D3D11_SAMPLER_DESC smp{};
		smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		smp.MaxLOD = D3D11_FLOAT32_MAX;
		ok = ok && SUCCEEDED(dev->CreateSamplerState(&smp, &fgSampler));
		fgReady = ok;
		AppLog("GPU pipeline (frame gen + picture filters) %s", ok ? "ready" : "unavailable - using plain video");
	}

	void RunCS(ID3D11ComputeShader *cs, std::initializer_list<ID3D11ShaderResourceView *> srvs, ID3D11UnorderedAccessView *uav, UINT w, UINT h)
	{
		ctx->CSSetShader(cs, nullptr, 0);
		ctx->CSSetShaderResources(0, (UINT)srvs.size(), srvs.begin());
		ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
		ID3D11ShaderResourceView *nullSrv[3] = {};
		ID3D11UnorderedAccessView *nullUav = nullptr;
		ctx->CSSetShaderResources(0, 3, nullSrv);
		ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	}

	uint64_t statFrames = 0, statPresented = 0;
	// Called from Render() with frameMutex held.
	void RenderPipeline()
	{
		ID3D11Buffer *cbs[2] = { fgCb.Get(), filterCb.Get() };
		ctx->CSSetConstantBuffers(0, 2, cbs);
		ID3D11SamplerState *smp = fgSampler.Get();
		ctx->CSSetSamplers(0, 1, &smp);

		bool fgOn = setFramegen && streamIs30;
		ID3D11ShaderResourceView *show = nullptr;
		uint64_t seq = frameSeq;
		if(seq != seqSeen && haveFrame && fgVpOut[0] && fgVpOut[1])
		{
			seqSeen = seq;
			fgCur ^= 1;
			D3D11_VIDEO_PROCESSOR_STREAM st{}; st.Enable = TRUE; st.pInputSurface = vpin.Get();
			vctx->VideoProcessorBlt(vp.Get(), fgVpOut[fgCur].Get(), 0, 1, &st);
			if(fgOn)
			{
				RunCS(csLuma.Get(), { fgRgbSrv[fgCur].Get() }, fgLumaUav[fgCur].Get(), QW, QH);
				if(fgHavePrev)
				{
					int prev = fgCur ^ 1;
					RunCS(csMotion.Get(), { fgLumaSrv[prev].Get(), fgLumaSrv[fgCur].Get() }, fgMvRawUav.Get(), BW, BH);
					RunCS(csSmooth.Get(), { fgMvRawSrv.Get() }, fgMvUav.Get(), BW, BH);
					RunCS(csInterp.Get(), { fgRgbSrv[prev].Get(), fgRgbSrv[fgCur].Get(), fgMvSrv.Get() }, fgOutUav.Get(), OUT_W, OUT_H);
					show = fgOutSrv.Get();
					fgShowRealNext = true;
				}
				else show = fgRgbSrv[fgCur].Get();
				fgHavePrev = true;
			}
			else { show = fgRgbSrv[fgCur].Get(); fgHavePrev = false; fgShowRealNext = false; }
		}
		else if(fgShowRealNext) { show = fgRgbSrv[fgCur].Get(); fgShowRealNext = false; }

		if(show) statPresented++;
		if(!show && filtersDirty && lastShown) show = lastShown;
		if(show)
		{
			struct { float sharpen, deband, restore, boost, time, p0, p1, p2; } fc =
				{ setSharpen / 10.0f, (float)setDeband, (float)setRestore, (float)setBoost, (float)(statPresented % 1000), 0, 0, 0 };
			ctx->UpdateSubresource(filterCb.Get(), 0, nullptr, &fc, 0, 0);
			RunCS(csFilter.Get(), { show }, filtOutUav.Get(), OUT_W, OUT_H);
			lastShown = show;
			filtersDirty = false;
			haveShown = true;
		}
		if(haveShown) ctx->CopyResource(backbuf.Get(), filtOut.Get());
	}

	// ------------------------------------------------------------ stats (FPS counter)
	uint64_t statLastFrames = 0, statLastPresented = 0;
	int statPs5Fps = 0, statShownFps = 0;
	std::chrono::steady_clock::time_point statT0 = std::chrono::steady_clock::now();
	void UpdateStats()
	{
		auto now = std::chrono::steady_clock::now();
		double dt = std::chrono::duration<double>(now - statT0).count();
		if(dt < 1.0) return;
		uint64_t f = frameSeq;
		statPs5Fps = (int)((f - statLastFrames) / dt + 0.5);
		statShownFps = (int)((statPresented - statLastPresented) / dt + 0.5);
		statLastFrames = f; statLastPresented = statPresented; statT0 = now;
	}

	// ------------------------------------------------------------ settings menu (hold Menu + D-pad Up)
	bool menuOpen = false;
	int menuCursor = 0;
	enum MenuItem { MI_RESTORE, MI_DEBAND, MI_SHARPEN, MI_BOOST, MI_FRAMEGEN, MI_FPS, MI_RES, MI_BITRATE, MI_CODEC,
		MI_PS, MI_RECONNECT, MI_DISCONNECT, MI_HELP, MI_CLOSE, MI_COUNT };

	std::wstring MenuLabel(int i)
	{
		static const wchar_t *lvl[] = { L"Off", L"Low", L"High" };
		wchar_t b[160];
		switch(i)
		{
			case MI_RESTORE: swprintf(b, 160, L"Detail restore  < %s >", lvl[setRestore]); break;
			case MI_DEBAND: swprintf(b, 160, L"Deband  < %s >", setDeband ? L"On" : L"Off"); break;
			case MI_SHARPEN: swprintf(b, 160, L"Sharpening  < %d >", setSharpen); break;
			case MI_BOOST: swprintf(b, 160, L"Color boost  < %s >", lvl[setBoost]); break;
			case MI_FRAMEGEN: swprintf(b, 160, L"Frame gen *  < %s >%s", setFramegen ? L"30 -> 60" : L"Off",
				setFramegen && !streamIs30 ? L"  (reconnect)" : L""); break;
			case MI_FPS: swprintf(b, 160, L"FPS counter  < %s >", setFps ? L"On" : L"Off"); break;
			case MI_RES: swprintf(b, 160, L"Resolution *  < %s >", ResName(setRes)); break;
			case MI_BITRATE: swprintf(b, 160, L"Bitrate *  < %d Mbps >", kBitrates[setBitrate] / 1000); break;
			case MI_CODEC: swprintf(b, 160, L"Codec *  < %s >", setH264 ? L"H.264" : L"HEVC"); break;
			case MI_PS: swprintf(b, 160, L"Press PS button  (or Menu+Down)"); break;
			case MI_RECONNECT: swprintf(b, 160, L"Reconnect now (applies *)"); break;
			case MI_DISCONNECT: swprintf(b, 160, L"Disconnect"); break;
			case MI_HELP: swprintf(b, 160, L"Controls & help"); break;
			default: swprintf(b, 160, L"Close menu"); break;
		}
		return b;
	}

	void MenuAdjust(int dir)
	{
		auto cyc = [&](int &v, int n) { v = (v + dir + n) % n; };
		auto clampStep = [&](int &v, int lo, int hi) { v = std::min(hi, std::max(lo, v + dir)); };
		switch(menuCursor)
		{
			case MI_RESTORE: cyc(setRestore, 3); break;
			case MI_DEBAND: cyc(setDeband, 2); break;
			case MI_SHARPEN: clampStep(setSharpen, 0, 10); break;
			case MI_BOOST: cyc(setBoost, 3); break;
			case MI_FRAMEGEN: cyc(setFramegen, 2); fgHavePrev = false; fgShowRealNext = false; break;
			case MI_FPS: cyc(setFps, 2); break;
			case MI_RES: cyc(setRes, 2); break;
			case MI_BITRATE: clampStep(setBitrate, 0, 5); break;
			case MI_CODEC: cyc(setH264, 2); break;
			default: return;
		}
		SaveSettings();
	}

	void MenuInput(const std::function<bool(GamepadButtons)> &pressed)
	{
		if(pressed(GamepadButtons::DPadUp)) menuCursor = (menuCursor + MI_COUNT - 1) % MI_COUNT;
		if(pressed(GamepadButtons::DPadDown)) menuCursor = (menuCursor + 1) % MI_COUNT;
		if(pressed(GamepadButtons::DPadLeft)) MenuAdjust(-1);
		if(pressed(GamepadButtons::DPadRight)) MenuAdjust(1);
		if(pressed(GamepadButtons::B)) { menuOpen = false; holdUntilRelease = true; return; }
		if(!pressed(GamepadButtons::A)) return;
		holdUntilRelease = true;
		switch(menuCursor)
		{
			case MI_PS: menuOpen = false; psPulseFrames = 12; AppLog("PS button sent (menu)"); break;
			case MI_RECONNECT: menuOpen = false; StopSession(); Connect(); break;
			case MI_DISCONNECT: menuOpen = false; StopSession(); GoProfiles(); break;
			case MI_HELP: menuOpen = false; helpBack = State::Streaming; state = State::Help; break;
			case MI_CLOSE: menuOpen = false; break;
			default: MenuAdjust(1); break;
		}
	}

	// ------------------------------------------------------------ render
	void Render()
	{
		if(frameWait) WaitForSingleObjectEx(frameWait, 100, TRUE);
		const float clear[4] = { 0.04f, 0.04f, 0.08f, 1 };
		ComPtr<ID3D11RenderTargetView> rtv;
		dev->CreateRenderTargetView(backbuf.Get(), nullptr, &rtv);
		ctx->ClearRenderTargetView(rtv.Get(), clear);

		if(state == State::Streaming || (state == State::Help && helpBack == State::Streaming))
		{
			std::lock_guard<std::mutex> l(frameMutex);
			if(fgReady && vp && vpin)
				RenderPipeline();
			else if(haveFrame && vp && vpin && vpout)
			{
				D3D11_VIDEO_PROCESSOR_STREAM st{}; st.Enable = TRUE; st.pInputSurface = vpin.Get();
				vctx->VideoProcessorBlt(vp.Get(), vpout.Get(), 0, 1, &st);
				uint64_t seq = frameSeq;
				if(seq != seqSeen) { seqSeen = seq; statPresented++; }
			}
		}

		if(!hdrOutputActive)
		{
			d2dctx->BeginDraw();
			if(state == State::Streaming) DrawOverlay();
			else DrawUi();
			d2dctx->EndDraw();
		}
		swap->Present(1, 0);
	}

	// ------------------------------------------------------------ UI (chiaki-ng look, same layout as chiaki4ps5)
	// Layout is written in 960x540 units (x2 = screen pixels), like the PS5 app.
	static constexpr float U = 2.0f;
	ComPtr<IDWriteTextFormat> uiFont[4];
	ComPtr<ID2D1SolidColorBrush> uiBrush;
	ComPtr<ID2D1Bitmap1> imgLogo, imgConsole;
	std::vector<bool> linkedCache;

	static D2D1_COLOR_F RGBA(int r, int g, int b, int a = 255) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }
	static D2D1_COLOR_F CBg() { return RGBA(48, 48, 48); }
	static D2D1_COLOR_F CBar() { return RGBA(33, 33, 33); }
	static D2D1_COLOR_F CSurface() { return RGBA(66, 66, 66); }
	static D2D1_COLOR_F CAccent() { return RGBA(0, 167, 255); }
	static D2D1_COLOR_F CText() { return RGBA(255, 255, 255); }
	static D2D1_COLOR_F CDim() { return RGBA(179, 179, 179); }
	static D2D1_COLOR_F CBtn() { return RGBA(88, 88, 88); }

	ComPtr<ID2D1Bitmap1> LoadPng(const std::wstring &path)
	{
		ComPtr<ID2D1Bitmap1> bmp;
		ComPtr<IWICImagingFactory> wic;
		if(FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return bmp;
		ComPtr<IWICBitmapDecoder> decoder;
		if(FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) return bmp;
		ComPtr<IWICBitmapFrameDecode> frame;
		if(FAILED(decoder->GetFrame(0, &frame))) return bmp;
		ComPtr<IWICFormatConverter> conv;
		wic->CreateFormatConverter(&conv);
		if(FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) return bmp;
		d2dctx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
		return bmp;
	}

	void InitUi()
	{
		const float sizes[4] = { 26.0f, 30.0f, 52.0f, 76.0f }; // index = text scale, in screen pixels
		for(int i = 0; i < 4; i++)
			dwfac->CreateTextFormat(L"Segoe UI", nullptr, i >= 2 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
				DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizes[i], L"en-us", &uiFont[i]);
		d2dctx->CreateSolidColorBrush(CText(), &uiBrush);
		imgLogo = LoadPng(g_install_dir + L"\\Assets\\ui_logo.png");
		imgConsole = LoadPng(g_install_dir + L"\\Assets\\ui_ps5.png");
	}

	ID2D1SolidColorBrush *B(D2D1_COLOR_F c) { uiBrush->SetColor(c); return uiBrush.Get(); }
	void URect(float x, float y, float w, float h, D2D1_COLOR_F c) { d2dctx->FillRectangle(D2D1::RectF(x * U, y * U, (x + w) * U, (y + h) * U), B(c)); }
	void URound(float x, float y, float w, float h, float r, D2D1_COLOR_F c)
	{
		d2dctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x * U, y * U, (x + w) * U, (y + h) * U), r * U, r * U), B(c));
	}
	IDWriteTextFormat *F(int scale) { return uiFont[scale < 1 ? 1 : scale > 3 ? 3 : scale].Get(); }
	float UTextWidth(const std::wstring &s, int scale)
	{
		ComPtr<IDWriteTextLayout> lay;
		if(FAILED(dwfac->CreateTextLayout(s.c_str(), (UINT32)s.size(), F(scale), 4000, 200, &lay))) return 0;
		DWRITE_TEXT_METRICS m; lay->GetMetrics(&m);
		return m.widthIncludingTrailingWhitespace / U;
	}
	void UText(float x, float y, const std::wstring &s, D2D1_COLOR_F c, int scale = 1)
	{
		d2dctx->DrawText(s.c_str(), (UINT32)s.size(), F(scale), D2D1::RectF(x * U, y * U - 4, 1920, 1080), B(c));
	}
	void UTextCentered(float y, const std::wstring &s, D2D1_COLOR_F c, int scale = 1) { UText((960 - UTextWidth(s, scale)) / 2, y, s, c, scale); }
	void UTextWrapped(float y, const std::wstring &s, D2D1_COLOR_F c, int scale, float maxw)
	{
		ComPtr<IDWriteTextLayout> lay;
		if(FAILED(dwfac->CreateTextLayout(s.c_str(), (UINT32)s.size(), F(scale), maxw * U, 400, &lay))) return;
		lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		d2dctx->DrawTextLayout(D2D1::Point2F((960 - maxw) / 2 * U, y * U - 4), lay.Get(), B(c));
	}
	void UImage(ID2D1Bitmap1 *img, float x, float y, float opacity)
	{
		if(!img) return;
		auto sz = img->GetPixelSize();
		d2dctx->DrawBitmap(img, D2D1::RectF(x * U, y * U, x * U + sz.width, y * U + sz.height), opacity);
	}
	float ImgW(ID2D1Bitmap1 *img) { return img ? img->GetPixelSize().width / U : 0; }
	float ImgH(ID2D1Bitmap1 *img) { return img ? img->GetPixelSize().height / U : 0; }

	void UButton(float x, float y, float w, float h, const std::wstring &label, bool focus, D2D1_COLOR_F text = D2D1::ColorF(1, 1, 1, 1))
	{
		URound(x, y, w, h, 6, focus ? CAccent() : CBtn());
		float tw = UTextWidth(label, 1);
		UText(x + (w - tw) / 2, y + (h - 16) / 2, label, text, 1);
	}

	void UField(float x, float y, float w, const std::wstring &label, const std::string &value, const std::wstring &empty, bool focus)
	{
		UText(x, y, label, focus ? CAccent() : CDim(), 1);
		URound(x, y + 18, w, 38, 4, RGBA(255, 255, 255, focus ? 34 : 20));
		URect(x, y + 54, w, focus ? 2.f : 1.f, focus ? CAccent() : RGBA(255, 255, 255, 90));
		UText(x + 12, y + 28, value.empty() ? empty : Widen(value), value.empty() ? CDim() : CText(), 1);
	}

	void UDialog(float x, float y, float w, float h, const std::wstring &title)
	{
		URound(x + 2, y + 4, w, h, 10, RGBA(0, 0, 0, 90));
		URound(x, y, w, h, 10, CSurface());
		UText(x + 24, y + 16, title, CText(), 2);
	}

	void RefreshLinked()
	{
		linkedCache.clear();
		std::wstring keep = g_profile_name;
		for(auto &p : cfg.profiles)
		{
			g_profile_name = Widen(p.name);
			ChiakiRegisteredHost tmp;
			linkedCache.push_back(LoadHost(&tmp));
		}
		g_profile_name = keep;
	}

	void DrawMainPage(bool active)
	{
		URect(0, 0, 960, 540, CBg());
		UImage(imgLogo.Get(), 480 - ImgW(imgLogo.Get()) / 2, 290 - ImgH(imgLogo.Get()) / 2, 0.13f);

		// toolbar
		URect(0, 0, 960, 56, CBar());
		URect(0, 56, 960, 2, RGBA(0, 0, 0, 70));
		UText(20, 12, L"chiaki4xbox", CText(), 2);
		int np = (int)cfg.profiles.size();
		if(CanAdd()) UButton(545, 10, 160, 36, L"+ Add Profile", active && profileCursor == ItemAdd());
		UButton(715, 10, 140, 36, cfg.host.empty() ? L"Console IP" : Widen(cfg.host), active && profileCursor == ItemIp());
		UButton(865, 10, 80, 36, L"Help", active && profileCursor == ItemHelp());

		// the profiles, like chiaki-ng's console list
		if(np == 0)
		{
			UTextCentered(240, L"No profiles yet", CText(), 2);
			UTextCentered(280, cfg.host.empty() ? L"Set your Console IP, then choose + Add Profile at the top"
				: L"Choose + Add Profile at the top to get started", CDim(), 1);
		}
		if((int)linkedCache.size() != np) RefreshLinked();
		float step = np <= 4 ? 100.f : (np <= 5 ? 88.f : 440.f / np);
		for(int i = 0; i < np; i++)
		{
			float y = 62 + i * step;
			bool sel = active && i == profileCursor;
			if(sel) { URect(0, y, 960, step, RGBA(255, 255, 255, 30)); URect(0, y, 5, step, CAccent()); }
			if(i > 0) URect(20, y, 920, 1, RGBA(255, 255, 255, 18));
			UImage(imgConsole.Get(), 30, y + (step - ImgH(imgConsole.Get())) / 2, 1.0f);
			bool linked = linkedCache[i];
			float ty = y + step / 2 - 30;
			UText(220, ty, Widen(cfg.profiles[i].name), CText(), 2);
			UText(220, ty + 34, L"Address: " + (cfg.host.empty() ? std::wstring(L"not set") : Widen(cfg.host)), CDim(), 1);
			UText(220, ty + 52, linked ? L"ID: registered" : L"ID: unregistered", CDim(), 1);
			UText(620, y + step / 2 - 8, linked ? L"State: ready" : L"State: link with PIN", linked ? CText() : CDim(), 1);
		}

		// status bar
		std::wstring hint;
		if(active)
			hint = profileCursor < np ? L"A  Connect      Y  Edit / delete      Up / Down  Move"
				: L"A  Select      Left / Right  Move      Down  Profiles";
		UText(20, 514, hint, CDim(), 1);
		if(active && !statusMsg.empty() && statusMsg != "Who's playing?") UTextCentered(484, Widen(statusMsg), CAccent(), 1);
		UText(905, 514, L"v1.1", CDim(), 1);
		if(!fgReady) UText(560, 514, L"frame gen unavailable", CDim(), 1);
	}

	void DrawEditDialog()
	{
		UDialog(180, 70, 600, 400, editIdx >= 0 ? L"Edit Profile" : L"Add Profile");
		UField(204, 124, 552, L"Name", editName, L"Enter a name", editSel == ED_NAME);
		UField(204, 192, 552, L"PSN Account ID", editAcc, L"base64 (AbCdEfGhIjk=) or number", editSel == ED_ACC);
		UButton(204, 266, editIdx >= 0 ? 268.f : 552.f, 38, L"Save", editSel == ED_SAVE);
		if(editIdx >= 0) UButton(488, 266, 268, 38, L"Delete Profile", editSel == ED_DELETE, editSel == ED_DELETE ? CText() : RGBA(255, 140, 120));
		if(!editMsg.empty()) UTextCentered(318, Widen(editMsg), CAccent(), 1);
		UTextWrapped(344, L"Use the PSN account ID signed in on the PS5 you stream from. Type it as base64 or as the number.", CDim(), 1, 540);
		UTextCentered(444, L"A  Select      B  Back", CDim(), 1);
	}

	void DrawKeyboardDialog()
	{
		UDialog(150, 40, 660, 460, Widen(kbTitle));
		URound(174, 92, 612, 46, 4, RGBA(255, 255, 255, 30));
		URect(174, 136, 612, 2, CAccent());
		UText(188, 98, Widen(kbBuf) + L"_", CText(), 2);
		const auto &rows = KbRows(kbMode);
		for(int r = 0; r <= (int)rows.size(); r++)
		{
			bool act = r == (int)rows.size();
			int len = KbRowLen(r);
			float w = act ? (kbMode == 1 ? 150.f : 132.f) : 54.f, gap = 6;
			float total = len * (w + gap) - gap, x0 = 480 - total / 2, y = 156 + r * 52.f;
			for(int c = 0; c < len; c++)
			{
				bool sel = r == kbRow && c == kbCol;
				float x = x0 + c * (w + gap);
				bool on = act && KbActions(kbMode)[c] == "Shift" && kbShift;
				URound(x, y, w, 44, 6, sel ? CAccent() : (on ? RGBA(0, 110, 170) : CBtn()));
				std::wstring label;
				if(act) label = Widen(KbActions(kbMode)[c]);
				else { char ch = rows[r][c]; if(kbShift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A'); label = std::wstring(1, (wchar_t)ch); }
				float tw = UTextWidth(label, act ? 1 : 2);
				UText(x + (w - tw) / 2, y + (act ? 13 : 6), label, CText(), act ? 1 : 2);
			}
		}
		UTextCentered(474, kbMode == 1 ? L"A  Type      X  Delete      Menu  Done      B  Cancel"
			: L"A  Type    X  Delete    Y  Space    LB/RB  Shift    Menu  Done    B  Cancel", CDim(), 1);
	}

	void DrawHelpDialog()
	{
		UDialog(90, 40, 780, 460, L"Controls & Help");
		const wchar_t *lines[] = {
			L"While streaming",
			L"Hold Menu + press D-pad Up: open / close the settings menu",
			L"Hold Menu + press D-pad Down: PS button on the streamed PS5",
			L"Tap Menu: Options in the game        View: touchpad click",
			L"Settings menu: Up/Down choose, Left/Right change, A select, B close",
			L"",
			L"Setting up",
			L"1. Console IP: the PS5 you stream from (on it: Settings > Network > Connection Status)",
			L"2. + Add Profile: a name and the PSN account ID signed in on that PS5",
			L"3. Pick the profile. On the PS5 open Settings > System > Remote Play >",
			L"    Link Device, and enter the PIN it shows here (only the first time)",
		};
		float y = 92;
		for(const wchar_t *l : lines)
		{
			bool head = !wcscmp(l, L"While streaming") || !wcscmp(l, L"Setting up");
			UText(118, y, l, head ? CAccent() : CText(), 1);
			y += *l ? 28.f : 14.f;
		}
		UTextCentered(474, L"B  Back", CDim(), 1);
	}

	void DrawPinDialog()
	{
		UDialog(160, 100, 640, 330, L"Link Profile");
		UTextWrapped(150, Widen(statusMsg), CDim(), 1, 580);
		for(int i = 0; i < 8; i++)
		{
			float x = 480 - 4 * 68 + i * 68 + 4.f, y = 224;
			URound(x, y, 60, 76, 6, i == pinCursor ? CAccent() : RGBA(255, 255, 255, 26));
			wchar_t d[2] = { (wchar_t)(L'0' + pinDigits[i]), 0 };
			UText(x + 19, y + 10, d, CText(), 3);
		}
		UTextCentered(330, L"Left/Right  Move      Up/Down  Change digit", CDim(), 1);
		UTextCentered(352, L"A  Register      B  Back", CDim(), 1);
	}

	void DrawStatusDialog()
	{
		State st = state;
		const wchar_t *title = st == State::Error ? L"Disconnected" : (st == State::Registering ? L"Registering" : L"Connecting");
		UDialog(220, 170, 520, 200, title);
		UTextWrapped(222, Widen(statusMsg), st == State::Error ? RGBA(255, 150, 130) : CText(), 1, 470);
		if(st == State::Error) UTextCentered(336, L"A  Retry      B  Back to profiles      Y  Re-link", CDim(), 1);
		else
		{
			int phase = (int)(GetTickCount64() / 300) % 4;
			for(int i = 0; i < 3; i++) URound(462 + i * 14.f, 330, 8, 8, 4, i < phase ? CAccent() : RGBA(255, 255, 255, 50));
		}
	}

	void DrawOverlay()
	{
		if(setFps)
		{
			std::wstring f = std::to_wstring(statPs5Fps) + L" fps  (" + std::to_wstring(statShownFps) + L" shown)";
			URound(960 - 190, 8, 182, 22, 11, RGBA(0, 0, 0, 150));
			UText(960 - 184, 10, f, RGBA(120, 255, 140), 1);
		}
		if(menuOpen)
		{
			URound(560, 30, 380, 490, 10, RGBA(48, 48, 48, 235));
			UText(580, 40, L"Settings", CText(), 2);
			URect(580, 74, 340, 2, CAccent());
			for(int i = 0; i < MI_COUNT; i++)
			{
				float y = 84 + i * 27.f;
				if(i == menuCursor) URound(570, y - 5, 360, 26, 5, CAccent());
				UText(584, y - 3, MenuLabel(i), CText(), 1);
			}
			UText(580, 462, L"* applies on reconnect", CDim(), 1);
			UText(580, 480, L"Up/Down  Left/Right  A  B: close", CDim(), 1);
			UText(580, 498, L"Menu+Up: menu   Menu+Down: PS", CAccent(), 1);
		}
	}

	void DrawUi()
	{
		State st = state;
		if(st == State::Help && helpBack == State::Streaming) { URect(0, 0, 960, 540, RGBA(0, 0, 0, 140)); DrawHelpDialog(); return; }
		DrawMainPage(st == State::ProfileSelect);
		if(st != State::ProfileSelect) URect(0, 0, 960, 540, RGBA(0, 0, 0, 140));
		if(st == State::EditProfile) DrawEditDialog();
		else if(st == State::Keyboard) DrawKeyboardDialog();
		else if(st == State::Help) DrawHelpDialog();
		else if(st == State::PinEntry) DrawPinDialog();
		else if(st != State::ProfileSelect) DrawStatusDialog();
	}

	void Tick()
	{
		if(registDone.exchange(false) && registActive) { chiaki_regist_stop(&regist); chiaki_regist_fini(&regist); registActive = false; }
		if(wantConnect.exchange(false)) Connect();
		UpdateStats();
		wantHdrOutput = false;
		if(hdrOutputActive && state != State::Streaming) SetHdrOutput(false);
		PollInput();
		Render();
	}
};


// ---------------------------------------------------------------- CoreApplication glue
struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
	ChiakiXbox x;
	CoreWindow window{ nullptr };
	bool closed = false;

	IFrameworkView CreateView() { return *this; }
	void Initialize(CoreApplicationView const &) {}
	void Load(hstring const &) {}
	void Uninitialize() { x.StopSession(); }

	void SetWindow(CoreWindow const &w)
	{
		window = w;
		w.Closed([this](auto &&, auto &&) { closed = true; });
		// Keep the B button / back from suspending the app
		winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested([](auto &&, BackRequestedEventArgs const &a) { a.Handled(true); });
		g_install_dir = std::wstring(Package::Current().InstalledLocation().Path());
		g_local_dir = std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
		g_log_file = _wfopen((g_local_dir + L"\\chiaki-log.txt").c_str(), L"w");
		x.InitGraphics(w);
		x.InitUi();
		x.hdrCapable = ChiakiXbox::QueryHdrCapable();
		x.InitFrameGen();
		x.LoadSettings();
		x.InitAudio();
	}

	void Run()
	{
		window.Activate();
		x.Start();
		while(!closed)
		{
			window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
			x.Tick();
		}
	}
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	init_apartment();
	CoreApplication::Run(make<App>());
	return 0;
}
