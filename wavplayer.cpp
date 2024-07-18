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

#define AUDIO_SEEK_SLIDER_WIDTH 180.0

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

// hacky way of knowing what the currently-playing audio's total length is (in bytes)
// when the audio scrub slider is moved (so we can calculate playback position based on slider pos)
int currAudioTotalLen = 0;

// audio data struct that callback will use
struct AudioData {
    Uint8* position;
    Uint32 length;
};

// struct to hold filename and sample rate info that can be passed to a thread when playing wav audio 
struct AudioParams {
    char* filename=NULL;
    int sampleRate; // desired sample rate by user
    int numChannels;
    int pitchShiftAmount;
    bool karaokeOn;
    bool lowpassFilterOn;
    bool highpassFilterOn;
    int lowpassCutoff;
    int highpassCutoff;
    int audioStartPos; // where to start playing audio, if specified
    bool reverse; // if audio should be played in reverse
};

AudioParams* audioParams = new AudioParams();

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
    
    return total;
}

int getIntFromTextbox(int dlgItemId){
    HWND textBox = GetDlgItem(hwnd, dlgItemId);
    int textLength = GetWindowTextLength(textBox);

    TCHAR* text = new TCHAR[textLength + 1];
    GetWindowText(textBox, text, textLength + 1);

    // get the value as a string
    std::string textBoxString = std::string((char*)text);

    // extract the int from the string
    int theNum = extractInt(textBoxString);

    delete[] text;

    return theNum;
}

/*
int interpolateLength(float newX, float x1, float y1, float x2, float y2){
    return std::round(y1 + (newX - x1) * ((y2-y1)/(x2-x1)));
}
*/

double getAudioDuration(SDL_AudioSpec& spec, uint32_t audioLen){
    // https://stackoverflow.com/questions/76030221/is-it-possible-to-get-length-in-seconds-of-a-loaded-wav-file-in-sdl-library
    uint32_t sampleSize = SDL_AUDIO_BITSIZE(spec.format) / 8;
    uint32_t sampleCount = audioLen / sampleSize;
    uint32_t sampleLen = sampleCount;
    if(spec.channels){
      sampleLen = sampleCount / spec.channels;
    }
    return (double)sampleLen / (double)spec.freq;
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
// Olli Parviainen's SoundTouch works very well and seems pretty fast (for my demo sample) but gets slower with larger audio files as one might expect.
std::vector<float> pitchShift(
    soundtouch::SoundTouch& soundTouch,
    Uint8* wavStart,
    Uint32 wavLength,
    SDL_AudioFormat srcFormat,
    int srcSampleRate,
    int srcNumChannels
){
    // I tried passing the pre-converted audio data as a std::vector<float>
    // and then trying to use .data() as a float* to use in the for loop to pass
    // samples to soundtouch but that caused segmentation faults on the last chunk passed to putSamples() 
    // maybe something wrong with using .data() as a float* to write data to? or is it because cvt.buf is really a uint8*?
    // Converting the data here directly seems to work fine using (float*)cvt.buf though. 
    // TODO: look into it again at some point to see if using a float vector is possible perhaps?
    SDL_AudioCVT cvt;
    
    // convert audio data to F32 
    SDL_BuildAudioCVT(&cvt, srcFormat, srcNumChannels, srcSampleRate, AUDIO_F32, srcNumChannels, srcSampleRate);
    cvt.len = wavLength;
    cvt.buf = (Uint8*)SDL_malloc(cvt.len * cvt.len_mult);
    
    // copy current audio data to the buffer (dest, src, len)
    SDL_memcpy(cvt.buf, wavStart, wavLength); // wavLength is the total number of bytes the audio data takes up
    SDL_ConvertAudio(&cvt);
    
    // audio data is now in float form!
    float* newData = (float*)cvt.buf;
    int floatBufLen = (int)cvt.len_cvt / 4; // 4 bytes per float
    
    std::cout << "pitchshift - float buffer len:" << floatBufLen << '\n';
    
    // assume 2 channels - TODO: is this a bad idea?
    int numChannels = 2;
    int buffSizeSamples = floatBufLen / numChannels;
    
    std::vector<float> modifiedData(floatBufLen);
    
    int numChunks = 8; // why 8?
    int sampleChunkSize = floatBufLen / numChunks; // number of floats per chunk
    
    int index = 0;
    
    // https://codeberg.org/soundtouch/soundtouch/src/branch/master/source/SoundStretch/main.cpp#L191
    for(int k = 0; k < numChunks; k++){
        int nSamples = sampleChunkSize / numChannels;
        
        float* nextDataToProcess = newData + (sampleChunkSize * k);
        
        soundTouch.putSamples(nextDataToProcess, nSamples);
        
        //std::cout << "processing chunk: " << k << '\n';
        
        do{
            nSamples = soundTouch.receiveSamples(nextDataToProcess, buffSizeSamples);
            
            //std::cout << "got nSamples: " << nSamples << '\n';
            
            for(int i = 0; i < nSamples * numChannels; i++){
                modifiedData[index++] = nextDataToProcess[i];
            }
        }while(nSamples != 0);
    }
    
    SDL_free(cvt.buf);
    
    return modifiedData;
}

// convert data to 32-bit float karaoke audio for 1 channel 
// need to pass it the data and the length of the data 
std::vector<float> convertToKaraoke(std::vector<float>& floatAudioData){
    int floatBufLen = (int)floatAudioData.size();
    
    std::vector<float> leftChannel;
    std::vector<float> rightChannel;
    
    int count = 0; // if 0, left channel. 1 for right channel 
    for(int i = 0; i < floatBufLen; i++){
        if(count == 0){
            leftChannel.push_back(floatAudioData[i]);
            count++;
        }else{
            rightChannel.push_back(floatAudioData[i]);
            count--;
        }
    }
    
    // now eliminate the vocal by getting the diff between left and right and dividing by 2 
    std::vector<float> modifiedData;
    for(int j = 0; j < (int)leftChannel.size(); j++){
        float temp = (leftChannel[j] - rightChannel[j]) / 2.0;
        modifiedData.push_back(temp);
    }
    
    return modifiedData;
}

// lowpass filter
std::vector<float> lowpassFilterAudio(
    std::vector<float>& floatAudioData, 
    int cutoffVal,
    int desiredSampleRate
){
    int floatBufLen = (int)floatAudioData.size();
    
    std::cout << "lowpass filter - float buffer len:" << floatBufLen << '\n';
    
    std::vector<float> modifiedData(floatBufLen);
    
    // do the filtering
    double cutoff = (double)cutoffVal; //1000;
    int taps = 100;
    
    RTFIR_lowpass lowpassFilter(taps, cutoff / (double)desiredSampleRate);
    
    for(int i = 0; i < floatBufLen; i++){
      modifiedData[i] = (float)(lowpassFilter.Filter((double)floatAudioData[i]));
    }
    
    return modifiedData;
}

// highpass filter
std::vector<float> highpassFilterAudio(
    std::vector<float>& floatAudioData, 
    int cutoffVal,
    int desiredSampleRate
){
    int floatBufLen = (int)floatAudioData.size();
    
    std::cout << "highpass filter - float buffer len:" << floatBufLen << '\n';
    
    std::vector<float> modifiedData(floatBufLen);
    
    double cutoff = (double)cutoffVal; //1000;
    int taps = 100;
    
    RTFIR_highpass highpassFilter(taps, cutoff / (double)desiredSampleRate);
    
    for(int i = 0; i < floatBufLen; i++){
      modifiedData[i] = (float)(highpassFilter.Filter((double)floatAudioData[i]));
    }
    
    return modifiedData;
}

// get audio data to vector<float>
std::vector<float> extractAudioDataToFloat(
    Uint8* wavStart,
    Uint32 wavLength,
    SDL_AudioFormat srcFormat,
    int srcSampleRate,
    int srcNumChannels
){
    SDL_AudioCVT cvt;
    
    // convert audio data to F32 
    SDL_BuildAudioCVT(&cvt, srcFormat, srcNumChannels, srcSampleRate, AUDIO_F32, srcNumChannels, srcSampleRate);
    cvt.len = wavLength;
    cvt.buf = (Uint8*)SDL_malloc(cvt.len * cvt.len_mult);
    
    // copy current audio data to the buffer (dest, src, len)
    SDL_memcpy(cvt.buf, wavStart, wavLength); // wavLength is the total number of bytes the audio data takes up
    SDL_ConvertAudio(&cvt);
    
    // audio data is now in float form!
    float* newData = (float*)cvt.buf;
    int floatBufLen = (int)cvt.len_cvt / 4; // 4 bytes per float
    
    std::vector<float> floatAudioData(floatBufLen);
    
    //std::vector<float> floatAudioData {newData, newData + floatBufLen};
    for(int i = 0; i < floatBufLen; i++){
      floatAudioData[i] = (float)newData[i];
    }
    
    std::cout << "extracted audio data - float buffer len:" << floatBufLen << '\n';
    
    // make sure to free allocated space!
    SDL_free(cvt.buf);
    
    return floatAudioData;
}

void writeWavToStream(std::ofstream& stream, std::vector<float>& audioData, int desiredSampleRate, int nChannels = 2){
    int32_t bufferSize = (int32_t)audioData.size();
    int32_t riffChunkSize = 36 + bufferSize * 2;
    int32_t formatSize = 16;
    int16_t pcm = 1;
    int16_t numChannels = nChannels;
    int32_t sampleRate = desiredSampleRate;
    int32_t byteRate = desiredSampleRate * 2; 
    int16_t bitsPerSample = 16;               // 16-bit pcm wav
    int16_t frameSize = numChannels * 2;      // block align
    int32_t dataChunkSize = bufferSize * 2;   // each entry in the buffer is split into 2 int16
    
    stream.write("RIFF", 4);
    stream.write(reinterpret_cast<char*>(&riffChunkSize), sizeof(int32_t));
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);
    stream.write(reinterpret_cast<char*>(&formatSize), sizeof(int32_t));
    stream.write(reinterpret_cast<char*>(&pcm), sizeof(int16_t)); 
    stream.write(reinterpret_cast<char*>(&numChannels), sizeof(int16_t)); 
    stream.write(reinterpret_cast<char*>(&sampleRate), sizeof(int32_t));
    stream.write(reinterpret_cast<char*>(&byteRate), sizeof(int32_t));
    stream.write(reinterpret_cast<char*>(&frameSize), sizeof(int16_t));
    stream.write(reinterpret_cast<char*>(&bitsPerSample), sizeof(int16_t));
    stream.write("data", 4); 
    stream.write(reinterpret_cast<char*>(&dataChunkSize), sizeof(int32_t));
    
    for(int32_t i = 0; i < bufferSize; i++){
        float maximum = fmax(-1.0, fmin(1.0, audioData[i]));
        int16_t m2;
        if(maximum < 0){
            m2 = maximum * 0x8000;
        }else{
            m2 = maximum * 0x7FFF;
        }
        stream.write(reinterpret_cast<char*>(&m2), sizeof(int16_t));
    }
}

void reverseAudioData(std::vector<float>& audioData){
    int audioDataLen = (int)audioData.size();
    for(int i = 0; i < (int)(audioDataLen / 2); i++){
      //std::cout << "i: " << i << ", end: " << audioDataLen - 1 - i << ", len: " << audioDataLen << '\n';
      //std::cout << "audio len: " << audioData.size() << '\n';
      float temp = audioData[i];
      audioData[i] = audioData[audioDataLen - 1 - i];
      audioData[audioDataLen - 1 - i] = temp;
    }
}

void checkLoadedWAV(SDL_AudioSpec* audioSpec){
    std::cout << "source num channels: " << (int)audioSpec->channels << '\n';
    std::cout << "source sample rate: " << audioSpec->freq << '\n';
    std::cout << "source format: " << audioSpec->format << '\n';
}

void playAudio(AudioParams* audioParams){
    std::string filename = std::string((char*)(audioParams->filename));
    std::cout << "playing: " << filename << std::endl;
  
    // set up an AudioSpec to load in the file 
    SDL_AudioSpec wavSpec;
    Uint8* wavStart;
    Uint32 wavLength;
    
    // load the wav file and some of its properties to the specified variables 
    if(SDL_LoadWAV(audioParams->filename, &wavSpec, &wavStart, &wavLength) == NULL){
        std::cout << "couldn't load wav file" << std::endl;
        return;
    }
    
    checkLoadedWAV(&wavSpec); // for debugging
    
    // get loaded audio's num channels
    audioParams->numChannels = (int)wavSpec.channels;
    
    /*
    // TODO: can we do this when we find the file?
    // update sample rate edit box to loaded audio's sample rate
    HWND sampleRateEdit = GetDlgItem(hwnd, ID_SPECIFY_SAMPLE_RATE);
    SendMessage(sampleRateEdit, WM_SETTEXT, (WPARAM)true, (LPARAM)wavSpec.freq);
    */
    
    double duration = getAudioDuration(wavSpec, wavLength);
    std::cout << "duration: " << duration << " sec\n";
    
    // apply filters as needed. 
    // TODO: order currently is arbitrary. let user choose order?
    
    AudioData audio;
    SDL_AudioSpec audioSpec = wavSpec;

    std::vector<float> audioData = extractAudioDataToFloat(
        wavStart,
        wavLength,
        wavSpec.format,
        audioParams->sampleRate,
        audioParams->numChannels
    );
    
    Uint8* audioDataStart = (Uint8*)audioData.data();
    Uint32 audioDataLen = (Uint32)audioData.size();
    
    // TODO: for some reason, doing pitch shift after lowpass filter breaks things.
    // there's a seg fault happening in pitchShift() but not really clear why atm. need to investigate.
    std::cout << "pitch shift amount: " << audioParams->pitchShiftAmount << "\n";
    if(audioParams->pitchShiftAmount != 0){
        std::cout << "pitch shift on\n";
        
        SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: applying pitch shift");
        
        // see https://codeberg.org/soundtouch/soundtouch/src/branch/master/source/SoundStretch/main.cpp
        soundtouch::SoundTouch soundTouch;
        soundTouch.setSampleRate(audioParams->sampleRate);
        soundTouch.setChannels(audioParams->numChannels);
        soundTouch.setPitchSemiTones(audioParams->pitchShiftAmount);
        
        audioData = pitchShift(
            soundTouch,
            wavStart,
            wavLength,
            wavSpec.format,
            audioParams->sampleRate,
            audioParams->numChannels
        );
        
        audioDataStart = (Uint8*)audioData.data();
        audioDataLen = (Uint32)audioData.size();
    }
    
    if(audioParams->lowpassFilterOn){
        std::cout << "lowpass filter on with cutoff: " << audioParams->lowpassCutoff << '\n';
        SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: applying lowpass filter");
        audioData = lowpassFilterAudio(audioData, audioParams->lowpassCutoff, audioParams->sampleRate);
        audioDataStart = (Uint8*)audioData.data();
        audioDataLen = (Uint32)audioData.size();
    }
    
    if(audioParams->highpassFilterOn){
        std::cout << "highpass filter on with cutoff: " << audioParams->highpassCutoff << '\n';
        SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: applying highpass filter");
        audioData = highpassFilterAudio(audioData, audioParams->highpassCutoff, audioParams->sampleRate);
        audioDataStart = (Uint8*)audioData.data();
        audioDataLen = (Uint32)audioData.size();
    }
    
    if(audioParams->reverse){
        reverseAudioData(audioData);
    }
    
    // do karaoke last since it's single channeled
    if(audioParams->karaokeOn){
        std::cout << "off-vocal on\n";
        SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: applying off-vocal");
        audioData = convertToKaraoke(audioData);
        
        audioDataStart = (Uint8*)audioData.data();
        audioDataLen = (Uint32)audioData.size();
        
        audio.position = audioDataStart + audioParams->audioStartPos;
        audio.length = audioDataLen * sizeof(float) - audioParams->audioStartPos;
        
        // set up another SDL_AudioSpec with 1 channel to play the modified audio buffer of wavSpec
        audioSpec.freq = audioParams->sampleRate;
        audioSpec.format = AUDIO_F32;
        audioSpec.channels = 1;
        audioSpec.callback = audioCallback;
        audioSpec.userdata = &audio; // attach modified audio data to audio spec
    }else{
        std::cout << "no off-vocal\n";
        
        audio.position = audioDataStart + audioParams->audioStartPos;
        audio.length = audioDataLen * sizeof(float) - audioParams->audioStartPos; // if we've applied a filter, the data is float* so we need to multiply by 4 to get total bytes (since 4 bytes per float)
        
        audioSpec.userdata = &audio;
        audioSpec.callback = audioCallback;
        audioSpec.freq = audioParams->sampleRate;
        
        audioSpec.format = AUDIO_F32;
        audioSpec.channels = audioParams->numChannels; //2;
    }
    
    SDL_AudioDeviceID audioDevice = SDL_OpenAudioDevice(NULL, 0, &audioSpec, NULL, 0);
    if(audioDevice == 0){
        std::cout << "failed to open audio device!" << std::endl;
        return;
    }
    
    currentDeviceID = audioDevice;
    
    // play 
    SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: playing");
    SDL_PauseAudioDevice(audioDevice, 0);
    
    double totalAudioLen = (double)audioDataLen * sizeof(float); //audio.length;
    currAudioTotalLen = totalAudioLen;
    
    std::cout << "starting play at position: " << audioParams->audioStartPos << ", total audio length: " << (int)totalAudioLen << ", length of audio to play: " << audio.length << '\n';
    
    while(audio.length > 0){
        // keep thread alive
        SDL_Delay(10);
        
        SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
        
        // move audio scrub marker position
        if(currentState == SDL_AUDIO_PLAYING){
            HWND audioScrubber = GetDlgItem(hwnd, ID_AUDIO_SCRUBBER);
            SendMessage(
                audioScrubber, 
                TBM_SETPOS, 
                (WPARAM)true, 
                (LPARAM)(int)(AUDIO_SEEK_SLIDER_WIDTH - ((double)audio.length / totalAudioLen * AUDIO_SEEK_SLIDER_WIDTH)) // audio.length here is the amount of audio remaining to play
            );
        }
        
        // check if we need to stop
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
    
    playAudio(audioParams);
    
    return 0;
}

// thread function to save karaoke audio 
DWORD WINAPI downloadAudioProc(LPVOID lpParam){
    AudioParams* audioParams = (AudioParams*)lpParam;
    
    SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: downloading wav file...");
    
    // set up an AudioSpec to load in the file
    SDL_AudioSpec wavSpec;
    Uint8* wavStart;
    Uint32 wavLength;

    // load the wav file and some of its properties to the specified variables
    if(SDL_LoadWAV(audioParams->filename, &wavSpec, &wavStart, &wavLength) == NULL){
        std::cout << "couldn't load wav file" << std::endl;
        return 0;
    }
    
    // get loaded audio's num channels
    audioParams->numChannels = (int)wavSpec.channels;
    std::cout << "num channels: " << audioParams->numChannels << '\n';
    
    // get string name
    std::string file(audioParams->filename);
    file = getFilename(file);
    file = "edit_" + file + ".wav";
    std::cout << "saving file as: " << file << std::endl;

    std::ofstream stream; // create an output file stream
    stream.open(file.c_str(), std::ios::binary);
    
    std::vector<float> audioData = extractAudioDataToFloat(
        wavStart,
        wavLength,
        wavSpec.format,
        audioParams->sampleRate,
        audioParams->numChannels
    );
    
    // TODO: for some reason, doing pitch shift after lowpass filter breaks things.
    // there's a seg fault happening in pitchShift() but not really clear why atm. need to investigate.
    std::cout << "pitch shift amount: " << audioParams->pitchShiftAmount << '\n';
    if(audioParams->pitchShiftAmount != 0){
        std::cout << "pitch shift on\n";
        //SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: applying pitch shift");
        // see https://codeberg.org/soundtouch/soundtouch/src/branch/master/source/SoundStretch/main.cpp
        soundtouch::SoundTouch soundTouch;
        soundTouch.setSampleRate(audioParams->sampleRate);
        soundTouch.setChannels(audioParams->numChannels);
        soundTouch.setPitchSemiTones(audioParams->pitchShiftAmount);
        
        audioData = pitchShift(
            soundTouch,
            wavStart,
            wavLength,
            wavSpec.format,
            audioParams->sampleRate,
            audioParams->numChannels
        );
    }
    
    if(audioParams->lowpassFilterOn){
        std::cout << "lowpass filter on with cutoff: " << audioParams->lowpassCutoff << '\n';
        //SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: applying lowpass filter");
        audioData = lowpassFilterAudio(audioData, audioParams->lowpassCutoff, audioParams->sampleRate);
    }
    
    if(audioParams->highpassFilterOn){
        std::cout << "highpass filter on with cutoff: " <<  audioParams->highpassCutoff << '\n';
        //SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: applying highpass filter");
        audioData = highpassFilterAudio(audioData, audioParams->highpassCutoff, audioParams->sampleRate);
    }

    if(audioParams->reverse){
        reverseAudioData(audioData);
    }
    
    // do karaoke last since it's single channeled
    if(audioParams->karaokeOn){
        std::cout << "off-vocal on\n";
        //SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: applying karaoke");
        audioData = convertToKaraoke(audioData);
        
        //audioDataStart = (Uint8*)audioData.data();
        //audioDataLen = (Uint32)(audioData.size() * sizeof(float)); // audio data should always be float
        
        writeWavToStream(stream, audioData, audioParams->sampleRate, 1); // 1 channel
    }else{
        std::cout << "no off-vocal\n";
        writeWavToStream(stream, audioData, audioParams->sampleRate, audioParams->numChannels);
    }

    SDL_FreeWAV(wavStart);
    
    SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: downloading complete");
    
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
        int sampleRate = getIntFromTextbox(ID_SPECIFY_SAMPLE_RATE);
        if(sampleRate == 0) sampleRate = DEF_SAMPLE_RATE;
        
        // get filter options
        HWND getLowpassBox = GetDlgItem(hwnd, ID_LOWPASS_CHECKBOX);
        int getLowpassVal = SendMessage(getLowpassBox, BM_GETCHECK, 0, 0);
        audioParams->lowpassFilterOn = (getLowpassVal == BST_CHECKED);
        audioParams->lowpassCutoff = getIntFromTextbox(ID_SPECIFY_LOWPASS_CUTOFF);
        
        HWND getHighpassBox = GetDlgItem(hwnd, ID_HIGHPASS_CHECKBOX);
        int getHighpassVal = SendMessage(getHighpassBox, BM_GETCHECK, 0, 0);
        audioParams->highpassFilterOn = (getHighpassVal == BST_CHECKED);
        audioParams->highpassCutoff = getIntFromTextbox(ID_SPECIFY_HIGHPASS_CUTOFF);
        
        HWND getKaraokeBox = GetDlgItem(hwnd, ID_KARAOKE_CHECKBOX);
        int getKaraokeVal = SendMessage(getKaraokeBox, BM_GETCHECK, 0, 0);
        audioParams->karaokeOn = (getKaraokeVal == BST_CHECKED);
        
        HWND getReverseBox = GetDlgItem(hwnd, ID_REVERSE_CHECKBOX);
        int getReverseVal = SendMessage(getReverseBox, BM_GETCHECK, 0, 0);
        audioParams->reverse = (getReverseVal == BST_CHECKED);

        // launch a thread to play the audio
        // pass the thread the params in the AudioParams struct
        if(audioParams->filename != NULL){
          delete[] audioParams->filename;
        }
        
        audioParams->filename = (char*)(filename);
        audioParams->sampleRate = sampleRate;

        audioThread = CreateThread(NULL, 0, playFunc, audioParams, 0, 0);
        
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
                        audioParams->audioStartPos = 0;
                        handlePlay(currentState, playAudioProc);
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
                        audioParams->audioStartPos = 0;
                    }
                    break;
                case ID_DOWNLOAD_BUTTON:
                    {
                        HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
                        int textLength = GetWindowTextLength(textbox);
                        TCHAR* filename = new TCHAR[textLength + 1];
                        GetWindowText(textbox, filename, textLength + 1);
                        
                        if(audioParams->filename != NULL){
                          delete[] audioParams->filename;
                        }
                        audioParams->filename = filename;
                        
                        int sampleRate = getIntFromTextbox(ID_SPECIFY_SAMPLE_RATE);
                        if(sampleRate == 0) sampleRate = DEF_SAMPLE_RATE;
                        audioParams->sampleRate = sampleRate;
                        
                        // https://stackoverflow.com/questions/40500616/c-11-stdthread-use-ofstream-output-stuff-but-get-nothing-why
                        // https://stackoverflow.com/questions/11779504/join-equivalent-in-windows
                        CreateThread(NULL, 0, downloadAudioProc, audioParams, 0, 0);
                        
                        // TODO: I think WaitForSingleObject() hangs forever if I use it with the global audioParams
                        // https://stackoverflow.com/questions/63981952/waitforsingleobject-for-a-thread-freezes-when-called-from-application-globals
                        // previously I made a new audioParams struct to pass to the thread, which worked with WaitForSingleObject(). need to investigate.
                        // in any case, it seems to work fine like this anyway :D
                        
                        // need to wait for this thread to finish!
                        // otherwise this iteration of the message loop will be done right away and the thread dies prematurely 
                        //WaitForSingleObject(downloadThread, INFINITE);
                    }
                    break;
            }
            break;
        case WM_HSCROLL:
            {
                int sliderId = GetDlgCtrlID((HWND)lParam);
                SDL_AudioStatus currentState = SDL_GetAudioDeviceStatus(currentDeviceID);
                
                // handle trackbar/slider activity
                switch(LOWORD(wParam)){
                    case TB_THUMBTRACK:
                    {
                        if(sliderId == ID_PITCH_SHIFT_SLIDER){
                            HWND slider = GetDlgItem(hwnd, ID_PITCH_SHIFT_SLIDER);
                            DWORD pos = SendMessage(slider, TBM_GETPOS, 0, 0);
                            
                            // the slider can only handle unsigned ints so we do some math
                            // to make sure the value is between -MAX_PITCH_SHIFT and MAX_PITCH_SHIFT.
                            // note that the range of the slider is 0 - MAX_PITCH_SHIFT*2.
                            int actualPitchShiftVal = pos - MAX_PITCH_SHIFT;
                            
                            audioParams->pitchShiftAmount = actualPitchShiftVal;
                            
                            SetDlgItemText(hwnd, ID_PITCH_SHIFT_SLIDER_LABEL, std::to_string(actualPitchShiftVal).c_str());
                        }
                        
                        if(sliderId == ID_AUDIO_SCRUBBER){
                            // stop the audio
                            std::cout << "the current state is: " << currentState << std::endl;
                            SDL_CloseAudioDevice(currentDeviceID);
                            SetDlgItemText(hwnd, ID_CURR_STATE_LABEL, "state: stopped");
                        }
                    }
                    break;
                    case TB_ENDTRACK:
                    {
                        // on mouse up
                        if(sliderId == ID_PITCH_SHIFT_SLIDER){
                            HWND slider = GetDlgItem(hwnd, ID_PITCH_SHIFT_SLIDER);
                            DWORD pos = SendMessage(slider, TBM_GETPOS, 0, 0);
                            int actualPitchShiftVal = pos - MAX_PITCH_SHIFT;
                            audioParams->pitchShiftAmount = actualPitchShiftVal;
                            SetDlgItemText(hwnd, ID_PITCH_SHIFT_SLIDER_LABEL, std::to_string(actualPitchShiftVal).c_str());
                        }
                        
                        if(sliderId == ID_AUDIO_SCRUBBER){
                            // get position of slider, start audio playback at location
                            HWND slider = GetDlgItem(hwnd, ID_AUDIO_SCRUBBER);
                            DWORD pos = SendMessage(slider, TBM_GETPOS, 0, 0);
                            //std::cout << "audio scrubber pos: " << pos << '\n';
                            //std::cout << "curr audio total len: " << currAudioTotalLen << '\n';
                            
                            int audioStartPos = (int)(((double)pos / AUDIO_SEEK_SLIDER_WIDTH) * (double)currAudioTotalLen);
                            
                            // note! the audioStartPos should be divisible by 2. why? I'm not really sure but maybe it has something to do with channels?
                            // TODO: check behavior for off-vocal (1 channel) - that scenario might not need audioStartPos to be divisible by 2?
                            // also, sometimes moving the seek marker results in silence - I'm not sure why yet atm, need to investigate
                            if(audioStartPos % 2 != 0){
                              audioStartPos++;
                            }
                            
                            audioParams->audioStartPos = audioStartPos;
                            //std::cout << "setting audio start pos: " << audioStartPos << ", " << "curr audio total len: " << currAudioTotalLen << '\n';
                            handlePlay(currentState, playAudioProc);
                        }
                    }
                    break;
                }
                break;
            }
        case WM_CLOSE:
            {
                // TODO: make a delete function for AudioParams that we can reuse?
                if(audioParams->filename != NULL){
                  delete[] audioParams->filename;
                }
                delete audioParams;
                SDL_Quit();
                DeleteObject(hFont);
                DestroyWindow(hwnd);
            }
            break;
        case WM_DESTROY:
            {
                if(audioParams->filename != NULL){
                  delete[] audioParams->filename;
                }
                delete audioParams;
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

// function to create and setup slider/trackbar for audio scrubbing
void setupAudioScrubSlider(
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
        "audio scrubber",
        WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_ENABLESELRANGE | TBS_TOOLTIPS,
        xCoord, yCoord,
        width, height,
        parent,
        (HMENU)ID_AUDIO_SCRUBBER,
        hInstance,
        NULL
    );
    
    SendMessage(slider, WM_SETFONT, (WPARAM)hFont, true);
    SendMessage(slider, TBM_SETRANGE, (WPARAM)true, (LPARAM)MAKELONG(0, AUDIO_SEEK_SLIDER_WIDTH));
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
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = g_szClassName;
    wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON));
    wc.hIconSm = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, 16, 16, 0);
    
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
        50, 20, // x, y
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
        150, 20,
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
        420, 20,
        100, 20,
        hwnd,
        (HMENU)ID_FILE_DIALOG,
        hInstance,
        NULL
    );
    SendMessage(wavFileDialog, WM_SETFONT, (WPARAM)hFont, true);
    
    // toggle reverse playback checkbox
    HWND addReverseCheckbox = CreateWindow(
        TEXT("button"),
        TEXT("reverse"),
        BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE,
        30, 58,
        70, 20,
        hwnd,
        (HMENU)ID_REVERSE_CHECKBOX,
        hInstance,
        NULL
    );
    SendMessage(addReverseCheckbox, WM_SETFONT, (WPARAM)hFont, true);
    
    // specify sample rate
    HWND addSampleRateLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("sample rate: "),
        WS_VISIBLE | WS_CHILD,
        105, 60,
        130, 20,
        hwnd,
        (HMENU)ID_SPECIFY_SAMPLE_RATE_LABEL,
        hInstance,
        NULL
    );
    SendMessage(addSampleRateLabel, WM_SETFONT, (WPARAM)hFont, true);
    
    HWND addSampleRateEdit = CreateWindow(
        TEXT("edit"),
        TEXT("48000"),
        WS_VISIBLE | WS_CHILD | WS_BORDER,
        180, 60,
        70, 20,
        hwnd,
        (HMENU)ID_SPECIFY_SAMPLE_RATE,
        hInstance,
        NULL
    );
    SendMessage(addSampleRateEdit, WM_SETFONT, (WPARAM)hFont, true);
    
    HWND pitchShiftSliderLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("pitch shift amount: "),
        WS_VISIBLE | WS_CHILD,
        260, 60, // x, y
        130, 20,
        hwnd,
        (HMENU)ID_PITCH_SHIFT_LABEL,
        hInstance,
        NULL
    );
    SendMessage(pitchShiftSliderLabel, WM_SETFONT, (WPARAM)hFont, true);
    
    // slider/trackbar for selecting pitch shift value
    setupPitchShiftSlider(160, 25, 370, 60, hwnd, hInstance, hFont);
    
    // add a label so we can display the current pitch shift value
    HWND addPitchShiftLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("0"), // pitch shift value is 0 by default
        WS_VISIBLE | WS_CHILD,
        540, 60,
        40, 20,
        hwnd,
        (HMENU)ID_PITCH_SHIFT_SLIDER_LABEL,
        hInstance,
        NULL
    );
    SendMessage(addPitchShiftLabel, WM_SETFONT, (WPARAM)hFont, true);    
    
    // toggle lowpass filter checkbox
    HWND checkBox = CreateWindow(
        TEXT("button"),
        TEXT("lowpass filter"),
        BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE,
        25, 100,  /* x, y coords */
        100, 20, /* width, height */
        hwnd,
        (HMENU)ID_LOWPASS_CHECKBOX,
        hInstance,
        NULL
    );
    SendMessage(checkBox, WM_SETFONT, (WPARAM)hFont, true);
    
    // specify cutoff for lowpass
    HWND addLowPassCutoffEditLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("cutoff: "),
        WS_VISIBLE | WS_CHILD,
        125, 102,
        50, 20,
        hwnd,
        (HMENU)ID_SPECIFY_LOWPASS_CUTOFF_LABEL,
        hInstance,
        NULL
    );
    SendMessage(addLowPassCutoffEditLabel, WM_SETFONT, (WPARAM)hFont, true);
    
    HWND addLowPassCutoffEdit = CreateWindow(
        TEXT("edit"),
        TEXT("1000"),
        WS_VISIBLE | WS_CHILD | WS_BORDER,
        165, 100,
        70, 20,
        hwnd,
        (HMENU)ID_SPECIFY_LOWPASS_CUTOFF,
        hInstance,
        NULL
    );
    SendMessage(addLowPassCutoffEdit, WM_SETFONT, (WPARAM)hFont, true);
    
    // toggle highpass filter checkbox
    HWND highpassCheckBox = CreateWindow(
        TEXT("button"),
        TEXT("highpass filter"),
        BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE,
        250, 100,  /* x, y coords */
        100, 20, /* width, height */
        hwnd,
        (HMENU)ID_HIGHPASS_CHECKBOX,
        hInstance,
        NULL
    );
    SendMessage(highpassCheckBox, WM_SETFONT, (WPARAM)hFont, true);
    
    // specify cutoff for highpass
    HWND addHighPassCutoffEditLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("cutoff: "),
        WS_VISIBLE | WS_CHILD,
        355, 102,
        50, 20,
        hwnd,
        (HMENU)ID_SPECIFY_HIGHPASS_CUTOFF_LABEL,
        hInstance,
        NULL
    );
    SendMessage(addHighPassCutoffEditLabel, WM_SETFONT, (WPARAM)hFont, true);
    
    HWND addHighPassCutoffEdit = CreateWindow(
        TEXT("edit"),
        TEXT("1000"),
        WS_VISIBLE | WS_CHILD | WS_BORDER,
        395, 100,
        70, 20,
        hwnd,
        (HMENU)ID_SPECIFY_HIGHPASS_CUTOFF,
        hInstance,
        NULL
    );
    SendMessage(addHighPassCutoffEdit, WM_SETFONT, (WPARAM)hFont, true);
    
    // toggle karaoke/off-vocal checkbox
    HWND checkBox2 = CreateWindow(
        TEXT("button"),
        TEXT("off vocal"),
        BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE,
        480, 100,  /* x, y coords */
        80, 20, /* width, height */
        hwnd,
        (HMENU)ID_KARAOKE_CHECKBOX,
        hInstance,
        NULL
    );
    SendMessage(checkBox2, WM_SETFONT, (WPARAM)hFont, true);
    
    // play button
    HWND playButton = CreateWindow(
        TEXT("button"),
        TEXT("play"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        50, 140,
        60, 20,
        hwnd,
        (HMENU)ID_PLAY_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(playButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // pause button
    HWND pauseButton = CreateWindow(
        TEXT("button"),
        TEXT("pause"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        115, 140,
        60, 20,
        hwnd,
        (HMENU)ID_PAUSE_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(pauseButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // stop button
    HWND stopButton = CreateWindow(
        TEXT("button"),
        TEXT("stop"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        180, 140,
        60, 20,
        hwnd,
        (HMENU)ID_STOP_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(stopButton, WM_SETFONT, (WPARAM)hFont, true);
    
    // download button
    HWND downloadButton = CreateWindow(
        TEXT("button"),
        TEXT("download"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        245, 140,
        70, 20,
        hwnd,
        (HMENU)ID_DOWNLOAD_BUTTON,
        hInstance,
        NULL
    );
    SendMessage(downloadButton, WM_SETFONT, (WPARAM)hFont, true);
    
    HWND audioScrubberLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("seek: "),
        WS_VISIBLE | WS_CHILD,
        330, 140, // x, y
        100, 20,
        hwnd,
        (HMENU)ID_AUDIO_SCRUBBER_LABEL,
        hInstance,
        NULL
    );
    SendMessage(audioScrubberLabel, WM_SETFONT, (WPARAM)hFont, true);
    
    // audio scrubber
    setupAudioScrubSlider(AUDIO_SEEK_SLIDER_WIDTH, 25, 365, 140, hwnd, hInstance, hFont); // width, height, x, y
    
    // display the current state of the app
    HWND currStateLabel = CreateWindow(
        TEXT("STATIC"),
        TEXT("current state: stopped"),
        WS_VISIBLE | WS_CHILD,
        230, 170,
        180, 20,
        hwnd,
        (HMENU)ID_CURR_STATE_LABEL,
        hInstance,
        NULL
    );
    SendMessage(currStateLabel, WM_SETFONT, (WPARAM)hFont, true);
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
