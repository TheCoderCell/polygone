#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <stdio.h>
#include <algorithm>

#define WIN_W 800
#define WIN_H 800
#define MAX_PARTICLES 500
#define M_PI 3.14159265358979323846

#define MIN_SIDES 3

#define POLY_RADIUS 220.0f
#define INITIAL_SCALE 2.0f       
#define BASE_SHRINK_RATE 0.18f
#define SHRINK_RATE_STEP 0.05f
#define MAX_SHRINK_RATE 1.2f
#define LOSE_SCALE 0.05f

#define SQUISH_DECAY_RATE 10.0f
#define PARTICLE_DRAG_RATE 3.0f
#define PULSE_SPEED 0.8f

#define PREVIEW_RADIUS 28.0f
#define PREVIEW_X (WIN_W - 70.0f)
#define PREVIEW_Y 80.0f
#define PREVIEW_SPIN_SPEED 2.5f

#define MAX_SFX_STREAMS 8

enum Game_State { 
    STATE_MENU, 
    STATE_SETTINGS_MENU, 
    STATE_INPUT, 
    STATE_PLAYING, 
    STATE_PAUSE, 
    STATE_WIN, 
   STATE_GAME_OVER 
};

bool g_particles_enabled = true;
int g_shrink_speed_setting = 1;

typedef struct {
    float r, g, b;
    const char* name;
} PulseColorDef;

PulseColorDef pulse_color_options[] = {
    {1.0f, 0.0f, 0.0f, "Red"},
    {0.0f, 0.5f, 1.0f, "Blue"},     
    {0.0f, 1.0f, 0.0f, "Green"},
    {1.0f, 1.0f, 0.0f, "Yellow"},
    {0.6f, 0.0f, 1.0f, "Purple"},
    {1.0f, 1.0f, 1.0f, "White"}
};
int num_pulse_colors = 6;
int g_pulse_color_idx = 1; 
float g_pulse_duration = 0.1f;
float g_pulse_max_alpha = 0.5f;

typedef struct {
    float x, y, w, h, val;
    bool dragging;
} Slider;

typedef struct { 
    Uint8* data; 
    Uint32 len; 
    SDL_AudioSpec spec; 
} WAV_Data;

WAV_Data load_wav(const char* path) {
    WAV_Data w = {0};
    FILE* f = fopen(path, "rb");
    if (!f) {
        SDL_Log("AUDIO ERROR: Failed to open WAV file '%s'. Make sure it's in the same folder as the executable.", path);
        return w;
    }
    
    char header[4];
    if (fread(header, 1, 4, f) != 4 || memcmp(header, "RIFF", 4) != 0) { 
        SDL_Log("AUDIO ERROR: '%s' is not a valid WAV (no RIFF header)", path);
        fclose(f); return w; 
    }
    
    fseek(f, 4, SEEK_CUR);
    if (fread(header, 1, 4, f) != 4 || memcmp(header, "WAVE", 4) != 0) { 
        SDL_Log("AUDIO ERROR: '%s' is not a valid WAV (no WAVE header)", path);
        fclose(f); return w; 
    }
    
    while (!feof(f)) {
        if (fread(header, 1, 4, f) != 4) break;
        Uint32 chunk_size = 0; 
        if (fread(&chunk_size, 4, 1, f) != 1) break;
        
        if (memcmp(header, "fmt ", 4) == 0) {
            Uint16 audio_format    = 0;
            Uint16 channels        = 0;
            Uint32 sample_rate     = 0;
            Uint32 byte_rate       = 0;
            Uint16 block_align     = 0;
            Uint16 bits_per_sample = 0;

            fread(&audio_format,    2, 1, f);
            fread(&channels,        2, 1, f);
            fread(&sample_rate,     4, 1, f);
            fread(&byte_rate,       4, 1, f);
            fread(&block_align,     2, 1, f);
            fread(&bits_per_sample, 2, 1, f);

            w.spec.channels = channels;
            w.spec.freq     = sample_rate;

            if (audio_format == 1) {
                if      (bits_per_sample == 8)  w.spec.format = (SDL_AudioFormat)SDL_AUDIO_U8;
                else if (bits_per_sample == 16) w.spec.format = (SDL_AudioFormat)SDL_AUDIO_S16;
                else if (bits_per_sample == 32) w.spec.format = (SDL_AudioFormat)SDL_AUDIO_S32;
            } else if (audio_format == 3) {
                if (bits_per_sample == 32)      w.spec.format = (SDL_AudioFormat)SDL_AUDIO_F32;
            }
            
            if (chunk_size > 16) {
                fseek(f, chunk_size - 16, SEEK_CUR);
            }
        } 
        else if (memcmp(header, "data", 4) == 0) {
            w.len = chunk_size; 
            w.data = (Uint8*)malloc(w.len);
            if (!w.data) { fclose(f); return w; }
            fread(w.data, 1, w.len, f); 
            fclose(f);
            SDL_Log("AUDIO: Loaded '%s' (Format: %d, Channels: %d, Freq: %d, Size: %d bytes)", path, w.spec.format, w.spec.channels, w.spec.freq, w.len);
            return w;
        } 
        else { 
            fseek(f, chunk_size, SEEK_CUR); 
        }
        
        if (chunk_size % 2 == 1) {
            fseek(f, 1, SEEK_CUR);
        }
    }
    SDL_Log("AUDIO ERROR: '%s' did not contain a data chunk", path);
    fclose(f); 
    return w;
}

SDL_AudioDeviceID audio_dev = 0;
SDL_AudioSpec desired_spec;
SDL_AudioStream* music_stream = NULL;
WAV_Data current_music_wav = {0};
WAV_Data click_sfx_wav = {0};
SDL_AudioStream* active_sfx[MAX_SFX_STREAMS] = {0};
int num_active_sfx = 0;

void change_music(const char* path) {
    if (music_stream) {
        SDL_UnbindAudioStream(music_stream);
        SDL_DestroyAudioStream(music_stream);
        music_stream = NULL;
    }
    if (current_music_wav.data) {
        free(current_music_wav.data);
        current_music_wav.data = NULL;
    }
    if (!audio_dev) return;
    
    current_music_wav = load_wav(path);
    if (current_music_wav.data && current_music_wav.spec.format != 0) {
        music_stream = SDL_CreateAudioStream(&current_music_wav.spec, &desired_spec);
        if (!music_stream) {
            SDL_Log("AUDIO ERROR: Failed to create music stream: %s", SDL_GetError());
        } else {
            SDL_SetAudioStreamGain(music_stream, 0.8f);
            if (!SDL_BindAudioStream(audio_dev, music_stream)) {
                SDL_Log("AUDIO ERROR: Failed to bind music stream: %s", SDL_GetError());
            }
            SDL_PutAudioStreamData(music_stream, current_music_wav.data, (int)current_music_wav.len);
            SDL_Log("AUDIO: Music stream created and bound.");
        }
    }
}

void update_audio() {
    if (music_stream && SDL_GetAudioStreamQueued(music_stream) == 0) {
        if (current_music_wav.data) {
            SDL_PutAudioStreamData(music_stream, current_music_wav.data, (int)current_music_wav.len);
        }
    }
    
    for (int i = num_active_sfx - 1; i >= 0; i--) {
        if (!active_sfx[i] || SDL_GetAudioStreamQueued(active_sfx[i]) <= 0) {
            if (active_sfx[i]) {
                SDL_UnbindAudioStream(active_sfx[i]);
                SDL_DestroyAudioStream(active_sfx[i]);
            }
            active_sfx[i] = active_sfx[num_active_sfx - 1];
            active_sfx[num_active_sfx - 1] = 0;
            num_active_sfx--;
        }
    }
}

void play_sfx(float volume) {
    if (!audio_dev || !click_sfx_wav.data || click_sfx_wav.spec.format == 0 || num_active_sfx >= MAX_SFX_STREAMS) return;
    
    SDL_AudioStream* sfx_stream = SDL_CreateAudioStream(&click_sfx_wav.spec, &desired_spec);
    if (!sfx_stream) {
        SDL_Log("AUDIO ERROR: Failed to create SFX stream: %s", SDL_GetError());
        return;
    }
    
    SDL_SetAudioStreamGain(sfx_stream, volume);
    if (!SDL_BindAudioStream(audio_dev, sfx_stream)) {
        SDL_Log("AUDIO ERROR: Failed to bind SFX stream: %s", SDL_GetError());
        SDL_DestroyAudioStream(sfx_stream);
        return;
    }
    SDL_PutAudioStreamData(sfx_stream, click_sfx_wav.data, (int)click_sfx_wav.len);
    SDL_FlushAudioStream(sfx_stream);
    
    active_sfx[num_active_sfx++] = sfx_stream;
}

typedef struct {
    int max_sides;
    int active_count;
    std::vector<bool> vert_active;
    std::vector<SDL_FPoint> verts;
    float x, y;
    float angle;
    float angular_vel;
    float radius;
    float scale;
    float shrink_rate;
    float squish;
    float red_flash_alpha;
    bool immune;
    SDL_FColor base_color;
    bool active;
} Polygon;

typedef struct {
    float x, y;
    float vx, vy;
    float life;
    float max_life;
    SDL_FColor color;
    bool active;
} Particle;

typedef struct {
    float x, y;
    float vx, vy;
} FloatingPoint;

typedef struct {
    float x, y;
    float radius;
    float angle;
    float rot_speed;
    int sides;
    SDL_FColor color;
} BgPoly;

SDL_FColor get_pastel_color() {
    SDL_FColor c;
    c.r = 0.6f + (rand() % 40) / 100.0f;
    c.g = 0.6f + (rand() % 40) / 100.0f;
    c.b = 0.6f + (rand() % 40) / 100.0f;
    c.a = 0.7f;
    return c;
}

void draw_circle(SDL_Renderer* renderer, float x, float y, float r, SDL_FColor color) {
    if (r <= 0) return;
    SDL_Vertex verts[108];
    int count = 0;
    for (int i = 0; i < 36; i++) {
        float a1 = (i / 36.0f) * 2.0f * M_PI;
        float a2 = ((i + 1) / 36.0f) * 2.0f * M_PI;
        
        verts[count].position.x = x; verts[count].position.y = y; verts[count].color = color; count++;
        verts[count].position.x = x + cosf(a1) * r; verts[count].position.y = y + sinf(a1) * r; verts[count].color = color; count++;
        verts[count].position.x = x + cosf(a2) * r; verts[count].position.y = y + sinf(a2) * r; verts[count].color = color; count++;
    }
    SDL_RenderGeometry(renderer, NULL, verts, count, NULL, 0);
}

void draw_slider(SDL_Renderer* renderer, Slider* s) {
    SDL_FRect track_rect = {s->x, s->y + s->h / 2.0f - 2.0f, s->w, 4.0f};
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
    SDL_RenderFillRect(renderer, &track_rect);
    
    SDL_FRect active_rect = {s->x, s->y + s->h / 2.0f - 2.0f, s->w * s->val, 4.0f};
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 150);
    SDL_RenderFillRect(renderer, &active_rect);

    float handle_x = s->x + s->val * s->w;
    float handle_y = s->y + s->h / 2.0f;
    SDL_FColor handle_color = {1.0f, 1.0f, 1.0f, 1.0f};
    draw_circle(renderer, handle_x, handle_y, 8.0f, handle_color);
}

void get_vertex_world_pos(Polygon* p, int idx, float scale, float* out_x, float* out_y) {
    float c = cosf(p->angle);
    float s = sinf(p->angle);
    float vx = p->verts[idx].x * scale;
    float vy = p->verts[idx].y * scale;
    *out_x = vx * c - vy * s + p->x;
    *out_y = vx * s + vy * c + p->y;
}

float get_shrink_rate_mult(int setting) {
    if (setting == 0) return 0.6f;
    if (setting == 2) return 1.5f;
    return 1.0f;
}

void draw_text(SDL_Renderer* renderer, const char* text, float x, float y, float scale) {
    SDL_SetRenderScale(renderer, scale, scale);
    SDL_RenderDebugText(renderer, x / scale, y / scale, text);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

void draw_centered_text(SDL_Renderer* renderer, const char* text, float x, float y, float scale) {
    float text_w = (float)strlen(text) * 8.0f * scale;
    float draw_x = x - text_w / 2.0f;
    draw_text(renderer, text, draw_x, y, scale);
}

void draw_right_aligned_text(SDL_Renderer* renderer, const char* text, float right_x, float y, float scale) {
    float text_w = (float)strlen(text) * 8.0f * scale;
    float draw_x = right_x - text_w;
    draw_text(renderer, text, draw_x, y, scale);
}

const char* get_shape_name(int sides) {
    switch (sides) {
        case 3:  return "Triangle";
        case 4:  return "Square";
        case 5:  return "Pentagon";
        case 6:  return "Hexagon";
        case 7:  return "Heptagon";
        case 8:  return "Octagon";
        case 9:  return "Nonagon";
        case 10: return "Decagon";
        default: return NULL;
    }
}

void spawn_polygon(Polygon* p, int sides, float shrink_rate) {
    if (sides < MIN_SIDES) sides = MIN_SIDES;
    
    p->max_sides = sides;
    p->active_count = sides;
    p->vert_active.assign(sides, true);
    p->verts.resize(sides);
    
    p->x = WIN_W / 2.0f;
    p->y = WIN_H / 2.0f;
    p->radius = POLY_RADIUS;
    p->scale = INITIAL_SCALE;
    p->shrink_rate = shrink_rate;
    p->angle = (rand() % 360) * M_PI / 180.0f;
    
    float speed = 0.2f + (rand() % 40) / 100.0f;
    float dir = (rand() % 2) ? 1.0f : -1.0f;
    p->angular_vel = speed * dir;
    
    p->squish = 0.0f;
    p->red_flash_alpha = 0.0f;
    p->base_color = get_pastel_color();
    
    for (int i = 0; i < sides; i++) {
        float a = (i / (float)sides) * 2.0f * M_PI - M_PI / 2.0f;
        p->verts[i].x = cosf(a) * POLY_RADIUS;
        p->verts[i].y = sinf(a) * POLY_RADIUS;
    }
    
    p->immune = true;
    p->active = true;
}

void spawn_particles(Particle* parts, int* part_count, float x, float y, SDL_FColor color, int amount) {
    if (!g_particles_enabled) return;
    
    for (int i = 0; i < amount; i++) {
        if (*part_count >= MAX_PARTICLES) return;
        Particle* pt = &parts[*part_count];
        
        float a = (rand() % 360) * M_PI / 180.0f;
        float sp = 30.0f + (rand() % 60);
        pt->x = x; pt->y = y;
        pt->vx = cosf(a) * sp; pt->vy = sinf(a) * sp;
        pt->life = 0.8f + (rand() % 40) / 100.0f;
        pt->max_life = pt->life;
        pt->color = color;
        pt->color.a = 1.0f;
        pt->active = true;
        (*part_count)++;
    }
}

void remove_vertex(Polygon* p, Particle* parts, int* part_count) {
    if (!p->active || p->active_count <= 0) return;
    
    int count = 0;
    for (int i = 0; i < p->max_sides; i++) {
        if (p->vert_active[i]) count++;
    }
    if (count <= 0) return;
    
    int target = rand() % count;
    int found = 0;
    int idx = -1;
    
    for (int i = 0; i < p->max_sides; i++) {
        if (p->vert_active[i]) {
            if (found == target) {
                idx = i;
                break;
            }
            found++;
        }
    }
    if (idx == -1) return;
    
    p->vert_active[idx] = false;
    p->active_count--;
    
    float vx, vy;
    get_vertex_world_pos(p, idx, p->scale, &vx, &vy);
    
    SDL_FColor burst_color = {1.0f, 0.3f, 0.2f, 1.0f};
    spawn_particles(parts, part_count, vx, vy, burst_color, 15);
    
    if (p->active_count <= 0) {
        p->active = false;
        spawn_particles(parts, part_count, p->x, p->y, p->base_color, 25);
    }
}

void start_game(Polygon* poly, Particle* parts, int* part_count, int start_sides, float* shrink_rate, int* clicks, int speed_setting) {
    *shrink_rate = BASE_SHRINK_RATE * get_shrink_rate_mult(speed_setting);
    *clicks = 0;
    *part_count = 0;
    for (int i = 0; i < MAX_PARTICLES; i++) parts[i].active = false;
    spawn_polygon(poly, start_sides, *shrink_rate);
}

int main(int argc, char* argv[]) {
    srand(time(NULL));
    
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("Could not initialize SDL: %s", SDL_GetError());
        return 1;
    }
    
    SDL_Window* window = NULL;
    SDL_Renderer* renderer = NULL;
    if (!SDL_CreateWindowAndRenderer("Cozy Polygons", WIN_W, WIN_H, 0, &window, &renderer)) {
        SDL_Log("Could not create window/renderer: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    SDL_SetRenderLogicalPresentation(renderer, WIN_W, WIN_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetWindowFullscreenMode(window, NULL);

    desired_spec.format = (SDL_AudioFormat)SDL_AUDIO_F32;
    desired_spec.channels = 2;
    desired_spec.freq = 48000;
    
    audio_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec);
    if (!audio_dev) {
        SDL_Log("AUDIO ERROR: Failed to open audio device: %s", SDL_GetError());
    } else {
        SDL_ResumeAudioDevice(audio_dev); 
        SDL_Log("AUDIO: Audio device opened successfully.");
    }
    
    click_sfx_wav = load_wav("clicker.wav");
    change_music("menu_background.wav");

    Particle parts[MAX_PARTICLES];
    int part_count = 0;
    for (int i = 0; i < MAX_PARTICLES; i++) parts[i].active = false;
    
    float shrink_rate = BASE_SHRINK_RATE;
    int clicks_this_polygon = 0;
    int start_sides = MIN_SIDES;
    bool is_decremental_mode = false;
    
    int state = STATE_MENU;
    char input_buffer[16] = {0};
    int input_len = 0;
    
    Polygon poly;
    poly.active = false; 

    std::vector<FloatingPoint> menu_verts;
    std::vector<BgPoly> bg_polys;
    for (int i = 0; i < 6; i++) {
        BgPoly bp;
        bp.x = 100.0f + (rand() % (WIN_W - 200));
        bp.y = 100.0f + (rand() % (WIN_H - 200));
        bp.radius = 40.0f + (rand() % 100);
        bp.angle = (rand() % 360) * M_PI / 180.0f;
        bp.rot_speed = ((rand() % 50) - 25) * 0.01f;
        bp.sides = 3 + (rand() % 6);
        bp.color.r = 0.2f + (rand()%20)/100.0f;
        bp.color.g = 0.2f + (rand()%20)/100.0f;
        bp.color.b = 0.4f + (rand()%20)/100.0f;
        bp.color.a = 0.1f + (rand()%10)/100.0f;
        bg_polys.push_back(bp);
    }

    Slider music_slider = {250.0f, 250.0f, 300.0f, 30.0f, 0.8f, false};
    Slider sfx_slider = {250.0f, 310.0f, 300.0f, 30.0f, 1.0f, false};
    Slider dur_slider = {250.0f, 370.0f, 300.0f, 30.0f, (0.1f - 0.01f) / 0.99f, false};
    Slider alpha_slider = {250.0f, 430.0f, 300.0f, 30.0f, (0.5f - 0.05f) / 0.95f, false};
    Slider* all_sliders[] = {&music_slider, &sfx_slider, &dur_slider, &alpha_slider};

    Uint64 last = SDL_GetTicksNS();
    float elapsed_time = 0.0f;
    bool running = true;

    while (running) {
        Uint64 now = SDL_GetTicksNS();
        float dt = (now - last) / 1000000000.0f;
        last = now;
        elapsed_time += dt;
        
        if (music_stream) {
            SDL_SetAudioStreamGain(music_stream, music_slider.val);
        }
        update_audio();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }

            if (state == STATE_MENU || state == STATE_SETTINGS_MENU) {
                if (state == STATE_SETTINGS_MENU) {
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                        for (int i = 0; i < 4; i++) {
                            float hx = all_sliders[i]->x + all_sliders[i]->val * all_sliders[i]->w;
                            float hy = all_sliders[i]->y + all_sliders[i]->h / 2.0f;
                            float dx = event.button.x - hx;
                            float dy = event.button.y - hy;
                            if (dx*dx + dy*dy < 15*15) {
                                all_sliders[i]->dragging = true;
                            }
                        }
                    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                        for (int i = 0; i < 4; i++) {
                            all_sliders[i]->dragging = false;
                        }
                    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                        for (int i = 0; i < 4; i++) {
                            if (all_sliders[i]->dragging) {
                                all_sliders[i]->val = (event.motion.x - all_sliders[i]->x) / all_sliders[i]->w;
                                if (all_sliders[i]->val < 0.0f) all_sliders[i]->val = 0.0f;
                                if (all_sliders[i]->val > 1.0f) all_sliders[i]->val = 1.0f;
                            }
                        }
                    } else if (event.type == SDL_EVENT_KEY_DOWN) {
                        if (event.key.key == SDLK_1) {
                            g_particles_enabled = !g_particles_enabled;
                        } else if (event.key.key == SDLK_2) {
                            g_shrink_speed_setting = (g_shrink_speed_setting + 1) % 3;
                        } else if (event.key.key == SDLK_3) {
                            bool is_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                            SDL_SetWindowFullscreen(window, !is_fullscreen);
                        } else if (event.key.key == SDLK_4) {
                            g_pulse_color_idx = (g_pulse_color_idx + 1) % num_pulse_colors;
                        } else if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                            state = STATE_MENU;
                            change_music("menu_background.wav");
                        }
                    }
                }
                
                if (state == STATE_MENU) {
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                        FloatingPoint p;
                        p.x = event.button.x;
                        p.y = event.button.y;
                        p.vx = ((rand() % 100) - 50) * 1.5f;
                        p.vy = ((rand() % 100) - 50) * 1.5f;
                        menu_verts.push_back(p);
                    }
                    
                    if (event.type == SDL_EVENT_KEY_DOWN) {
                        if (event.key.key == SDLK_1 || event.key.key == SDLK_KP_1) {
                            is_decremental_mode = false;
                            start_sides = MIN_SIDES;
                            start_game(&poly, parts, &part_count, start_sides, &shrink_rate, &clicks_this_polygon, g_shrink_speed_setting);
                            state = STATE_PLAYING;
                            change_music("endless_background.wav");
                        } else if (event.key.key == SDLK_2 || event.key.key == SDLK_KP_2) {
                            input_len = 0;
                            input_buffer[0] = '\0';
                            state = STATE_INPUT;
                        } else if (event.key.key == SDLK_3 || event.key.key == SDLK_KP_3) {
                            state = STATE_SETTINGS_MENU;
                        }
                    }
                }
            } 
            else if (state == STATE_INPUT) {
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        state = STATE_MENU;
                    } else if (event.key.scancode == SDL_SCANCODE_RETURN || event.key.scancode == SDL_SCANCODE_KP_ENTER) {
                        int val = atoi(input_buffer);
                        if (val >= MIN_SIDES) {
                            is_decremental_mode = true;
                            start_sides = val;
                            start_game(&poly, parts, &part_count, start_sides, &shrink_rate, &clicks_this_polygon, g_shrink_speed_setting);
                            state = STATE_PLAYING;
                            change_music("decremental_background.wav");
                        }
                    } else if (event.key.scancode == SDL_SCANCODE_BACKSPACE) {
                        if (input_len > 0) {
                            input_len--;
                            input_buffer[input_len] = '\0';
                        }
                    } else {
                        SDL_Keycode sym = event.key.key;
                        if ((sym >= SDLK_0 && sym <= SDLK_9) || (sym >= SDLK_KP_0 && sym <= SDLK_KP_9)) {
                            if (input_len < 15) {
                                char c = (sym >= SDLK_0 && sym <= SDLK_9) ? (char)('0' + (sym - SDLK_0)) : (char)('0' + (sym - SDLK_KP_0));
                                input_buffer[input_len++] = c;
                                input_buffer[input_len] = '\0';
                            }
                        }
                    }
                }
            } 
            else if (state == STATE_PLAYING) {
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    state = STATE_PAUSE;
                } else {
                    bool click_input = (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_SPACE) ||
                                      (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT);
                                      
                    if (click_input) {
                        play_sfx(sfx_slider.val);
                        clicks_this_polygon++;
                        
                        if (poly.max_sides >= 3) {
                            poly.red_flash_alpha = g_pulse_max_alpha;
                        }
                        poly.squish = 0.25f;
                        
                        if (poly.immune) {
                            poly.shrink_rate *= 1.1f;
                        } else {
                            remove_vertex(&poly, parts, &part_count);
                            
                            if (!poly.active) {
                                if (clicks_this_polygon > poly.max_sides) {
                                    shrink_rate += SHRINK_RATE_STEP;
                                    if (shrink_rate > MAX_SHRINK_RATE) shrink_rate = MAX_SHRINK_RATE;
                                }
                                
                                if (is_decremental_mode) {
                                    int next_sides = poly.max_sides - 1;
                                    if (next_sides < MIN_SIDES) {
                                        state = STATE_WIN;
                                        poly.active = false;
                                    } else {
                                        spawn_polygon(&poly, next_sides, shrink_rate);
                                        clicks_this_polygon = 0;
                                    }
                                } else {
                                    spawn_polygon(&poly, poly.max_sides + 1, shrink_rate);
                                    clicks_this_polygon = 0;
                                }
                            }
                        }
                    }
                }
            } 
            else if (state == STATE_PAUSE) {
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE || event.key.key == SDLK_1 || event.key.key == SDLK_KP_1) {
                        state = STATE_PLAYING;
                    } else if (event.key.key == SDLK_2 || event.key.key == SDLK_KP_2) {
                        start_game(&poly, parts, &part_count, start_sides, &shrink_rate, &clicks_this_polygon, g_shrink_speed_setting);
                        state = STATE_PLAYING;
                    } else if (event.key.key == SDLK_3 || event.key.key == SDLK_KP_3) {
                        state = STATE_MENU;
                        poly.active = false;
                        change_music("menu_background.wav");
                    }
                }
            } 
            else if (state == STATE_WIN || state == STATE_GAME_OVER) {
                bool click_input = (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_SPACE) ||
                                  (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) ||
                                  (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE);
                                  
                if (click_input) {
                    state = STATE_MENU;
                    poly.active = false;
                    change_music("menu_background.wav");
                }
            }
        }

        if (state == STATE_MENU || state == STATE_SETTINGS_MENU) {
            for (auto& bp : bg_polys) {
                bp.angle += bp.rot_speed * dt;
            }
            for (auto& p : menu_verts) {
                p.x += p.vx * dt;
                p.y += p.vy * dt;
                if (p.x < 0) { p.x = 0; p.vx *= -1; }
                if (p.x > WIN_W) { p.x = WIN_W; p.vx *= -1; }
                if (p.y < 0) { p.y = 0; p.vy *= -1; }
                if (p.y > WIN_H) { p.y = WIN_H; p.vy *= -1; }
            }
        }

        if (state == STATE_PLAYING && poly.active) {
            poly.angle += poly.angular_vel * dt;
            poly.squish *= expf(-SQUISH_DECAY_RATE * dt);
            
            if (poly.red_flash_alpha > 0.0f) {
                poly.red_flash_alpha -= (g_pulse_max_alpha / g_pulse_duration) * dt;
                if (poly.red_flash_alpha < 0.0f) poly.red_flash_alpha = 0.0f;
            }
            
            poly.scale -= poly.shrink_rate * dt;
            if (poly.scale < 0.0f) poly.scale = 0.0f;
            
            if (poly.immune) {
                bool all_on_screen = true;
                for (int j = 0; j < poly.max_sides; j++) {
                    if (!poly.vert_active[j]) continue;
                    float vx, vy;
                    get_vertex_world_pos(&poly, j, poly.scale, &vx, &vy);
                    if (vx < 0 || vx > WIN_W || vy < 0 || vy > WIN_H) {
                        all_on_screen = false;
                        break;
                    }
                }
                if (all_on_screen) poly.immune = false;
            }
            
            if (!poly.immune && poly.scale <= LOSE_SCALE) {
                state = STATE_GAME_OVER;
            }
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        if (state == STATE_MENU || state == STATE_SETTINGS_MENU) {
            for (const auto& bp : bg_polys) {
                std::vector<SDL_FPoint> bg_pts(bp.sides);
                for (int i = 0; i < bp.sides; i++) {
                    float a = (i / (float)bp.sides) * 2.0f * M_PI + bp.angle;
                    bg_pts[i].x = bp.x + cosf(a) * bp.radius;
                    bg_pts[i].y = bp.y + sinf(a) * bp.radius;
                }
                SDL_SetRenderDrawColor(renderer, (Uint8)(bp.color.r*255), (Uint8)(bp.color.g*255), (Uint8)(bp.color.b*255), (Uint8)(bp.color.a*255));
                SDL_RenderLines(renderer, bg_pts.data(), bp.sides);
                SDL_FPoint closing_line[2] = { bg_pts[bp.sides - 1], bg_pts[0] };
                SDL_RenderLines(renderer, closing_line, 2);
            }

            if (menu_verts.size() > 1) {
                std::vector<SDL_FPoint> m_pts(menu_verts.size());
                for (size_t i = 0; i < menu_verts.size(); i++) {
                    m_pts[i].x = menu_verts[i].x;
                    m_pts[i].y = menu_verts[i].y;
                }
                SDL_SetRenderDrawColor(renderer, 153, 153, 204, 102);
                SDL_RenderLines(renderer, m_pts.data(), m_pts.size());
                SDL_FPoint close_line[2] = { m_pts[m_pts.size() - 1], m_pts[0] };
                SDL_RenderLines(renderer, close_line, 2);
            }
            
            SDL_FColor dot_col = {1.0f, 1.0f, 1.0f, 0.8f};
            for (const auto& p : menu_verts) {
                draw_circle(renderer, p.x, p.y, 5.0f, dot_col);
            }

            SDL_FColor text_bg_col = {0.0f, 0.0f, 0.0f, 0.5f};
            SDL_Vertex bg_ov[4] = {
                {0, 100, text_bg_col},
                {(float)WIN_W, 100, text_bg_col},
                {(float)WIN_W, 600, text_bg_col},
                {0, 600, text_bg_col}
            };
            SDL_RenderGeometry(renderer, NULL, bg_ov, 4, NULL, 0);

            if (state == STATE_MENU) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                draw_centered_text(renderer, "COZY POLYGONS", WIN_W / 2.0f, 120.0f, 4.0f);
                
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
                draw_centered_text(renderer, "Click anywhere to add floating vertices", WIN_W / 2.0f, 180.0f, 1.2f);
                
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
                draw_centered_text(renderer, "[1] Endless Mode", WIN_W / 2.0f, 260.0f, 2.0f);
                draw_centered_text(renderer, "[2] Decremental Mode", WIN_W / 2.0f, 310.0f, 2.0f);
                draw_centered_text(renderer, "[3] Settings", WIN_W / 2.0f, 360.0f, 2.0f);
            } 
            else if (state == STATE_SETTINGS_MENU) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                draw_centered_text(renderer, "SETTINGS", WIN_W / 2.0f, 100.0f, 4.0f);
                
                const char* speed_names[] = {"Slow", "Normal", "Fast"};
                char buf[64];

                snprintf(buf, 64, "[1] Particles: %s", g_particles_enabled ? "ON" : "OFF");
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
                draw_centered_text(renderer, buf, WIN_W / 2.0f, 180.0f, 1.5f);
                
                snprintf(buf, 64, "[2] Shrink Speed: %s", speed_names[g_shrink_speed_setting]);
                draw_centered_text(renderer, buf, WIN_W / 2.0f, 210.0f, 1.5f);

                bool is_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                snprintf(buf, 64, "[3] Fullscreen: %s", is_fullscreen ? "ON" : "OFF");
                draw_centered_text(renderer, buf, WIN_W / 2.0f, 490.0f, 1.5f);

                snprintf(buf, 64, "[4] Pulse Color: %s", pulse_color_options[g_pulse_color_idx].name);
                draw_centered_text(renderer, buf, WIN_W / 2.0f, 520.0f, 1.5f);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 150);
                snprintf(buf, 64, "Music Volume: %d%%", (int)(music_slider.val * 100));
                draw_text(renderer, buf, 50.0f, 255.0f, 1.5f);
                draw_slider(renderer, &music_slider);
                
                snprintf(buf, 64, "SFX Volume: %d%%", (int)(sfx_slider.val * 100));
                draw_text(renderer, buf, 50.0f, 315.0f, 1.5f);
                draw_slider(renderer, &sfx_slider);

                g_pulse_duration = 0.01f + dur_slider.val * 0.99f;
                snprintf(buf, 64, "Pulse Duration: %.2fs", g_pulse_duration);
                draw_text(renderer, buf, 50.0f, 375.0f, 1.5f);
                draw_slider(renderer, &dur_slider);

                g_pulse_max_alpha = 0.05f + alpha_slider.val * 0.95f;
                snprintf(buf, 64, "Pulse Max Alpha: %.2f", g_pulse_max_alpha);
                draw_text(renderer, buf, 50.0f, 435.0f, 1.5f);
                draw_slider(renderer, &alpha_slider);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
                draw_centered_text(renderer, "[ESC] Back", WIN_W / 2.0f, 560.0f, 1.2f);
            }
        } 
        else if (state == STATE_INPUT) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
            draw_centered_text(renderer, "DECREMENTAL MODE", WIN_W / 2.0f, WIN_H / 2.0f - 120.0f, 3.0f);
            draw_centered_text(renderer, "Enter starting sides (min 3):", WIN_W / 2.0f, WIN_H / 2.0f - 40.0f, 1.5f);
            
            SDL_FColor bg_col = {1.0f, 1.0f, 1.0f, 0.05f};
            draw_circle(renderer, WIN_W / 2.0f, WIN_H / 2.0f + 40.0f, 60.0f, bg_col);

            char display_text[32];
            snprintf(display_text, sizeof(display_text), "%s", input_buffer);
            if (fmod(elapsed_time, 1.0f) < 0.6f) {
                strcat(display_text, "|");
            }
            
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            draw_centered_text(renderer, display_text, WIN_W / 2.0f, WIN_H / 2.0f + 32.0f, 3.0f);
            
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
            draw_centered_text(renderer, "Press ENTER to start, ESC to go back", WIN_W / 2.0f, WIN_H / 2.0f + 120.0f, 1.2f);
        } 
        else if (state == STATE_PLAYING || state == STATE_PAUSE || state == STATE_WIN || state == STATE_GAME_OVER) {
            float center_pulse = 1.0f + sinf(elapsed_time * PULSE_SPEED) * 0.1f;
            SDL_FColor glow_col1 = {1.0f, 1.0f, 1.0f, 0.05f};
            draw_circle(renderer, WIN_W / 2.0f, WIN_H / 2.0f, 80.0f * center_pulse, glow_col1);
            SDL_FColor glow_col2 = {1.0f, 1.0f, 1.0f, 0.15f};
            draw_circle(renderer, WIN_W / 2.0f, WIN_H / 2.0f, 40.0f * center_pulse, glow_col2);

            if (poly.active) {
                float display_scale = poly.scale + poly.squish;
                if (display_scale < 0.0f) display_scale = 0.0f;

                std::vector<SDL_FPoint> active_world;
                for (int j = 0; j < poly.max_sides; j++) {
                    if (poly.vert_active[j]) {
                        float vx, vy;
                        get_vertex_world_pos(&poly, j, display_scale, &vx, &vy);
                        active_world.push_back({vx, vy});
                    }
                }

                if (poly.red_flash_alpha > 0.0f && active_world.size() >= 3) {
                    PulseColorDef pc = pulse_color_options[g_pulse_color_idx];
                    SDL_FColor fill_col = {pc.r, pc.g, pc.b, poly.red_flash_alpha};
                    int num = (int)active_world.size();
                    SDL_Vertex tris[384];
                    int tri_idx = 0;
                    
                    for (int t = 0; t < num - 1; t++) {
                        tris[tri_idx].position = {poly.x, poly.y}; tris[tri_idx].color = fill_col; tri_idx++;
                        tris[tri_idx].position = active_world[t]; tris[tri_idx].color = fill_col; tri_idx++;
                        tris[tri_idx].position = active_world[t + 1]; tris[tri_idx].color = fill_col; tri_idx++;
                    }
                    tris[tri_idx].position = {poly.x, poly.y}; tris[tri_idx].color = fill_col; tri_idx++;
                    tris[tri_idx].position = active_world[num - 1]; tris[tri_idx].color = fill_col; tri_idx++;
                    tris[tri_idx].position = active_world[0]; tris[tri_idx].color = fill_col; tri_idx++;
                    
                    SDL_RenderGeometry(renderer, NULL, tris, tri_idx, NULL, 0);
                }

                SDL_FColor out_col = poly.base_color;
                SDL_SetRenderDrawColor(renderer, (Uint8)(out_col.r*255), (Uint8)(out_col.g*255), (Uint8)(out_col.b*255), (Uint8)(out_col.a*255));

                for (int j = 0; j < poly.max_sides; j++) {
                    if (!poly.vert_active[j]) continue;
                    int k = -1;
                    for (int l = 1; l <= poly.max_sides; l++) {
                        int next_idx = (j + l) % poly.max_sides;
                        if (poly.vert_active[next_idx]) {
                            k = next_idx;
                            break;
                        }
                    }
                    if (k == -1 || k == j) continue;
                    
                    float v1x, v1y, v2x, v2y;
                    get_vertex_world_pos(&poly, j, display_scale, &v1x, &v1y);
                    get_vertex_world_pos(&poly, k, display_scale, &v2x, &v2y);
                    
                    SDL_FPoint line[2] = {{v1x, v1y}, {v2x, v2y}};
                    SDL_RenderLines(renderer, line, 2);
                }

                SDL_FColor dot_col = {1.0f, 1.0f, 1.0f, 1.0f};
                for (int j = 0; j < poly.max_sides; j++) {
                    if (poly.vert_active[j]) {
                        float vx, vy;
                        get_vertex_world_pos(&poly, j, display_scale, &vx, &vy);
                        draw_circle(renderer, vx, vy, 4.0f, dot_col);
                    }
                }
            }

            float particle_drag = expf(-PARTICLE_DRAG_RATE * dt);
            for (int i = 0; i < part_count; i++) {
                Particle* pt = &parts[i];
                if (!pt->active) continue;
                pt->x += pt->vx * dt; pt->y += pt->vy * dt;
                pt->vx *= particle_drag; pt->vy *= particle_drag;
                pt->life -= dt;
                if (pt->life <= 0) {
                    pt->active = false;
                    continue;
                }
                pt->color.a = pt->life / pt->max_life;
                draw_circle(renderer, pt->x, pt->y, 3.0f, pt->color);
            }
            for (int i = part_count - 1; i >= 0; i--) {
                if (!parts[i].active) {
                    parts[i] = parts[part_count - 1];
                    part_count--;
                }
            }

            if (state == STATE_PLAYING) {
                int next_sides = is_decremental_mode ? (poly.max_sides - 1) : (poly.max_sides + 1);
                
                if (!is_decremental_mode || next_sides >= MIN_SIDES) {
                    SDL_FColor prev_bg = {1.0f, 1.0f, 1.0f, 0.06f};
                    draw_circle(renderer, PREVIEW_X, PREVIEW_Y, PREVIEW_RADIUS + 14.0f, prev_bg);

                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
                    draw_right_aligned_text(renderer, "NEXT UP", PREVIEW_X + PREVIEW_RADIUS + 10.0f, PREVIEW_Y - PREVIEW_RADIUS - 24.0f, 1.2f);

                    float preview_angle = elapsed_time * PREVIEW_SPIN_SPEED;
                    SDL_FColor preview_col = {0.7f, 0.7f, 0.85f, 0.6f};
                    int pts_count = next_sides;
                    std::vector<SDL_FPoint> preview_pts(pts_count);
                    for (int i = 0; i < pts_count; i++) {
                        float a = (i / (float)pts_count) * 2.0f * M_PI - M_PI / 2.0f + preview_angle;
                        preview_pts[i].x = PREVIEW_X + cosf(a) * PREVIEW_RADIUS;
                        preview_pts[i].y = PREVIEW_Y + sinf(a) * PREVIEW_RADIUS;
                    }
                    
                    SDL_SetRenderDrawColor(renderer, (Uint8)(preview_col.r*255), (Uint8)(preview_col.g*255), (Uint8)(preview_col.b*255), (Uint8)(preview_col.a*255));
                    SDL_RenderLines(renderer, preview_pts.data(), pts_count);
                    SDL_FPoint close_line[2] = { preview_pts[pts_count - 1], preview_pts[0] };
                    SDL_RenderLines(renderer, close_line, 2);

                    SDL_FColor preview_dot = {1.0f, 1.0f, 1.0f, 0.5f};
                    for (int i = 0; i < pts_count; i++) {
                        draw_circle(renderer, preview_pts[i].x, preview_pts[i].y, 2.0f, preview_dot);
                    }

                    char count_text[32];
                    snprintf(count_text, 32, "%d sides", next_sides);
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 140);
                    draw_centered_text(renderer, count_text, PREVIEW_X, PREVIEW_Y + PREVIEW_RADIUS + 16.0f, 1.0f);

                    const char* shape_name = get_shape_name(next_sides);
                    if (shape_name) {
                        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
                        draw_centered_text(renderer, shape_name, PREVIEW_X, PREVIEW_Y + PREVIEW_RADIUS + 32.0f, 0.9f);
                    }
                } else {
                    SDL_SetRenderDrawColor(renderer, 255, 255, 200, 180);
                    draw_centered_text(renderer, "FINISH!", PREVIEW_X, PREVIEW_Y - 10.0f, 1.5f);
                }

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 100);
                char current_text[64];
                snprintf(current_text, 64, "Current: %d sides", poly.max_sides);
                draw_text(renderer, current_text, 16.0f, 16.0f, 1.5f);

                if (poly.immune) {
                    SDL_SetRenderDrawColor(renderer, 255, 255, 200, 80);
                    draw_text(renderer, "Entering...", 16.0f, 36.0f, 1.5f);
                } else {
                    char remain_text[64];
                    snprintf(remain_text, 64, "Remaining: %d / %d", poly.active_count, poly.max_sides);
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 80);
                    draw_text(renderer, remain_text, 16.0f, 36.0f, 1.5f);
                }
            }

            if (state == STATE_PAUSE) {
                SDL_FColor overlay_col = {0.0f, 0.0f, 0.0f, 0.6f};
                SDL_Vertex ov[4] = {
                    {0, 0, overlay_col},
                    {(float)WIN_W, 0, overlay_col},
                    {(float)WIN_W, (float)WIN_H, overlay_col},
                    {0, (float)WIN_H, overlay_col}
                };
                SDL_RenderGeometry(renderer, NULL, ov, 4, NULL, 0);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                draw_centered_text(renderer, "PAUSED", WIN_W / 2.0f, WIN_H / 2.0f - 80.0f, 4.0f);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
                draw_centered_text(renderer, "[1] Resume", WIN_W / 2.0f, WIN_H / 2.0f + 20.0f, 2.0f);
                draw_centered_text(renderer, "[2] Restart Run", WIN_W / 2.0f, WIN_H / 2.0f + 60.0f, 2.0f);
                draw_centered_text(renderer, "[3] Main Menu", WIN_W / 2.0f, WIN_H / 2.0f + 100.0f, 2.0f);
                
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
                draw_centered_text(renderer, "(or press ESC to resume)", WIN_W / 2.0f, WIN_H / 2.0f + 150.0f, 1.2f);
            }

            if (state == STATE_GAME_OVER) {
                SDL_FColor overlay_col = {0.0f, 0.0f, 0.0f, 0.4f};
                SDL_Vertex ov[4] = {
                    {0, 0, overlay_col},
                    {(float)WIN_W, 0, overlay_col},
                    {(float)WIN_W, (float)WIN_H, overlay_col},
                    {0, (float)WIN_H, overlay_col}
                };
                SDL_RenderGeometry(renderer, NULL, ov, 4, NULL, 0);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                draw_centered_text(renderer, "GAME OVER", WIN_W / 2.0f, WIN_H / 2.0f - 40.0f, 4.0f);

                char got_text[64];
                snprintf(got_text, 64, "Failed on %d-gon", poly.max_sides);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
                draw_centered_text(renderer, got_text, WIN_W / 2.0f, WIN_H / 2.0f + 30.0f, 2.0f);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
                draw_centered_text(renderer, "Click or press Space to return to menu", WIN_W / 2.0f, WIN_H / 2.0f + 80.0f, 1.5f);
            }
            else if (state == STATE_WIN) {
                SDL_FColor overlay_col = {0.0f, 0.0f, 0.0f, 0.4f};
                SDL_Vertex ov[4] = {
                    {0, 0, overlay_col},
                    {(float)WIN_W, 0, overlay_col},
                    {(float)WIN_W, (float)WIN_H, overlay_col},
                    {0, (float)WIN_H, overlay_col}
                };
                SDL_RenderGeometry(renderer, NULL, ov, 4, NULL, 0);

                SDL_SetRenderDrawColor(renderer, 200, 255, 200, 255);
                draw_centered_text(renderer, "YOU WIN!", WIN_W / 2.0f, WIN_H / 2.0f - 40.0f, 4.0f);

                char got_text[64];
                snprintf(got_text, 64, "Cleared down from %d-gon", start_sides);
                SDL_SetRenderDrawColor(renderer, 200, 255, 200, 180);
                draw_centered_text(renderer, got_text, WIN_W / 2.0f, WIN_H / 2.0f + 30.0f, 2.0f);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
                draw_centered_text(renderer, "Click or press Space to return to menu", WIN_W / 2.0f, WIN_H / 2.0f + 80.0f, 1.5f);
            }
        }

        SDL_RenderPresent(renderer);
    }

    if (click_sfx_wav.data) free(click_sfx_wav.data);
    if (current_music_wav.data) free(current_music_wav.data);
    if (music_stream) {
        SDL_UnbindAudioStream(music_stream);
        SDL_DestroyAudioStream(music_stream);
    }
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    return 0;
}