#include <iostream>
#include <string>
#include "util/music-player.h"

class Sound{
public:
    struct SoundInfo{
        SoundInfo():
            frequency(22050),
            channels(2),
            format(0){
            }

        int frequency;
        int channels;

        /* format is mostly for SDL */
        int format;
    };
    static SoundInfo Info;
};

Sound::SoundInfo Sound::Info;

using namespace std;

void play(const string & path){
    Util::Mp3Player player(path);
    player.play();

    while (true){
        SDL_Delay(1);
    }
}

void initialize(int rate){
    SDL_Init(SDL_INIT_AUDIO);
    atexit(SDL_Quit);

    int audio_rate = rate;
    // int audio_rate = 22050;
    Uint16 audio_format = AUDIO_S16; 
    // Uint16 audio_format = MIX_DEFAULT_FORMAT; 
    int audio_channels = 2;
    int audio_buffers = 4096;
    // int audio_buffers = 44100;
    if (Mix_OpenAudio(audio_rate, audio_format, audio_channels, audio_buffers)) {
        printf("Unable to open audio: %s!\n", Mix_GetError());
        // exit(1);
    }

    /* use the frequency enforced by the audio system */
    Mix_QuerySpec(&audio_rate, &audio_format, &audio_channels);
    Sound::Info.frequency = audio_rate;
    Sound::Info.channels = audio_channels;
    Sound::Info.format = audio_format;
}

int main(int argc, char ** argv){
    if (argc < 2){
        cout << "Give an audio file as an argument" << endl;
        return 0;
    }
    initialize(48000);
    play(argv[1]);
}