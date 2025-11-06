#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <sndfile.h>
#include <vector>
#include <map>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>

#define MYDRUMKIT_URI "http://realsigmamusic.com/plugins/mydrumkit"

// Estrutura de um sample carregado (mono ou estéreo)
struct Sample {
    std::vector<float> dataL;  // canal esquerdo (ou mono)
    std::vector<float> dataR;  // canal direito (vazio se mono)
    int channels;
    int sampleRate;
    bool is_stereo;

    Sample() : channels(0), sampleRate(0), is_stereo(false) {}
};

// Round Robin Group - grupo de samples para uma nota
struct RRGroup {
    std::vector<Sample> samples;
    uint32_t current_rr;  // índice atual do round robin
    int output;           // saída de áudio (para mono, ou L para estéreo)

    RRGroup() : current_rr(0), output(0) {}

    const Sample* getNextSample() {
        if (samples.empty()) return nullptr;
        const Sample* s = &samples[current_rr];
        current_rr = (current_rr + 1) % samples.size();
        return s;
    }
};

// Estrutura de uma voz ativa
struct Voice {
    const Sample* sample;
    uint32_t pos;
    uint32_t length;
    int output;

    Voice() : sample(nullptr), pos(0), length(0), output(0) {}
};

// Estrutura principal do plugin
struct MyDrumKit {
    std::map<int, RRGroup> rr_groups;  // nota MIDI -> grupo round robin
    std::vector<Voice> voices;
    float* outputs[8];
    const LV2_Atom_Sequence* midi_in;
    LV2_URID midi_event_urid;

    // Construtor
    MyDrumKit() : midi_in(nullptr), midi_event_urid(0) {
        for (int i = 0; i < 8; ++i) {
            outputs[i] = nullptr;
        }
    }
};

// Função para converter caminhos: bundle_path + "/" + rel
static std::string join_path(const char* bundle_path, const char* rel) {
    if (!bundle_path || bundle_path[0] == '\0') return std::string(rel);
    std::string bp(bundle_path);
    if (bp.back() == '/')
        return bp + rel;
    else
        return bp + "/" + rel;
}

// Função para carregar um arquivo WAV (mantém estéreo se for o caso)
static Sample load_wav_from_bundle(const char* bundle_path, const char* relpath, bool force_stereo = false) {
    std::string full = join_path(bundle_path, relpath);
    const std::string& path = full;
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);

    Sample s;
    if (!file) {
        fprintf(stderr, "MyDrumKit: Erro ao carregar %s: %s\n",
                path.c_str(), sf_strerror(nullptr));
        return s;
    }

    s.channels = info.channels;
    s.sampleRate = info.samplerate;
    sf_count_t frames = info.frames;

    if (frames <= 0 || info.channels <= 0) {
        fprintf(stderr, "MyDrumKit: Arquivo inválido %s\n", path.c_str());
        sf_close(file);
        return s;
    }

    std::vector<float> tmp(frames * info.channels);
    sf_count_t read = sf_read_float(file, tmp.data(), tmp.size());
    sf_close(file);

    if (read != tmp.size()) {
        fprintf(stderr, "MyDrumKit: Leitura incompleta de %s\n", path.c_str());
    }

    // Se force_stereo está ativo E o arquivo é estéreo, mantém estéreo
    if (force_stereo && info.channels >= 2) {
        s.is_stereo = true;
        s.dataL.resize(frames);
        s.dataR.resize(frames);

        for (sf_count_t i = 0; i < frames; ++i) {
            s.dataL[i] = tmp[i * info.channels + 0];      // Canal L
            s.dataR[i] = tmp[i * info.channels + 1];      // Canal R
        }
        s.channels = 2;
        fprintf(stderr, "MyDrumKit: Carregado %s (ESTÉREO, %d Hz, %lld frames)\n",
                relpath, s.sampleRate, (long long)frames);
    }
    // Senão, converte para mono
    else if (info.channels == 1) {
        s.is_stereo = false;
        s.dataL = std::move(tmp);
        s.channels = 1;
        fprintf(stderr, "MyDrumKit: Carregado %s (mono, %d Hz, %lld frames)\n",
                relpath, s.sampleRate, (long long)frames);
    } else {
        // Converte para mono (média dos canais)
        s.is_stereo = false;
        s.dataL.resize(frames);
        for (sf_count_t i = 0; i < frames; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < info.channels; ++c) {
                sum += tmp[i * info.channels + c];
            }
            s.dataL[i] = sum / (float)info.channels;
        }
        s.channels = 1;
        fprintf(stderr, "MyDrumKit: Carregado %s (%d canais -> mono, %d Hz, %lld frames)\n",
                relpath, info.channels, s.sampleRate, (long long)frames);
    }

    return s;
}

// Helper para adicionar sample a um grupo RR
static void add_to_rr_group(MyDrumKit* self, int note, const char* bundle_path,
                           const char* relpath, int output, bool force_stereo = false) {
    Sample s = load_wav_from_bundle(bundle_path, relpath, force_stereo);
    if (!s.dataL.empty()) {
        auto& group = self->rr_groups[note];
        group.samples.push_back(std::move(s));
        group.output = output;
    }
}

// Inicialização do plugin
static LV2_Handle instantiate(const LV2_Descriptor* desc,
                              double sample_rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {

    fprintf(stderr, "MyDrumKit: Iniciando instanciação (bundle=%s, sr=%.1f)\n",
            bundle_path ? bundle_path : "(null)", sample_rate);

    MyDrumKit* self = nullptr;
    try {
        self = new MyDrumKit();
    } catch (const std::exception& e) {
        fprintf(stderr, "MyDrumKit: Erro ao alocar: %s\n", e.what());
        return nullptr;
    }

    if (!self) {
        fprintf(stderr, "MyDrumKit: Falha ao alocar memória\n");
        return nullptr;
    }

    // Recupera o map URID
    LV2_URID_Map* map = nullptr;
    for (const LV2_Feature* const* f = features; f && *f; ++f) {
        if (!strcmp((*f)->URI, LV2_URID__map)) {
            map = (LV2_URID_Map*)(*f)->data;
            break;
        }
    }

    if (map) {
        self->midi_event_urid = map->map(map->handle, LV2_MIDI__MidiEvent);
        fprintf(stderr, "MyDrumKit: URID mapeado: %u\n", self->midi_event_urid);
    } else {
        fprintf(stderr, "MyDrumKit: AVISO - URID map não encontrado!\n");
        delete self;
        return nullptr;
    }

    // Carrega samples com Round Robin
    try {
        fprintf(stderr, "MyDrumKit: Carregando samples com Round Robin...\n");

        // CRASH (nota 49) - saída 6/7 (Overhead L/R) - ESTÉREO
        add_to_rr_group(self, 49, bundle_path, "samples/crash1_edge_v1_r1.wav", 6, true);
        add_to_rr_group(self, 57, bundle_path, "samples/crash2_edge_v1_r1.wav", 6, true);

        // HIHAT CLOSED (nota 42) - saída 2 (HiHat)
        add_to_rr_group(self, 42, bundle_path, "samples/hihat_downclosed_v1_r1.wav", 2);
        add_to_rr_group(self, 42, bundle_path, "samples/hihat_downclosed_v1_r2.wav", 2);

        // HIHAT OPEN (nota 46) - saída 2 (HiHat)
        add_to_rr_group(self, 46, bundle_path, "samples/hihat_downopen_v1_r1.wav", 2);
        add_to_rr_group(self, 46, bundle_path, "samples/hihat_downopen_v1_r2.wav", 2);

        // HIHAT PEDAL (nota 44) - saída 2 (HiHat)
        add_to_rr_group(self, 44, bundle_path, "samples/hihat_pedalclosed_v1_r1.wav", 2);

        // KICK OPEN (nota 36) - saída 0 (Kick)
        add_to_rr_group(self, 36, bundle_path, "samples/kick_open_v1_r1.wav", 0);

        // KICK MUTED (nota 35) - saída 0 (Kick)
        add_to_rr_group(self, 35, bundle_path, "samples/kick_muted_v1_r1.wav", 0);

        // RIDE (nota 51) - saída 6/7 (Overhead L/R) - ESTÉREO
        add_to_rr_group(self, 51, bundle_path, "samples/ride_bow_v1_r1.wav", 6, true);

        // SNARE CENTER (nota 38) - saída 1 (Snare)
        add_to_rr_group(self, 38, bundle_path, "samples/snare_center_v1_r1.wav", 1);
        add_to_rr_group(self, 38, bundle_path, "samples/snare_center_v1_r2.wav", 1);

        // SNARE RIMSHOT (nota 40) - saída 1 (Snare)
        add_to_rr_group(self, 40, bundle_path, "samples/snare_rimshot_v1_r1.wav", 1);

        // SNARE SIDESTICK (nota 37) - saída 1 (Snare)
        add_to_rr_group(self, 37, bundle_path, "samples/snare_sidestick_v1_r1.wav", 1);

        // TOM 1 (nota 50) - saída 3 (Tom1)
        add_to_rr_group(self, 50, bundle_path, "samples/tom1_center_v1_r1.wav", 3);
        add_to_rr_group(self, 50, bundle_path, "samples/tom1_center_v1_r2.wav", 3);

        // TOM 2 (nota 47) - saída 4 (Tom2)
        add_to_rr_group(self, 47, bundle_path, "samples/tom2_center_v1_r1.wav", 4);
        add_to_rr_group(self, 47, bundle_path, "samples/tom2_center_v1_r2.wav", 4);

        // TOM 3 / FLOOR TOM (nota 41) - saída 5 (Tom3)
        add_to_rr_group(self, 41, bundle_path, "samples/tom3_center_v1_r1.wav", 5);
        add_to_rr_group(self, 41, bundle_path, "samples/tom3_center_v1_r2.wav", 5);

        // Log resumo
        fprintf(stderr, "MyDrumKit: %zu notas MIDI carregadas:\n", self->rr_groups.size());
        for (const auto& pair : self->rr_groups) {
            fprintf(stderr, "  Nota %d: %zu variações RR -> saída %d\n",
                    pair.first, pair.second.samples.size(), pair.second.output);
        }

    } catch (const std::exception& e) {
        fprintf(stderr, "MyDrumKit: Erro ao carregar samples: %s\n", e.what());
        delete self;
        return nullptr;
    }

    fprintf(stderr, "MyDrumKit: Instanciação completa\n");
    return (LV2_Handle)self;
}

// Conexão das portas
static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    MyDrumKit* self = (MyDrumKit*)instance;
    if (!self) return;

    if (port == 0) {
        self->midi_in = (const LV2_Atom_Sequence*)data;
    } else if (port >= 1 && port <= 8) {
        self->outputs[port - 1] = (float*)data;
    }
}

// Execução (processamento de áudio e MIDI)
static void run(LV2_Handle instance, uint32_t n_samples) {
    MyDrumKit* self = (MyDrumKit*)instance;
    if (!self) return;

    // Limpa os buffers de saída
    for (int i = 0; i < 8; ++i) {
        if (self->outputs[i])
            std::memset(self->outputs[i], 0, sizeof(float) * n_samples);
    }

    // Processa os eventos MIDI
    if (self->midi_in && self->midi_event_urid != 0) {
        LV2_ATOM_SEQUENCE_FOREACH(self->midi_in, ev) {
            if (ev->body.type == self->midi_event_urid) {
                const uint8_t* msg = (const uint8_t*)(ev + 1);
                if (!msg) continue;

                uint8_t status = msg[0] & 0xF0;
                uint8_t note   = msg[1];
                uint8_t vel    = msg[2];

                if (status == 0x90 && vel > 0) { // NOTE ON
                    auto it = self->rr_groups.find(note);
                    if (it != self->rr_groups.end()) {
                        RRGroup& group = it->second;
                        const Sample* sample = group.getNextSample();

                        if (sample && !sample->dataL.empty()) {
                            Voice v;
                            v.sample = sample;
                            v.pos = 0;
                            v.length = sample->dataL.size();
                            v.output = group.output;

                            self->voices.push_back(v);
                        }
                    }
                }
            }
        }
    }

    // Renderiza o áudio
    for (auto it = self->voices.begin(); it != self->voices.end();) {
        auto& v = *it;

        if (!v.sample || v.sample->dataL.empty()) {
            it = self->voices.erase(it);
            continue;
        }

        const float* dataL = v.sample->dataL.data();
        const float* dataR = v.sample->is_stereo ? v.sample->dataR.data() : nullptr;

        if (!dataL) {
            it = self->voices.erase(it);
            continue;
        }

        for (uint32_t i = 0; i < n_samples && v.pos < v.length; ++i) {
            // Renderiza canal L (ou mono)
            if (v.output >= 0 && v.output < 8 && self->outputs[v.output]) {
                self->outputs[v.output][i] += dataL[v.pos];
            }

            // Se for estéreo, renderiza canal R na próxima saída
            if (dataR && v.output >= 0 && v.output < 7 && self->outputs[v.output + 1]) {
                self->outputs[v.output + 1][i] += dataR[v.pos];
            }

            v.pos++;
        }

        if (v.pos >= v.length)
            it = self->voices.erase(it);
        else
            ++it;
    }
}

// Limpeza de memória
static void cleanup(LV2_Handle instance) {
    MyDrumKit* self = (MyDrumKit*)instance;
    if (!self) return;

    fprintf(stderr, "MyDrumKit: Limpando plugin\n");
    delete self;
}

// Descritor do plugin
static const LV2_Descriptor descriptor = {
    MYDRUMKIT_URI,
    instantiate,
    connect_port,
    nullptr,     // activate
    run,
    nullptr,     // deactivate
    cleanup,
    nullptr      // extension_data
};

// Função principal de exportação
extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : nullptr;
}
