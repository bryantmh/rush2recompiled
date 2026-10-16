// DualSense haptics: plays the rumble on DualSenses' two voice-coil actuators (include/dualsense_haptics.h).
//
// Over USB a DualSense is also a 4-channel 48 kHz sound device ("DualSense Wireless Controller"): channels 1 and 2 are
// its speaker and headphone jack, 3 and 4 the left and right actuators. Playing a waveform on channel 3 or 4 vibrates
// that side only, which is how PS5 games make their haptics. Each controller's actuator channels get three voices
// whose loudness on each side follows set_levels: a low thump, a rough buzz (two tones a little apart, which beat
// against each other like tires chattering) and a grinding rasp (a low tone jumping in loudness, with noise). The
// speaker channels stay silent.
//
// Every DualSense plays on its own sound device. Two DualSenses' sound devices have the same name, so they're told
// apart by device container instead: a controller's HID interface (its SDL path) and its sound endpoint belong to the
// same container, so the render endpoint whose container ID matches the controller's is its own. It's played through
// WASAPI in shared mode, one stream and thread per controller.
//
// Opening is done on those threads (device enumeration can take tens of milliseconds, too long for the VI thread that
// sets the levels), retrying every couple of seconds, so plugging a controller in mid-game picks it up. A manager
// thread stops a controller's thread once it hasn't been wanted for a while.
//
// On Linux the controller's sound card is the ALSA card on the same USB device as its SDL path (a hidraw or evdev
// node; src/linux_devices.cpp). When Steam Input stands in for the controller its path is a virtual device on no USB
// device, so it takes a DualSense card no other player has. How the card is reached depends on the sound server's
// profile for it:
// - A 4-channel sink (PulseAudio's or PipeWire's surround 4.0 profile, before alsa-ucm-conf knew the DualSense, or
//   UCM's Direct profile): played through Pulse (or PipeWire's pulse server) on the sink whose alsa.card is the card.
// - alsa-ucm-conf's DualSense profile (USB-Audio/Sony/DualSense-PS5-HiFi.conf, the default where it ships) splits
//   the card into a mono speaker and stereo headphone sink, each an ALSA dshare of the 4 channels (common/pcm/
//   split.conf), and channels 3 and 4 belong to no sink. A dshare opened with the same ipc_key and slave settings
//   joins that one, so the actuators play next to the sound server's speaker and headphones, as on Windows.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cfgmgr32.h>
#elif defined(__linux__)
#include <cstdlib>
#include <set>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <alsa/asoundlib.h>
#include "linux_devices.h"
#endif

#include "dualsense_haptics.h"

#if defined(_WIN32) || defined(__linux__)
namespace {
    // ---- Tuning ----

    // The voices' pitches are the profile's (set_levels; defaults in include/control_profiles.h). The voice coils
    // barely move below about 40 Hz, and above about 250 Hz they start to sound more than they shake.

    // The thump: a low fundamental plus a little of its octave, which gives the coil some bite.
    constexpr float thump_octave_mix = 0.35f; // Loudness of the octave (2 * thump_hz) next to the fundamental.

    // The buzz: two tones that aren't harmonics of each other, so they beat into a rough texture: buzz_hz and
    // buzz_hz * buzz_rough_ratio.
    constexpr double buzz_rough_ratio = 233.0 / 170.0;
    constexpr float buzz_rough_mix = 0.6f; // Loudness of the second tone next to buzz_hz.

    // The grind: a low tone whose loudness jumps randomly (grind_jitter_hz times a second), plus filtered noise, so it
    // feels like metal dragging on concrete rather than a steady hum.
    constexpr double grind_jitter_hz = 30.0;
    constexpr float grind_noise_mix = 0.5f;    // Loudness of the noise next to the tone.
    constexpr float grind_noise_cutoff = 0.05f; // One-pole low-pass on the noise per sample (about 400 Hz at 48 kHz).

    // Loudness of a level of 1, from 0 to 1 of full scale, per voice. The voices together are clamped to 1.
    constexpr float thump_gain = 1.0f;
    constexpr float buzz_gain = 0.8f;
    constexpr float grind_gain = 0.9f;

    // How fast each side's loudness follows set_levels, in seconds to get most of the way there. Short enough to keep a
    // hit sharp, long enough not to click.
    constexpr double level_smoothing_s = 0.008;

    // Seconds between looks for a DualSense's sound device while it's wanted but not open (over Bluetooth it never is).
    constexpr int scan_interval_s = 2;

    // Seconds a DualSense can stop wanting haptics before its device is closed (a menu between races shouldn't reopen
    // it).
    constexpr int idle_close_s = 10;

    constexpr int channels = 4;
    constexpr int left_channel = 2;  // Channel 3.
    constexpr int right_channel = 3; // Channel 4.

    // Targets set by set_levels: thump left, thump right, buzz left, buzz right, grind left, grind right.
    constexpr int num_levels = 6;

#ifdef _WIN32
    // WASAPI's buffer, 100 ns units: 20 ms, a little over one VI.
    constexpr REFERENCE_TIME buffer_duration = 200000;

    // DEVPKEY_Device_InstanceId and DEVPKEY_Device_ContainerId (devpkey.h). An endpoint's PKEY_Device_ContainerId is
    // the same key.
    const DEVPROPKEY instance_id_key = { { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 256 };
    const DEVPROPKEY container_id_key = { { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2 };
    const PROPERTYKEY endpoint_container_key = { { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2 };
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT and KSDATAFORMAT_SUBTYPE_PCM.
    const GUID subtype_float = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    const GUID subtype_pcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#else
    // Pulse's target buffer and the chunk each write fills: 20 ms and 5 ms, about WASAPI's latency.
    constexpr int pulse_rate = 48000;
    constexpr int buffer_frames = pulse_rate / 50;
    constexpr int chunk_frames = pulse_rate / 200;

    // alsa-ucm-conf's split dshare (common/pcm/split.conf): its ipc_key, sample format and period and buffer times, which
    // a dshare joining it has to match.
    constexpr int ucm_split_ipc_key = 6678293;
    constexpr int ucm_split_period_us = 20000;
    constexpr int ucm_split_buffer_us = 500000;
    // Frames kept queued in the shared buffer (whose 500 ms would be far too late): about WASAPI's 20 ms.
    constexpr snd_pcm_sframes_t shared_queue_frames = buffer_frames;

    constexpr unsigned vendor_sony = 0x054C;
    constexpr unsigned product_dualsense = 0x0CE6;
    constexpr unsigned product_dualsense_edge = 0x0DF2;
#endif

    // One controller's voices: the levels and pitches set_levels asks for, and the synthesis state (its thread only).
    struct Voices {
        std::atomic<float> targets[num_levels] = {};
        std::atomic<double> thump_hz = 55.0;
        std::atomic<double> buzz_hz = 170.0;
        std::atomic<double> grind_hz = 95.0;

        float levels[num_levels] = {};
        double thump_phase = 0.0;
        double buzz_phase = 0.0;
        double rough_phase = 0.0;
        double grind_phase = 0.0;
        uint32_t noise_state = 0x12345678;
        float noise_filtered = 0.0f;
        float grind_jitter = 1.0f;
        int jitter_samples_left = 0;

        void set(const rush2::haptics::Levels& l, float thump, float buzz, float grind) {
            const float values[num_levels] = { l.thump[0], l.thump[1], l.buzz[0], l.buzz[1], l.grind[0], l.grind[1] };
            for (int v = 0; v < num_levels; v++) {
                targets[v].store(values[v], std::memory_order_relaxed);
            }
            thump_hz.store(thump, std::memory_order_relaxed);
            buzz_hz.store(buzz, std::memory_order_relaxed);
            grind_hz.store(grind, std::memory_order_relaxed);
        }

        float noise() { // -1 to 1.
            noise_state = noise_state * 1664525u + 1013904223u;
            return (float)(noise_state >> 8) / (float)(1 << 23) - 1.0f;
        }

        static double advance(double& phase, double hz, int rate) {
            constexpr double two_pi = 6.283185307179586;
            double value = phase;
            phase += two_pi * hz / rate;
            if (phase >= two_pi) {
                phase -= two_pi;
            }
            return value;
        }

        // Fills `frames` frames of 4 channels (the actuators on 3 and 4, the speaker silent) as float or 16-bit samples.
        template <typename Sample>
        void fill(Sample* out, int frames, int rate) {
            float want[num_levels];
            for (int v = 0; v < num_levels; v++) {
                want[v] = targets[v].load(std::memory_order_relaxed);
            }
            const double thump_tone = thump_hz.load(std::memory_order_relaxed);
            const double buzz_tone = buzz_hz.load(std::memory_order_relaxed);
            const double grind_tone = grind_hz.load(std::memory_order_relaxed);
            const float smoothing = (float)(1.0 - std::exp(-1.0 / (level_smoothing_s * rate)));
            for (int i = 0; i < frames; i++) {
                for (int v = 0; v < num_levels; v++) {
                    levels[v] += (want[v] - levels[v]) * smoothing;
                }
                double t = advance(thump_phase, thump_tone, rate);
                float thump = (float)((std::sin(t) + thump_octave_mix * std::sin(2.0 * t)) / (1.0 + thump_octave_mix));
                float buzz = (float)((std::sin(advance(buzz_phase, buzz_tone, rate)) +
                    buzz_rough_mix * std::sin(advance(rough_phase, buzz_tone * buzz_rough_ratio, rate))) /
                    (1.0 + buzz_rough_mix));
                if (--jitter_samples_left <= 0) {
                    jitter_samples_left = (int)(rate / grind_jitter_hz);
                    grind_jitter = 0.55f + 0.45f * noise();
                }
                noise_filtered += (noise() - noise_filtered) * grind_noise_cutoff;
                float grind = ((float)std::sin(advance(grind_phase, grind_tone, rate)) * grind_jitter +
                    grind_noise_mix * noise_filtered * 4.0f) / (1.0f + grind_noise_mix);
                Sample* frame = out + i * channels;
                frame[0] = Sample{};
                frame[1] = Sample{};
                for (int side = 0; side < 2; side++) {
                    float value = std::clamp(thump * levels[side] * thump_gain + buzz * levels[2 + side] * buzz_gain +
                        grind * levels[4 + side] * grind_gain, -1.0f, 1.0f);
                    Sample& sample = frame[side == 0 ? left_channel : right_channel];
                    if constexpr (std::is_same_v<Sample, float>) {
                        sample = value;
                    }
                    else {
                        sample = (Sample)(value * 32767.0f);
                    }
                }
            }
        }
    };

#ifdef _WIN32
    // The device container of a controller's HID interface (its SDL path), or false.
    bool container_of(const std::string& path, GUID* container) {
        std::wstring wide(path.begin(), path.end());
        WCHAR instance[MAX_DEVICE_ID_LEN] = {};
        ULONG size = sizeof(instance);
        DEVPROPTYPE type;
        if (CM_Get_Device_Interface_PropertyW(wide.c_str(), &instance_id_key, &type, (PBYTE)instance, &size, 0) !=
            CR_SUCCESS) {
            return false;
        }
        DEVINST node;
        if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
            return false;
        }
        size = sizeof(*container);
        return CM_Get_DevNode_PropertyW(node, &container_id_key, &type, (PBYTE)container, &size, 0) == CR_SUCCESS;
    }

    // The active render endpoint in a device container, or null.
    IMMDevice* endpoint_in(const GUID& container) {
        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    (void**)&enumerator))) {
            return nullptr;
        }
        IMMDevice* found = nullptr;
        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count && found == nullptr; i++) {
                IMMDevice* device = nullptr;
                if (FAILED(collection->Item(i, &device))) {
                    continue;
                }
                IPropertyStore* store = nullptr;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
                    PROPVARIANT value;
                    PropVariantInit(&value);
                    if (SUCCEEDED(store->GetValue(endpoint_container_key, &value)) && value.vt == VT_CLSID &&
                        IsEqualGUID(*value.puuid, container)) {
                        found = device;
                        device = nullptr;
                    }
                    PropVariantClear(&value);
                    store->Release();
                }
                if (device != nullptr) {
                    device->Release();
                }
            }
            collection->Release();
        }
        enumerator->Release();
        return found;
    }
#else
    // Cards a player has open, so a controller behind Steam Input doesn't take one another player's controller plays on.
    std::mutex cards_mutex;
    std::set<int> cards_in_use;

    bool is_dualsense_card(const rush2::linux_devices::SoundCard& card) {
        return card.vendor == vendor_sony && (card.product == product_dualsense || card.product == product_dualsense_edge);
    }

    // Takes the ALSA card of the DualSense at a controller's SDL path into cards_in_use, or -1.
    int claim_card(const std::string& path) {
        std::vector<rush2::linux_devices::SoundCard> cards = rush2::linux_devices::sound_cards();
        std::string usb = rush2::linux_devices::usb_device_of(path);
        if (usb.empty() && !rush2::linux_devices::is_virtual(path)) {
            return -1; // Over Bluetooth: no sound card.
        }
        std::lock_guard lock{ cards_mutex };
        for (const auto& card : cards) {
            bool mine = usb.empty() ? is_dualsense_card(card) && cards_in_use.count(card.index) == 0
                                    : card.usb_device == usb;
            if (mine) {
                cards_in_use.insert(card.index);
                return card.index;
            }
        }
        return -1;
    }

    struct Sink {
        std::string name;
        pa_channel_map map{};
        int channels = 0;
    };

    // The Pulse sink playing on an ALSA card, or false (no server, or the card's profile is off).
    bool sink_for(int card, Sink* sink) {
        pa_mainloop* loop = pa_mainloop_new();
        pa_context* context = pa_context_new(pa_mainloop_get_api(loop), "Rush 2 Recompiled haptics");
        struct Search {
            int card;
            Sink* sink;
            bool found = false;
            bool done = false;
        } search{ card, sink };
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0) {
            pa_context_state_t state;
            while ((state = pa_context_get_state(context)) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(state)) {
                pa_mainloop_iterate(loop, 1, nullptr);
            }
            if (state == PA_CONTEXT_READY) {
                pa_operation* op = pa_context_get_sink_info_list(context,
                    [](pa_context*, const pa_sink_info* info, int eol, void* user) {
                        Search& s = *(Search*)user;
                        if (eol != 0) {
                            s.done = true;
                            return;
                        }
                        // PulseAudio's ALSA sinks have alsa.card; PipeWire's have it and api.alsa.card.
                        for (const char* key : { "alsa.card", "api.alsa.card", "api.alsa.pcm.card" }) {
                            const char* value = pa_proplist_gets(info->proplist, key);
                            if (!s.found && value != nullptr && atoi(value) == s.card) {
                                s.found = true;
                                s.sink->name = info->name;
                                s.sink->map = info->channel_map;
                                s.sink->channels = info->sample_spec.channels;
                            }
                        }
                    }, &search);
                while (op != nullptr && !search.done && pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
                    pa_mainloop_iterate(loop, 1, nullptr);
                }
                if (op != nullptr) {
                    pa_operation_unref(op);
                }
            }
            pa_context_disconnect(context);
        }
        pa_context_unref(context);
        pa_mainloop_free(loop);
        return search.found;
    }
#endif

    // One DualSense: its voices and the thread playing them.
    struct Player {
        std::string path;
        Voices voices;
        std::atomic_bool stop = false;
        std::atomic_bool open = false;
        std::chrono::steady_clock::time_point last_wanted; // Under players_mutex.
        std::thread thread;

#ifdef _WIN32
        // Plays on the endpoint until told to stop or the device goes away.
        void play(IMMDevice* device) {
            IAudioClient* client = nullptr;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) {
                return;
            }
            WAVEFORMATEX* format = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&format))) {
                bool extensible = format->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
                const GUID* subtype = extensible ? &((WAVEFORMATEXTENSIBLE*)format)->SubFormat : nullptr;
                bool is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (subtype != nullptr && IsEqualGUID(*subtype, subtype_float));
                bool is_pcm16 = (format->wFormatTag == WAVE_FORMAT_PCM ||
                                 (subtype != nullptr && IsEqualGUID(*subtype, subtype_pcm))) &&
                    format->wBitsPerSample == 16;
                if (format->nChannels != channels || !(is_float || is_pcm16)) {
                    // A stereo device would get the haptics mixed into its speaker.
                    fprintf(stderr, "[Haptics] The sound device for %s has %d channels of %d bits; not using it\n",
                        path.c_str(), format->nChannels, format->wBitsPerSample);
                }
                else {
                    render(client, format, is_float);
                }
                CoTaskMemFree(format);
            }
            client->Release();
        }

        void render(IAudioClient* client, WAVEFORMATEX* format, bool is_float) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            IAudioRenderClient* renderer = nullptr;
            UINT32 buffer_frames = 0;
            if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer_duration,
                                          0, format, nullptr)) ||
                FAILED(client->SetEventHandle(event)) || FAILED(client->GetBufferSize(&buffer_frames)) ||
                FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&renderer)) ||
                FAILED(client->Start())) {
                fprintf(stderr, "[Haptics] Couldn't start the sound device for %s\n", path.c_str());
                if (renderer != nullptr) {
                    renderer->Release();
                }
                CloseHandle(event);
                return;
            }
            open = true;
            printf("[Haptics] Playing DualSense haptics on %s\n", path.c_str());
            int rate = (int)format->nSamplesPerSec;
            while (!stop) {
                WaitForSingleObject(event, 200);
                UINT32 padding = 0;
                if (FAILED(client->GetCurrentPadding(&padding))) {
                    break; // Unplugged.
                }
                UINT32 frames = buffer_frames - padding;
                BYTE* data = nullptr;
                if (frames == 0 || FAILED(renderer->GetBuffer(frames, &data))) {
                    continue;
                }
                if (is_float) {
                    voices.fill((float*)data, (int)frames, rate);
                }
                else {
                    voices.fill((int16_t*)data, (int)frames, rate);
                }
                renderer->ReleaseBuffer(frames, 0);
            }
            open = false;
            client->Stop();
            renderer->Release();
            CloseHandle(event);
            printf("[Haptics] Stopped DualSense haptics on %s\n", path.c_str());
        }

        void run() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            while (!stop) {
                GUID container;
                IMMDevice* device = container_of(path, &container) ? endpoint_in(container) : nullptr;
                if (device != nullptr) {
                    play(device);
                    device->Release();
                }
                for (int i = 0; i < scan_interval_s * 10 && !stop; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            CoUninitialize();
        }
#else
        // Plays on the card's 4-channel sink until told to stop or the device goes away.
        void play(const Sink& sink) {
            pa_sample_spec spec{ PA_SAMPLE_FLOAT32LE, pulse_rate, channels };
            const uint32_t frame_bytes = sizeof(float) * channels;
            pa_buffer_attr attr{ (uint32_t)-1, buffer_frames * frame_bytes, (uint32_t)-1, chunk_frames * frame_bytes,
                                 (uint32_t)-1 };
            int error = 0;
            // The sink's own channel map, so the actuators' channels 3 and 4 reach them unmixed.
            pa_simple* stream = pa_simple_new(nullptr, "Rush 2 Recompiled", PA_STREAM_PLAYBACK, sink.name.c_str(),
                "DualSense haptics", &spec, &sink.map, &attr, &error);
            if (stream == nullptr) {
                fprintf(stderr, "[Haptics] Couldn't start the sound device for %s: %s\n", path.c_str(),
                    pa_strerror(error));
                return;
            }
            open = true;
            printf("[Haptics] Playing DualSense haptics on %s (%s)\n", path.c_str(), sink.name.c_str());
            float buffer[chunk_frames * channels];
            while (!stop) {
                voices.fill(buffer, chunk_frames, pulse_rate);
                if (pa_simple_write(stream, buffer, sizeof(buffer), &error) < 0) {
                    break; // Unplugged.
                }
            }
            open = false;
            pa_simple_free(stream);
            printf("[Haptics] Stopped DualSense haptics on %s\n", path.c_str());
        }

        // Plays on the actuator channels through a dshare joining alsa-ucm-conf's split of the card, until told to stop
        // or the device goes away. Fails (busy) when the sound server has the card open whole.
        void play_shared(int card) {
            char conf[512];
            snprintf(conf, sizeof(conf),
                "pcm.rush2_haptics { type dshare ipc_key %d slave { pcm { type hw card %d device 0 } format S16_LE "
                "channels %d rate %d period_time %d buffer_time %d } bindings { 0 %d 1 %d } }",
                ucm_split_ipc_key, card, channels, pulse_rate, ucm_split_period_us, ucm_split_buffer_us, left_channel,
                right_channel);
            snd_config_t* config = nullptr;
            snd_input_t* input = nullptr;
            snd_pcm_t* pcm = nullptr;
            int error = snd_config_top(&config);
            if (error >= 0 && (error = snd_input_buffer_open(&input, conf, -1)) >= 0) {
                error = snd_config_load(config, input);
                snd_input_close(input);
            }
            if (error >= 0) {
                error = snd_pcm_open_lconf(&pcm, "rush2_haptics", SND_PCM_STREAM_PLAYBACK, 0, config);
            }
            if (error >= 0) {
                error = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, pulse_rate, 0,
                    ucm_split_buffer_us);
            }
            snd_pcm_sw_params_t* sw = nullptr;
            if (error >= 0 && snd_pcm_sw_params_malloc(&sw) >= 0) {
                // Start on the first write: the default waits for the whole half second.
                snd_pcm_sw_params_current(pcm, sw);
                snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
                error = snd_pcm_sw_params(pcm, sw);
                snd_pcm_sw_params_free(sw);
            }
            if (config != nullptr) {
                snd_config_delete(config);
            }
            if (error < 0) {
                fprintf(stderr, "[Haptics] Couldn't open the sound device for %s (card %d): %s\n", path.c_str(), card,
                    snd_strerror(error));
                if (pcm != nullptr) {
                    snd_pcm_close(pcm);
                }
                return;
            }
            open = true;
            printf("[Haptics] Playing DualSense haptics on %s (card %d, shared)\n", path.c_str(), card);
            int16_t frames4[chunk_frames * channels];
            int16_t frames2[chunk_frames * 2];
            while (!stop) {
                snd_pcm_sframes_t queued = 0;
                if (snd_pcm_delay(pcm, &queued) >= 0 && queued > shared_queue_frames) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                voices.fill(frames4, chunk_frames, pulse_rate);
                for (int i = 0; i < chunk_frames; i++) {
                    frames2[i * 2] = frames4[i * channels + left_channel];
                    frames2[i * 2 + 1] = frames4[i * channels + right_channel];
                }
                snd_pcm_sframes_t written = snd_pcm_writei(pcm, frames2, chunk_frames);
                if (written < 0 && snd_pcm_recover(pcm, (int)written, 1) < 0) {
                    break; // Unplugged.
                }
            }
            open = false;
            snd_pcm_drop(pcm);
            snd_pcm_close(pcm);
            printf("[Haptics] Stopped DualSense haptics on %s\n", path.c_str());
        }

        void run() {
            while (!stop) {
                int card = claim_card(path);
                if (card >= 0) {
                    Sink sink;
                    if (sink_for(card, &sink) && sink.channels == channels) {
                        play(sink);
                    }
                    else {
                        play_shared(card);
                    }
                    std::lock_guard lock{ cards_mutex };
                    cards_in_use.erase(card);
                }
                for (int i = 0; i < scan_interval_s * 10 && !stop; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
#endif
    };

    std::mutex players_mutex;
    // By path; under players_mutex. Never destroyed: a player's thread runs as long as the game, and destroying it at
    // exit would abort the process (a joinable std::thread) and write a crash dump, which held up closing.
    std::map<std::string, std::unique_ptr<Player>>& players = *new std::map<std::string, std::unique_ptr<Player>>;
    std::once_flag manager_started;

    // Stops the players no longer wanted (joining their threads, which can take a moment, off the VI thread).
    void manage() {
        while (true) {
            std::vector<std::unique_ptr<Player>> closing;
            {
                std::lock_guard lock{ players_mutex };
                auto now = std::chrono::steady_clock::now();
                for (auto it = players.begin(); it != players.end();) {
                    if (now - it->second->last_wanted > std::chrono::seconds(idle_close_s)) {
                        it->second->stop = true;
                        closing.push_back(std::move(it->second));
                        it = players.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
            for (auto& player : closing) {
                player->thread.join();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

void rush2::haptics::set_controllers(const std::vector<std::string>& paths) {
    if (paths.empty()) {
        return;
    }
    std::call_once(manager_started, [] { std::thread(manage).detach(); });
    std::lock_guard lock{ players_mutex };
    auto now = std::chrono::steady_clock::now();
    for (const std::string& path : paths) {
        auto& player = players[path];
        if (player == nullptr) {
            player = std::make_unique<Player>();
            player->path = path;
            player->thread = std::thread([p = player.get()] { p->run(); });
        }
        player->last_wanted = now;
    }
}

bool rush2::haptics::active(const std::string& path) {
    std::lock_guard lock{ players_mutex };
    auto it = players.find(path);
    return it != players.end() && it->second->open;
}

void rush2::haptics::set_levels(const std::string& path, const Levels& levels, float thump_hz, float buzz_hz,
                                float grind_hz) {
    std::lock_guard lock{ players_mutex };
    auto it = players.find(path);
    if (it != players.end()) {
        it->second->voices.set(levels, thump_hz, buzz_hz, grind_hz);
    }
}
#else // Other systems: none.
void rush2::haptics::set_controllers(const std::vector<std::string>&) {}
bool rush2::haptics::active(const std::string&) { return false; }
void rush2::haptics::set_levels(const std::string&, const Levels&, float, float, float) {}
#endif
