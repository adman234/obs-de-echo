/*
OBS De-Echo
Copyright (C) 2026 adman234

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/util_uint64.h>
#include <plugin-support.h>

#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

#define SETTING_EXECUTABLE "executable"
#define DEFAULT_EXECUTABLE "Discord.exe"

#define CHANNELS 2
#define BUFFER_TIME_100NS (5 * 10000000)
#define PROCESS_POLL_MS 500
#define RETRY_MS 3000

namespace {

class ActivateHandler
	: public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
					      Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
	HANDLE done;

public:
	ActivateHandler() { done = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
	~ActivateHandler() { CloseHandle(done); }

	HANDLE event() const { return done; }

	STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation *) override
	{
		SetEvent(done);
		return S_OK;
	}
};

std::wstring to_wide(const char *utf8)
{
	wchar_t *wide = nullptr;
	os_utf8_to_wcs_ptr(utf8, 0, &wide);
	std::wstring result = wide ? wide : L"";
	bfree(wide);
	return result;
}

std::string to_utf8(const wchar_t *wide)
{
	char *utf8 = nullptr;
	os_wcs_to_utf8_ptr(wide, 0, &utf8);
	std::string result = utf8 ? utf8 : "";
	bfree(utf8);
	return result;
}

/* Trims whitespace and appends ".exe" when no extension was typed. */
std::wstring normalize_executable(const char *value)
{
	std::wstring exe = to_wide(value ? value : "");

	const wchar_t *whitespace = L" \t\r\n\"";
	size_t first = exe.find_first_not_of(whitespace);
	if (first == std::wstring::npos)
		return L"";
	size_t last = exe.find_last_not_of(whitespace);
	exe = exe.substr(first, last - first + 1);

	if (exe.find(L'.') == std::wstring::npos)
		exe += L".exe";
	return exe;
}

/* Returns the top-level process with the given executable name, or 0 when it
 * is not running. Apps like Discord spawn children with the same name, and the
 * audio comes from one of those, so the exclusion has to target the root. */
DWORD find_root_process(const std::wstring &exe)
{
	if (exe.empty())
		return 0;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	std::map<DWORD, DWORD> matches;

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (_wcsicmp(entry.szExeFile, exe.c_str()) == 0)
				matches[entry.th32ProcessID] = entry.th32ParentProcessID;
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);

	for (const auto &match : matches) {
		if (matches.find(match.second) == matches.end())
			return match.first;
	}
	return matches.empty() ? 0 : matches.begin()->first;
}

struct DeEchoSource {
	obs_source_t *source;

	std::mutex settings_mutex;
	std::wstring executable;

	HANDLE stop_event = nullptr;
	HANDLE audio_event = nullptr;
	std::thread thread;

	ComPtr<IAudioClient> client;
	ComPtr<IAudioCaptureClient> capture;
	uint32_t sample_rate = 48000;
	DWORD active_pid = 0;
	bool logged_failure = false;
	std::vector<float> silence;

	DeEchoSource(obs_source_t *source_, obs_data_t *settings) : source(source_)
	{
		Update(settings);
		stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		thread = std::thread([this] { Run(); });
	}

	~DeEchoSource()
	{
		SetEvent(stop_event);
		if (thread.joinable())
			thread.join();
		CloseHandle(audio_event);
		CloseHandle(stop_event);
	}

	void Update(obs_data_t *settings)
	{
		std::wstring exe = normalize_executable(obs_data_get_string(settings, SETTING_EXECUTABLE));
		std::lock_guard<std::mutex> lock(settings_mutex);
		executable = exe;
	}

	std::wstring Executable()
	{
		std::lock_guard<std::mutex> lock(settings_mutex);
		return executable;
	}

	void Stop()
	{
		if (client)
			client->Stop();
		capture.Reset();
		client.Reset();
		active_pid = 0;
	}

	/* Starts a loopback stream of everything except the process tree of pid. */
	HRESULT Start(DWORD pid)
	{
		Stop();

		AUDIOCLIENT_ACTIVATION_PARAMS params = {};
		params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
		params.ProcessLoopbackParams.TargetProcessId = pid;
		params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

		PROPVARIANT activate_params = {};
		activate_params.vt = VT_BLOB;
		activate_params.blob.cbSize = sizeof(params);
		activate_params.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

		ComPtr<ActivateHandler> handler = Microsoft::WRL::Make<ActivateHandler>();
		if (!handler)
			return E_OUTOFMEMORY;

		ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
		HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
							 &activate_params, handler.Get(), &operation);
		if (FAILED(hr))
			return hr;

		if (WaitForSingleObject(handler->event(), 5000) != WAIT_OBJECT_0)
			return HRESULT_FROM_WIN32(ERROR_TIMEOUT);

		HRESULT activate_hr = E_FAIL;
		ComPtr<IUnknown> unknown;
		hr = operation->GetActivateResult(&activate_hr, &unknown);
		if (FAILED(hr))
			return hr;
		if (FAILED(activate_hr))
			return activate_hr;

		ComPtr<IAudioClient> new_client;
		hr = unknown.As(&new_client);
		if (FAILED(hr))
			return hr;

		struct obs_audio_info info = {};
		if (obs_get_audio_info(&info) && info.samples_per_sec)
			sample_rate = info.samples_per_sec;

		WAVEFORMATEXTENSIBLE format = {};
		format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		format.Format.nChannels = CHANNELS;
		format.Format.nSamplesPerSec = sample_rate;
		format.Format.wBitsPerSample = 32;
		format.Format.nBlockAlign = CHANNELS * sizeof(float);
		format.Format.nAvgBytesPerSec = sample_rate * format.Format.nBlockAlign;
		format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		format.Samples.wValidBitsPerSample = 32;
		format.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
		format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

		hr = new_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
					    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
					    BUFFER_TIME_100NS, 0, &format.Format, nullptr);
		if (FAILED(hr))
			return hr;

		hr = new_client->SetEventHandle(audio_event);
		if (FAILED(hr))
			return hr;

		ComPtr<IAudioCaptureClient> new_capture;
		hr = new_client->GetService(IID_PPV_ARGS(&new_capture));
		if (FAILED(hr))
			return hr;

		hr = new_client->Start();
		if (FAILED(hr))
			return hr;

		client = new_client;
		capture = new_capture;
		active_pid = pid;
		return S_OK;
	}

	bool Drain()
	{
		for (;;) {
			BYTE *buffer = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;

			HRESULT hr = capture->GetBuffer(&buffer, &frames, &flags, nullptr, nullptr);
			if (FAILED(hr)) {
				obs_log(LOG_WARNING, "capture failed (0x%08lX), restarting", hr);
				return false;
			}
			if (hr == AUDCLNT_S_BUFFER_EMPTY || frames == 0) {
				capture->ReleaseBuffer(0);
				return true;
			}

			struct obs_source_audio audio = {};
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
				silence.assign(static_cast<size_t>(frames) * CHANNELS, 0.0f);
				audio.data[0] = reinterpret_cast<const uint8_t *>(silence.data());
			} else {
				audio.data[0] = buffer;
			}
			audio.frames = frames;
			audio.speakers = SPEAKERS_STEREO;
			audio.format = AUDIO_FORMAT_FLOAT;
			audio.samples_per_sec = sample_rate;
			audio.timestamp = os_gettime_ns() - util_mul_div64(frames, 1000000000ULL, sample_rate);

			obs_source_output_audio(source, &audio);

			capture->ReleaseBuffer(frames);
		}
	}

	void Run()
	{
		os_set_thread_name("obs-de-echo capture");
		HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		const HANDLE handles[] = {stop_event, audio_event};
		ULONGLONG last_poll = 0;
		DWORD wanted_pid = GetCurrentProcessId();

		for (;;) {
			ULONGLONG now = GetTickCount64();
			if (!client || now - last_poll >= PROCESS_POLL_MS) {
				last_poll = now;

				const std::wstring exe = Executable();
				const DWORD target = find_root_process(exe);

				/* With the application closed, excluding OBS itself yields all
				 * other desktop audio and keeps audio monitoring from feeding
				 * back into the capture. */
				wanted_pid = target ? target : GetCurrentProcessId();

				if (!client || wanted_pid != active_pid) {
					HRESULT hr = Start(wanted_pid);
					if (SUCCEEDED(hr)) {
						logged_failure = false;
						if (target)
							obs_log(LOG_INFO, "excluding %s (pid %lu)",
								to_utf8(exe.c_str()).c_str(), target);
						else
							obs_log(LOG_INFO,
								"%s is not running, capturing all desktop audio",
								to_utf8(exe.c_str()).c_str());
					} else {
						Stop();
						if (!logged_failure)
							obs_log(LOG_ERROR,
								"could not start capture (0x%08lX), retrying", hr);
						logged_failure = true;
						if (WaitForSingleObject(stop_event, RETRY_MS) == WAIT_OBJECT_0)
							break;
						continue;
					}
				}
			}

			DWORD result = WaitForMultipleObjects(2, handles, FALSE, PROCESS_POLL_MS);
			if (result == WAIT_OBJECT_0)
				break;
			if (result == WAIT_OBJECT_0 + 1 && !Drain())
				Stop();
		}

		Stop();
		if (SUCCEEDED(com))
			CoUninitialize();
	}
};

struct WindowScan {
	std::set<DWORD> pids;
};

BOOL CALLBACK collect_window_pid(HWND window, LPARAM param)
{
	if (IsWindowVisible(window) && GetWindowTextLengthW(window) > 0) {
		DWORD pid = 0;
		GetWindowThreadProcessId(window, &pid);
		if (pid)
			reinterpret_cast<WindowScan *>(param)->pids.insert(pid);
	}
	return TRUE;
}

/* Executable names of every process that owns a visible window. */
std::set<std::string> list_window_executables()
{
	std::set<std::string> names;

	WindowScan scan;
	EnumWindows(collect_window_pid, reinterpret_cast<LPARAM>(&scan));

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return names;

	const DWORD self = GetCurrentProcessId();

	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (entry.th32ProcessID != self && scan.pids.count(entry.th32ProcessID))
				names.insert(to_utf8(entry.szExeFile));
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);

	return names;
}

const char *deecho_get_name(void *)
{
	return obs_module_text("DeEcho.Source");
}

void *deecho_create(obs_data_t *settings, obs_source_t *source)
{
	return new DeEchoSource(source, settings);
}

void deecho_destroy(void *data)
{
	delete static_cast<DeEchoSource *>(data);
}

void deecho_update(void *data, obs_data_t *settings)
{
	static_cast<DeEchoSource *>(data)->Update(settings);
}

void deecho_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_EXECUTABLE, DEFAULT_EXECUTABLE);
}

obs_properties_t *deecho_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, SETTING_EXECUTABLE, obs_module_text("DeEcho.Executable"),
						       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(list, obs_module_text("DeEcho.Executable.Tooltip"));

	std::set<std::string> names = list_window_executables();
	names.insert(DEFAULT_EXECUTABLE);
	for (const std::string &name : names)
		obs_property_list_add_string(list, name.c_str(), name.c_str());

	return props;
}

} // namespace

void deecho_register_source()
{
	struct obs_source_info info = {};
	info.id = "deecho_desktop_audio";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = deecho_get_name;
	info.create = deecho_create;
	info.destroy = deecho_destroy;
	info.update = deecho_update;
	info.get_defaults = deecho_get_defaults;
	info.get_properties = deecho_get_properties;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;

	obs_register_source(&info);
}
