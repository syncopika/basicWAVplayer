#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h> // important that this comes before commctrl.h
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <SDL.h>
#include <stdlib.h>
#include <commctrl.h>
//#include <ctime>

// SoundTouch code by Olli Parviainen
#include "soundtouch/SoundTouch.h"

// give some identifiers for the GUI components 
#include "resources.h"

// pitch shifting code by Stephan Bernsee 
//#include "smbPitchShift.h"

// filters from @vfiksdal (https://github.com/vfiksdal/rtfir/tree/main)
#include "filters/rtfir.hpp"

// default sample rate 
#define DEF_SAMPLE_RATE 44100

#define GUI_WIDTH 600
#define GUI_HEIGHT 250
#define VISUALIZER_WINDOW_WIDTH 510
#define VISUALIZER_WINDOW_HEIGHT 180

// max value for pitch shift
#define MAX_PITCH_SHIFT 5

// register window 
const char g_szClassName[] = "mainGUI";

// handler variable for the window 
HWND hwnd;

// use Tahoma font for the text 
// this HFONT object needs to be deleted (via DeleteObject) when program ends 
HFONT hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, 
      OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 
      DEFAULT_PITCH | FF_DONTCARE, TEXT("Tahoma")
);

// keep track of audiodevice id - only keep one around!
SDL_AudioDeviceID currentDeviceID;

// keep track of thread designated to play audio. 
HANDLE audioThread;

// the sdl window and renderer for visualization 
//SDL_Window* sdlWnd;
//SDL_Renderer* sdlRend;

// amount to pitch shift
int pitchShiftValue = 0;

// audio data struct that callback will use
struct AudioData {
    Uint8* position;
    Uint32 length;
};

// struct to hold filename and sample rate info that can be passed to a thread when playing wav audio 
struct AudioParams {
    char* filename;
    int sampleRate;
};

// get the name of the file 
std::string getFilename(std::string file){
    // find the last instance of a slash, if any 
    // if there isn't any, then just return file
    size_t lastSlashIndex = 0;
    lastSlashIndex = file.find_last_of('\\');
    
    // getting just the length of the file's name
    // need to add 1 because of 0-index, subract 4 for .wav 
    int filenameLength = file.size() - (lastSlashIndex + 1) - 4; 
    
    if(lastSlashIndex != std::string::npos){
        // get file name    
        return file.substr(lastSlashIndex+1, filenameLength); 
    }else{
        // no slash found - just remove the .wav extension 
        std::cout << file << std::endl;
        return file.substr(0, file.size() - 4);
    }
    
}

// extract int from std::string
// THIS IS NOT SAFE FROM OVERFLOWS!!! 
int extractInt(std::string str){
    int total = 0;
    int place = 1;
    
    for(std::string::reverse_iterator it = str.rbegin(); it < str.rend(); ++it){
        if(isdigit(*it)){
            total += (place * (int)(*it - '0'));
            place *= 10;
        }
    }
    //std::cout << total << std::endl;
    return total;
}

int getCurrentSampleRate(){
    int sampleRate = 0;

    HWND sampleRateTextBox = GetDlgItem(hwnd, ID_SPECIFY_SAMPLE_RATE);
    int textLengthSample = GetWindowTextLength(sampleRateTextBox);

    TCHAR* sampleRateText = new TCHAR[textLengthSample + 1];
    GetWindowText(sampleRateTextBox, sampleRateText, textLengthSample + 1);

    // get the sample rate as a string
    std::string sampleRateString = std::string((char*)sampleRateText);

    // extract the int value from the string
    sampleRate = extractInt(sampleRateString);

    delete[] sampleRateText;

    return sampleRate;
}

int interpolateLength(float newX, float x1, float y1, float x2, float y2){
    return std::round(y1 + (newX - x1) * ((y2-y1)/(x2-x1)));
}

/* get a vector of the evenly-distributed indices to sample from the dataset based on num samples desired (for visuzalization)
// https://stackoverflow.com/questions/9873626/choose-m-evenly-spaced-elements-from-a-sequence-of-length-n
std::vector<int> getSampleIndices(int dataLen, int numSamples){
    std::vector<int> result;
    int samples = (numSamples <= dataLen) ? numSamples : dataLen; 
    for(int i = 0; i < samples; i++){
        int idx = std::floor((i*dataLen)/samples) + std::floor(dataLen/(2*samples));
        result.push_back(idx);
    }
    return result;
}*/

// define an audio callback that SDL_AudioSpec will use
void audioCallback(void* userData, Uint8* stream, int length){
    AudioData* audio = (AudioData*)userData;
    float* streamF = (float*)stream;
    
    if(audio->length == 0){
        // stop playing stream here
        return;
    }
    
    // length is number of bytes of userData's audio data
    Uint32 len = (Uint32)length;
    
    if(len > audio->length){
        len = audio->length;
    }
    
    // copy len bytes from audio stream at audio->position to stream buffer
    SDL_memcpy(streamF, audio->position, len);
    
    audio->position += len;
    audio->length -= len;
}

// pitch shifting works with Stephan Bernsee's solution, but note that it's slow. just don't think it's broken...
// use gdb to run it and check
// Olli Parviainen's SoundTouch works well and seems pretty fast (for my demo sample) but gets slower with larger audio files (which is probably expected?).
std::vector<float> pitchShift(Uint8* wavStart, Uint32 wavLength, soundtouch::SoundTouch& soundTouch){
    // convert audio data to F32 
    SDL_AudioCVT cvt;
    SDL_BuildAudioCVT(&cvt, AUDIO_S16, 2, DEF_SAMPLE_RATE, AUDIO_F32, 2, DEF_SAMPLE_RATE);
    cvt.len = wavLength;
    cvt.buf = (Uint8 *)SDL_malloc(cvt.len * cvt.len_mult);
    
    // copy current audio data to the buffer (dest, src, len)
    SDL_memcpy(cvt.buf, wavStart, wavLength); // wavLength is the total number of bytes the audio data takes up
    SDL_ConvertAudio(&cvt);
    
    // audio data is now in float form!
    float* newData = (float*)cvt.buf;
    
    int floatBufLen = (int)cvt.len_cvt / 4; // 4 bytes per float
    
    int numChannels = 2; // assuming 2 channels. TODO: don't assume this
    
    int buffSizeSamples = floatBufLen / numChannels;
    
    // https://stackoverflow.com/questions/56370244/is-stdpush-back-relatively-expensive-to-use
    // https://lemire.me/blog/2012/06/20/do-not-waste-time-with-stl-vectors/
    std::vector<float> modifiedData(floatBufLen);
    
    int numChunks = 8;
    int sampleChunkSize = floatBufLen / numChunks; // number of floats per chunk
    
    int counter = 0;
    try{
        // https://codeberg.org/soundtouch/soundtouch/src/branch/master/source/SoundStretch/main.cpp#L191
        for(int k = 0; k < numChunks; k++){
            int nSamples = sampleChunkSize / numChannels;
            float* nextDataToProcess = newData+(sampleChunkSize*k);
            
            soundTouch.putSamples(nextDataToProcess, nSamples);
            
            do{
                nSamples = soundTouch.receiveSamples(nextDataToProcess, buffSizeSamples); // assuming 2 channels
                
                for(int i = 0; i < nSamples * numChannels; i++){
                    modifiedData[counter++] = nextDataToProcess[i];
                }
            } while (nSamples != 0);
        }
        
    }catch(const std::runtime_error &e){
        printf("%s\n", e.what());
    }
    
    // make sure to free allocated space!
    SDL_free(cvt.buf);
    
    return modifiedData;
}

// convert data to 32-bit float karaoke audio for 1 channel 
// need to pass it the data and the length of the data 
std::vector<float> convertToKaraoke(Uint8* wavStart, Uint32 wavLength){
    // convert audio data to F32 
    SDL_AudioCVT cvt;
    SDL_BuildAudioCVT(&cvt, AUDIO_S16, 2, DEF_SAMPLE_RATE, AUDIO_F32, 2, DEF_SAMPLE_RATE);
    cvt.len = wavLength;
    cvt.buf = (Uint8 *)SDL_malloc(cvt.len * cvt.len_mult);
    
    // copy current audio data to the buffer (dest, src, len)
    SDL_memcpy(cvt.buf, wavStart, wavLength); // wavLength is the total number of bytes the audio data takes up
    SDL_ConvertAudio(&cvt);
    
    // audio data is now in float form!
    float* newData = (float *)cvt.buf;

    std::vector<float> leftChannel;
    std::vector<float> rightChannel;
    
    // divide by 4 since cvt.len_cvt is total bytes of the buffer, and 4 bytes per float
    int floatBufLen = (int)cvt.len_cvt / 4;
    int count = 0; // if 0, left channel. 1 for right channel 
    for(int i = 0; i < floatBufLen; i++){
        if(count == 0){
            leftChannel.push_back(newData[i]);
            count++;
        }else{
            rightChannel.push_back(newData[i]);
            count--;
        }
    }
    
    // now eliminate the vocal by getting the diff between left and right and dividing by 2 
    std::vector<float> modifiedData;
    for(int j = 0; j < (int)leftChannel.size(); j++){
        float temp = (leftChannel[j] - rightChannel[j]) / 2.0;
        modifiedData.push_back(temp);
    }
    
    // make sure to free allocated space!
    SDL_free(cvt.buf);
    
    return modifiedData;
}

// for filtering audio
std::vector<float> filterAudio(Uint8* wavStart, Uint32 wavLength){
    // convert audio data to F32 
    SDL_AudioCVT cvt;
    SDL_BuildAudioCVT(&cvt, AUDIO_S16, 2, DEF_SAMPLE_RATE, AUDIO_F32, 2, DEF_SAMPLE_RATE);
    cvt.len = wavLength;
    cvt.buf = (Uint8 *)SDL_malloc(cvt.len * cvt.len_mult);
    
    // copy current audio data to the buffer (dest, src, len)
    SDL_memcpy(cvt.buf, wavStart, wavLength); // wavLength is the total number of bytes the audio data takes up
    SDL_ConvertAudio(&cvt);
    
    // audio data is now in float form!
    float* newData = (float *)cvt.buf;
    
    int floatBufLen = (int)cvt.len_cvt / 4; // 4 bytes per float

    std::vector<float> modifiedData(floatBufLen);
    
    // do the filtering
    double cutoff = 1000;
    int taps = 100;
    
    RTFIR_lowpass lowpassFilter(taps, cutoff/(double)DEF_SAMPLE_RATE);
    
    for(int i = 0; i < floatBufLen; i++){
      modifiedData[i] = (float)(lowpassFilter.Filter((double)newData[i]));
    }
    
    // make sure to free allocated space!
    SDL_free(cvt.buf);
    
    return modifiedData;
}

void writeWavToStream(std::ofstream& stream, std::vector<float>& audioData, int nChannels = 2){
    int32_t bufferSize = (int32_t)audioData.size();
    int32_t riffChunkSize = 36 + bufferSize * 2;
    int32_t formatSize = 16;
    int16_t pcm = 1;
    int16_t numChannels = nChannels;
    int32_t sampleRate = DEF_SAMPLE_RATE;     // TODO: pass in sample rate?
    int32_t byteRate = sampleRate * 2; 
    int16_t bitsPerSample = 16;               // 16-bit pcm wav
    int16_t frameSize = numChannels * 2;      // block align
    int32_t dataChunkSize = bufferSize * 2;   // each entry in the buffer is split into 2 int16
    
    stream.write("RIFF", 4);
    stream.write(reinterpret_cast<char *>(&riffChunkSize), sizeof(int32_t));
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);
    stream.write(reinterpret_cast<char *>(&formatSize), sizeof(int32_t));
    stream.write(reinterpret_cast<char *>(&pcm), sizeof(int16_t)); 
    stream.write(reinterpret_cast<char *>(&numChannels), sizeof(int16_t)); 
    stream.write(reinterpret_cast<char *>(&sampleRate), sizeof(int32_t));
    stream.write(reinterpret_cast<char *>(&byteRate), sizeof(int32_t));
    stream.write(reinterpret_cast<char *>(&frameSize), sizeof(int16_t));
    stream.write(reinterpret_cast<char *>(&bitsPerSample), sizeof(int16_t));
    stream.write("data", 4); 
    stream.write(reinterpret_cast<char *>(&dataChunkSize), sizeof(int32_t));
    
    for(int32_t i = 0; i < bufferSize; i++){
        float maximum = fmax(-1.0, fmin(1.0, audioData[i]));
        int16_t m2;
        if(maximum < 0){
            m2 = maximum * 0x8000;
        }else{
            m2 = maximum * 0x7FFF;
        }
        stream.write(reinterpret_cast<char *>(&m2), sizeof(int16_t));
    }
}

// save file as wav
void saveKaraokeWAV(const char* filename){
    // set up an AudioSpec to load in the file
    SDL_AudioSpec wavSpec;
    Uint8* wavStart;
    Uint32 wavLength;

    // load the wav file and some of its properties to the specified variables
    if(SDL_LoadWAV(filename, &wavSpec, &wavStart, &wavLength) == NULL){
        std::cout << "couldn't load wav file" << std::endl;
        return;
    }

    std::vector<float> audioData = convertToKaraoke(wavStart, wavLength);

    // get string name
    std::string file(filename);
    file = getFilename(file);
    file = "OFF_VOCAL_" + file + ".wav";
    std::cout << "saving file as: " << file << std::endl;

    std::ofstream stream; // create an output file stream
    stream.open(file.c_str(), std::ios::binary);

    writeWavToStream(stream, audioData, 1); // 1 channel

    SDL_FreeWAV(wavStart);
}

void savePitchShiftWAV(const char* filename, int sampleRate = DEF_SAMPLE_RATE){
    SDL_AudioSpec wavSpec;
    Uint8* wavStart;
    Uint32 wavLength;

    // load the wav file and some of its properties to the specified variables
    if(SDL_LoadWAV(filename, &wavSpec, &wavStart, &wavLength) == NULL){
        std::cout << "couldn't load wav file" << std::endl;
        return;
    }

    soundtouch::SoundTouch soundTouch;
    soundTouch.setSampleRate(sampleRate);
    soundTouch.setChannels(2);
    soundTouch.setPitchSemiTones(pitchShiftValue);

    // pass soundtouch to pitchShift and do the thing
    std::vector<float> audioData = pitchShift(wavStart, wavLength, soundTouch);

    std::string file(filename);
    file = getFilename(file);
    file = "PITCH_SHIFTED_" + std::to_string(pitchShiftValue) + "_" + file + ".wav";
    std::cout << "saving file as: " << file << std::endl;

    std::ofstream stream;
    stream.open(file.c_str(), std::ios::binary); 

    writeWavToStream(stream, audioData);

    SDL_FreeWAV(wavStart);
}

// play wav file regularly 
void playWavAudio(std::string file = "", int sampleRate = DEF_SAMPLE_RATE){
    std::cout << "playing: " << file << std::endl;
    
    // set up an AudioSpec to load in the file 
    SDL_AudioSpec wavSpec;
    Uint8* wavStart;
    Uint32 wavLength;
    
    // load the wav file and some of its properties to the specified variables 
    if(SDL_LoadWAV(file.c_str(), &wavSpec, &wavStart, &wavLength) == NULL){
        std::cout << "couldn't load wav file" << std::endl;
        return;
    }
    
    AudioData audio;
    audio.position = wavStart; 
    audio.length = wavLength;
    
    wavSpec.userdata = &audio;
    wavSpec.callback = audioCallback;
    wavSpec.freq = sampleRate; // user-specified sample rate
    
    SDL_AudioDeviceID audioDevice;
    audioDevice = SDL_OpenAudioDevice(NULL, 0, &wavSpec, NULL, 0);
    
    if(audioDevice == 0){
        std::cout << "failed to open audio device!" << std::endl;
        return;
    }
    
    // update global audiodevice id variable 
    currentDeviceID = audioDevice;
    
    // play 
    SDL_PauseAudioDevice(audioDevice, 0);
    
    while(audio.length > 0){
        // as long as there's audio data left to play, keep the thread that this code is running in alive with this while loop
        SDL_Delay(10);
        
        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
        if(currentState == SDL_AUDIO_STOPPED){
            std::cout << "audio stopping, breaking out of loop in thread...\n";
            break;
        }
    }
    
    // done playing audio. make sure to free stuff.
    SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: stopped");
    SDL_CloseAudioDevice(audioDevice);
    SDL_FreeWAV(wavStart);
}

// play wav file with vocal removal 
// assumes SDL is initialized!
void playKaraokeAudio(std::string file = "", int sampleRate = DEF_SAMPLE_RATE){
    // set up an AudioSpec to load in the file 
    SDL_AudioSpec wavSpec;
    Uint8* wavStart;
    Uint32 wavLength;
    
    // load the wav file and some of its properties to the specified variables 
    if(SDL_LoadWAV(file.c_str(), &wavSpec, &wavStart, &wavLength) == NULL){
        std::cout << "couldn't load wav file" << std::endl;
        return;
    }
    
    std::vector<float> modifiedData = convertToKaraoke(wavStart, wavLength);
    
    AudioData audio;
    audio.position = (Uint8*)modifiedData.data(); 
    audio.length = (Uint32)(modifiedData.size() * sizeof(float));
    
    // set up another SDL_AudioSpec with 1 channel to play the modified audio buffer of wavSpec
    SDL_AudioSpec karaokeAudio;
    karaokeAudio.freq = sampleRate; //wavSpec.freq;
    karaokeAudio.format = AUDIO_F32;
    karaokeAudio.channels = 1;
    karaokeAudio.samples = wavSpec.samples;
    karaokeAudio.callback = audioCallback;
    karaokeAudio.userdata = &audio; // attach modified audio data to audio spec 
    
    SDL_AudioDeviceID audioDevice;
    audioDevice = SDL_OpenAudioDevice(NULL, 0, &karaokeAudio, NULL, 0);
    currentDeviceID = audioDevice;
    
    // play 
    SDL_PauseAudioDevice(audioDevice, 0);
    
    while(audio.length > 0){
        // keep thread alive
        SDL_Delay(10);
        
        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
        if(currentState == SDL_AUDIO_STOPPED) break;
    }
    
    // done playing audio. make sure to free stuff 
    SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: stopped");
    SDL_CloseAudioDevice(audioDevice);
    SDL_FreeWAV(wavStart);
}

// play pitch-shifted wav file 
// don't need sampleRate arg? or at least make it a float
void playPitchShiftedAudio(std::string file = "", int sampleRate = DEF_SAMPLE_RATE){
    // set up an AudioSpec to load in the file 
    SDL_AudioSpec wavSpec;
    Uint8* wavStart;
    Uint32 wavLength;
    
    // load the wav file and some of its properties to the specified variables 
    if(SDL_LoadWAV(file.c_str(), &wavSpec, &wavStart, &wavLength) == NULL){
        std::cout << "couldn't load wav file" << std::endl;
        return;
    }
    
    // see https://codeberg.org/soundtouch/soundtouch/src/branch/master/source/SoundStretch/main.cpp
    soundtouch::SoundTouch soundTouch;
    soundTouch.setSampleRate(sampleRate);
    soundTouch.setChannels(2);
    soundTouch.setPitchSemiTones(pitchShiftValue);
    
    // pass soundtouch to pitchShift and do the thing
    std::vector<float> audioData = pitchShift(wavStart, wavLength, soundTouch);
    
    AudioData audio;
    audio.position = (Uint8*)audioData.data(); 
    audio.length = (Uint32)(audioData.size() * sizeof(float));
    
    SDL_AudioSpec pitchShiftSpec;
    pitchShiftSpec.userdata = &audio;
    pitchShiftSpec.callback = audioCallback;
    pitchShiftSpec.freq = sampleRate;
    pitchShiftSpec.format = AUDIO_F32;
    pitchShiftSpec.channels = 2;
    pitchShiftSpec.samples = wavSpec.samples;

    SDL_AudioDeviceID audioDevice;
    audioDevice = SDL_OpenAudioDevice(NULL, 0, &pitchShiftSpec, NULL, 0);
    currentDeviceID = audioDevice;
    
    // play 
    SDL_PauseAudioDevice(audioDevice, 0);
    
    while(audio.length > 0){
        // keep thread alive
        SDL_Delay(10);
        
        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
        if(currentState == SDL_AUDIO_STOPPED) break;
    }
    
    // done playing audio. make sure to free stuff 
    SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: stopped");
    SDL_CloseAudioDevice(audioDevice);
    SDL_FreeWAV(wavStart);
}

void playFilteredAudio(std::string file = "", int sampleRate = DEF_SAMPLE_RATE){
    // set up an AudioSpec to load in the file 
    SDL_AudioSpec wavSpec;
    Uint8* wavStart;
    Uint32 wavLength;
    
    // load the wav file and some of its properties to the specified variables 
    if(SDL_LoadWAV(file.c_str(), &wavSpec, &wavStart, &wavLength) == NULL){
        std::cout << "couldn't load wav file" << std::endl;
        return;
    }
    
    // filter audio e.g. lowpass, highpass, etc.
    std::vector<float> modifiedData = filterAudio(wavStart, wavLength);
    
    AudioData audio;
    audio.position = (Uint8*)modifiedData.data(); 
    audio.length = (Uint32)(modifiedData.size() * sizeof(float));
    
    // update the audio spec
    wavSpec.freq = sampleRate;
    wavSpec.format = AUDIO_F32;
    wavSpec.channels = 2;
    wavSpec.callback = audioCallback;
    wavSpec.userdata = &audio; // attach modified audio data to audio spec 
    
    SDL_AudioDeviceID audioDevice;
    audioDevice = SDL_OpenAudioDevice(NULL, 0, &wavSpec, NULL, 0);
    currentDeviceID = audioDevice;
    
    // play 
    SDL_PauseAudioDevice(audioDevice, 0);
    
    while(audio.length > 0){
        // keep thread alive
        SDL_Delay(10);
        
        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
        if(currentState == SDL_AUDIO_STOPPED) break;
    }
    
    // done playing audio. make sure to free stuff 
    SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: stopped");
    SDL_CloseAudioDevice(audioDevice);
    SDL_FreeWAV(wavStart);
}


// thread function to play audio 
DWORD WINAPI playAudioProc(LPVOID lpParam){
    AudioParams* audioParams = (AudioParams*)lpParam;
    
    std::string filename = std::string((char*)(audioParams->filename));
    int sampleRate = audioParams->sampleRate;
    
    playWavAudio(filename, sampleRate);
    
    // done playing 
    delete audioParams->filename;
    delete audioParams;
    
    return 0;
}

// thread function to play karaoke audio 
DWORD WINAPI playKaraokeAudioProc(LPVOID lpParam){
    AudioParams* audioParams = (AudioParams*)lpParam;
    
    std::string filename = std::string((char*)(audioParams->filename));
    int sampleRate = audioParams->sampleRate;
    
    playKaraokeAudio(filename, sampleRate);
    
    delete audioParams->filename;
    delete audioParams;
    
    return 0;
}

// thread function to play pitch-shifted audio
DWORD WINAPI playPitchShiftedAudioProc(LPVOID lpParam){
    AudioParams* audioParams = (AudioParams*)lpParam;
    
    std::string filename = std::string((char*)(audioParams->filename));
    int sampleRate = audioParams->sampleRate;
    
    playPitchShiftedAudio(filename, sampleRate);
    
    delete audioParams->filename;
    delete audioParams;
    
    return 0;
}

// thread function to play filtered audio
DWORD WINAPI playFilteredAudioProc(LPVOID lpParam){
    AudioParams* audioParams = (AudioParams*)lpParam;
    
    std::string filename = std::string((char*)(audioParams->filename));
    int sampleRate = audioParams->sampleRate;
    
    playFilteredAudio(filename, sampleRate);
    
    delete audioParams->filename;
    delete audioParams;
    
    return 0;
}

// thread function to save karaoke audio 
DWORD WINAPI saveKaraokeAudio(LPVOID lpParam){
    saveKaraokeWAV((char*)lpParam);
    return 0;
}

// thread function to save pitch shift audio
DWORD WINAPI savePitchShiftAudio(LPVOID lpParam){
    AudioParams* audioParams = (AudioParams*)lpParam;

    const char* filename = (char*)(audioParams->filename);
    int sampleRate = audioParams->sampleRate;

    savePitchShiftWAV(filename, sampleRate);

    delete audioParams->filename;
    delete audioParams;

    return 0;
}

void getFile(HWND buttonHandle, HWND textBox){
    OPENFILENAME ofn;
    TCHAR szFile[260] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = buttonHandle;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Audio Files\0*.wav\0\0";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if(GetOpenFileName(&ofn)){
        SetWindowText(textBox, ofn.lpstrFile);
    }
}

void handlePlay(SDL_AudioStatus currentState, DWORD WINAPI(*playFunc)(LPVOID)){
    if(currentState == SDL_AUDIO_STOPPED){
        // get the file first from the text area
        HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
        int textLength = GetWindowTextLength(textbox);

        //https://stackoverflow.com/questions/4545525/pass-charn-param-to-thread
        // also note that MinGW lets you get away with variable-length arrays but it's not standard
        TCHAR* filename = new TCHAR[textLength + 1];

        GetWindowText(textbox, filename, textLength + 1);

        // get the sample rate specified
        // set sample rate to default, which is 44100 if sample rate can't be extracted
        int sampleRate = getCurrentSampleRate();
        if(sampleRate == 0) sampleRate = DEF_SAMPLE_RATE;

        // launch a thread to play the audio
        // pass the thread the params in the AudioParams struct
        AudioParams* audioParams = new AudioParams();
        audioParams->filename = (char*)(filename);
        audioParams->sampleRate = sampleRate;

        audioThread = CreateThread(NULL, 0, playFunc, audioParams, 0, 0);

        SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: playing");
    }else if(currentState == SDL_AUDIO_PAUSED){
        // start up paused audio device again
        std::cout << "starting where we left off..." << std::endl;
        SDL_PauseAudioDevice(currentDeviceID, 0);
        SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: playing");
    }
}


// procedure for main window 
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
        case WM_COMMAND:
            /* LOWORD takes the lower 16 bits of wParam => the element clicked on */
            switch(LOWORD(wParam)){
                case ID_PLAY_BUTTON:
                    {
                        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
                        std::cout << "the current state is: " << currentState << std::endl;
                        handlePlay(currentState, playAudioProc);
                    }
                    break;
                case ID_PLAY_KARAOKE_BUTTON:
                    {
                        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
                        handlePlay(currentState, playKaraokeAudioProc);
                    }
                    break;
                case ID_PLAY_FILTERED_BUTTON:
                    {
                        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
                        handlePlay(currentState, playFilteredAudioProc);
                    }
                    break;
                case ID_PITCH_SHIFT:    
                    {
                        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
                        handlePlay(currentState, playPitchShiftedAudioProc);
                    }
                    break;
                case ID_FILE_DIALOG:
                    {
                        HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
                        HWND button = GetDlgItem(hwnd, ID_FILE_DIALOG);
                        getFile(button, textbox);
                    }
                    break;
                case ID_PAUSE_BUTTON: 
                    {
                        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
                        if(currentState == SDL_AUDIO_PLAYING){
                            SDL_PauseAudioDevice(currentDeviceID, 1);
                            SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: paused");
                        }
                    }
                    break;
                case ID_STOP_BUTTON:
                    {
                        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
                        std::cout << "the current state is: " << currentState << std::endl;
                        SDL_CloseAudioDevice(currentDeviceID);
                        SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: stopped");
                    }
                    break;
                case ID_SAVE_KARAOKE:
                    {
                        HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
                        int textLength = GetWindowTextLength(textbox);
                        TCHAR* filename = new TCHAR[textLength + 1];
                        GetWindowText(textbox, filename, textLength + 1);
                        
                        // https://stackoverflow.com/questions/40500616/c-11-stdthread-use-ofstream-output-stuff-but-get-nothing-why
                        // https://stackoverflow.com/questions/11779504/join-equivalent-in-windows
                        HANDLE saveThread = CreateThread(NULL, 0, saveKaraokeAudio, (char*)(filename), 0, 0);
                        
                        // need to wait for this thread to finish!
                        // otherwise this iteration of the message loop will be done right away and the thread dies prematurely 
                        WaitForSingleObject(saveThread, INFINITE);
                    }
                    break;
                case ID_SAVE_PITCHSHIFT:
                    {
                        HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
                        int textLength = GetWindowTextLength(textbox);
                        TCHAR* filename = new TCHAR[textLength + 1];
                        GetWindowText(textbox, filename, textLength + 1);

                        int sampleRate = getCurrentSampleRate();
                        if(sampleRate == 0) sampleRate = DEF_SAMPLE_RATE;

                        AudioParams* audioParams = new AudioParams();
                        audioParams->filename = (char*)(filename);
                        audioParams->sampleRate = sampleRate;

                        HANDLE saveThread = CreateThread(NULL, 0, savePitchShiftAudio, audioParams, 0, 0);
                        WaitForSingleObject(saveThread, INFINITE);
                    }
                    break;
            }
            break;
        case WM_HSCROLL:
            {
                // handle trackbar/slider activity
                switch(LOWORD(wParam)){
                    case TB_THUMBTRACK:
                    {
                        HWND slider = GetDlgItem(hwnd, ID_PITCH_SHIFT_SLIDER);
                        DWORD pos = SendMessage(slider, TBM_GETPOS, 0, 0);
                        
                        // the slider can only handle unsigned ints so we do some math
                        // to make sure the value is between -MAX_PITCH_SHIFT and MAX_PITCH_SHIFT.
                        // note that the range of the slider is 0 - MAX_PITCH_SHIFT*2.
                        int actualPitchShiftVal = pos - MAX_PITCH_SHIFT;
                        
                        pitchShiftValue = actualPitchShiftVal;
                        
                        SetDlgItemText(hwnd, ID_PITCH_SHIFT_SLIDER_LABEL, std::to_string(actualPitchShiftVal).c_str());
                    }
                    break;
                    case TB_ENDTRACK:
                    {
                        // on mouse up
                        HWND slider = GetDlgItem(hwnd, ID_PITCH_SHIFT_SLIDER);
                        DWORD pos = SendMessage(slider, TBM_GETPOS, 0, 0);
                        int actualPitchShiftVal = pos - MAX_PITCH_SHIFT;
                        pitchShiftValue = actualPitchShiftVal;
                        SetDlgItemText(hwnd, ID_PITCH_SHIFT_SLIDER_LABEL, std::to_string(actualPitchShiftVal).c_str());
                    }
                    break;
                }
                break;
            }
        case WM_CLOSE:
            {
                SDL_Quit();
                DeleteObject(hFont);
                DestroyWindow(hwnd);
            }
            break;
        case WM_DESTROY:
            {
                SDL_Quit();
                DeleteObject(hFont);
                PostQuitMessage(0);
            }
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    
    return 0;
}

// function to create and setup slider/trackbar for setting pitch shift value
void setupPitchShiftSlider(
    int width,
    int height,
    int xCoord,
    int yCoord,
    HWND parent,
    HINSTANCE hInstance,
    HFONT hFont
){
    HWND slider = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        TRACKBAR_CLASS,
        "pitch shift control",
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_ENABLESELRANGE,
        xCoord, yCoord,
        width, height,
        parent,
        (HMENU)ID_PITCH_SHIFT_SLIDER,
        hInstance,
        NULL
    );
    
    // note! we're restricted to using only unsigned ints (so no negatives)
    // but I really mean the range for pitch shift values will be -5 to 5.
    int maxVal = MAX_PITCH_SHIFT*2;
    int minVal = 0;
    SendMessage(slider, WM_SETFONT, (WPARAM)hFont, true);
    SendMessage(slider, TBM_SETRANGE, (WPARAM)true, (LPARAM)MAKELONG(minVal, maxVal));
    SendMessage(slider, TBM_SETPOS, (WPARAM)true, (LPARAM)MAX_PITCH_SHIFT);
}

// function for creating buttons
void setupButton(
    int width,
    int height,
    int xCoord,
    int yCoord,
    HWND parent,
    HINSTANCE hInstance,
    HFONT hFont
){
    // TODO
}


// the main method to launch gui 
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow){
    // uncomment these for seeing console output when debugging
    AllocConsole();
    freopen( "CON", "w", stdout );
    
    // needed on windows 7 
    // see https://stackoverflow.com/questions/22960325/no-audio-with-sdl-c
    // and https://discourse.urho3d.io/t/soundeffect-demo-reports-error-after-sdl-2-0-7-upgrade/3871/7 for this solution 
    SDL_setenv("SDL_AUDIODRIVER", "directsound", true);
    
    // initialize SDL before doing anything else 
    if(SDL_Init(SDL_INIT_AUDIO) != 0){
        std::cout << "Error initializing SDL!" << std::endl;
        return 1;
    }
    
    // specify window 
    WNDCLASSEX wc;
    MSG Msg;
    
    // register the window 
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = 0;
    wc.lpfnWndProc = WndProc; // the above function WndProc is attached here to the window 
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION); // load a default icon 
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = g_szClassName;
    
    if(!RegisterClassEx(&wc)){
        std::cout << "error code: " << GetLastError() << std::endl;
        MessageBox(NULL, "window registration failed!", "Error!", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }
    
    // define all the window components (i.e. buttons, text fields)
    // this is the main window 
    hwnd = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        g_szClassName,
        "basic WAV player",
        WS_TILEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, GUI_WIDTH, GUI_HEIGHT,
        NULL, NULL, hInstance, NULL
    );
    
    if(hwnd == NULL){
        MessageBox(NULL, "window creation failed!", "Error!", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }
    
    // add a label to the add file text edit box
    HWND addWAVPathLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("specify WAV file: "),
        WS_VISIBLE | WS_CHILD, 
        50, 30, // x, y
        100, 20, // width, height
        hwnd,
        (HMENU)ID_ADDWAVPATH_LABEL,
        hInstance,
        NULL
    );
    SendMessage(addWAVPathLabel, WM_SETFONT, (WPARAM)hFont, true);    
    
    // make a text edit area for holding the path to the WAV file to play 
    HWND addWAVPath = CreateWindow(
        TEXT("edit"),
        TEXT(""),
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 
        150, 30,
        250, 20,
        hwnd,
        (HMENU)ID_ADDWAVPATH,
        hInstance,
        NULL
    );
    SendMessage(addWAVPath, WM_SETFONT, (WPARAM)hFont, true);
    
    // button for file dialog
    HWND wavFileDialog = CreateWindow(
        TEXT("button"),
        TEXT("find file"),
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        420, 30,
        100, 20,
        hwnd,
        (HMENU)ID_FILE_DIALOG,
        hInstance,
        NULL
    );
    SendMessage(wavFileDialog, WM_SETFONT, (WPARAM)hFont, true);
    
    // specify sample rate
    HWND addSampleRateLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("specify sample rate: "),
        WS_VISIBLE | WS_CHILD,
        50, 80,
        130, 20,
        hwnd,
        (HMENU)ID_SPECIFY_SAMPLE_RATE_LABEL,
        hInstance,
        NULL
    );
    SendMessage(addSampleRateLabel, WM_SETFONT, (WPARAM)hFont, true);
    
    HWND addSampleRateEdit = CreateWindow(
        TEXT("edit"),
        TEXT("44100"),
        WS_VISIBLE | WS_CHILD | WS_BORDER,
        170, 80,
        70, 20,
        hwnd,
        (HMENU)ID_SPECIFY_SAMPLE_RATE,
        hInstance,
        NULL
    );
    SendMessage(addSampleRateEdit, WM_SETFONT, (WPARAM)hFont, true);
    
    // pitch shift button 
    HWND playPitchShift = CreateWindow(
        TEXT("button"),
        TEXT("play pitch shift"),
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        260, 80, // x, y
        100, 20, // width, height 
        hwnd,
        (HMENU)ID_PITCH_SHIFT,
        hInstance,
        NULL
    );
    SendMessage(playPitchShift, WM_SETFONT, (WPARAM)hFont, true);
    
    // slider/trackbar for selecting pitch shift value
    setupPitchShiftSlider(160, 20, 370, 80, hwnd, hInstance, hFont);
    
    // add a label so we can display the current pitch shift value
    HWND addPitchShiftLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("0"), // pitch shift value is 0 by default
        WS_VISIBLE | WS_CHILD,
        540, 80,
        40, 20,
        hwnd,
        (HMENU)ID_PITCH_SHIFT_SLIDER_LABEL,
        hInstance,
        NULL
    );
    SendMessage(addPitchShiftLabel, WM_SETFONT, (WPARAM)hFont, true);    
    
    // display the current state of the app
    HWND currStateLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("current state: stopped"),
        WS_VISIBLE | WS_CHILD,
        230, 115,
        180, 20,
        hwnd,
        (HMENU)ID_CURR_STATE_LABEL,
        hInstance,
        NULL
    );
    SendMessage(currStateLabel, WM_SETFONT, (WPARAM)hFont, true);
    
    // make a button to play 
    HWND playButton = CreateWindow(
        TEXT("button"),
        TEXT("play"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        25, 150,
        60, 20,
        hwnd,
        (HMENU)ID_PLAY_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(playButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // make a button to play filtered audio
    HWND playFilteredButton = CreateWindow(
        TEXT("button"),
        TEXT("play filtered"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        25, 180,
        90, 20,
        hwnd,
        (HMENU)ID_PLAY_FILTERED_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(playFilteredButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // make a button to play karaoke version 
    HWND playKaraokeButton = CreateWindow(
        TEXT("button"),
        TEXT("play karaoke"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        90, 150,
        100, 20,
        hwnd,
        (HMENU)ID_PLAY_KARAOKE_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(playKaraokeButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // make a button to pause 
    HWND pauseButton = CreateWindow(
        TEXT("button"),
        TEXT("pause"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        195, 150,
        60, 20,
        hwnd,
        (HMENU)ID_PAUSE_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(pauseButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // make a button to stop
    HWND stopButton = CreateWindow(
        TEXT("button"),
        TEXT("stop"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        260, 150,
        60, 20,
        hwnd,
        (HMENU)ID_STOP_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(stopButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // make a button to save karaoke audio 
    HWND saveKaraokeButton = CreateWindow(
        TEXT("button"),
        TEXT("save karaoke"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        325, 150,
        100, 20,
        hwnd,
        (HMENU)ID_SAVE_KARAOKE,
        hInstance,
        NULL
    );
    SendMessage(saveKaraokeButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // make a button to save pitch shifted audio
    HWND savePitchShiftButton = CreateWindow(
        TEXT("button"),
        TEXT("save pitch shift"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        430, 150,
        130, 20,
        hwnd,
        (HMENU)ID_SAVE_PITCHSHIFT,
        hInstance,
        NULL
    );
    SendMessage(savePitchShiftButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // display the gui 
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    // message loop
    while(GetMessage(&Msg, NULL, 0, 0) > 0){
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }
    
    return Msg.wParam;
    
}


