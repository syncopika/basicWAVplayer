#include <iostream>
#include <vector>
#include <fstream>
#include <ctype.h>
#include <math.h>
#include <string>
#include <fcntl.h>
#include <SDL.h>
#include <windows.h>
#include <stdlib.h>
#include <cstdio>
#include <stdexcept>

// SoundTouch code by Olli Parviainen
#include "soundtouch/SoundTouch.h"

// give some identifiers for the GUI components 
#include "resources.h"

// pitch shifting code by Stephan Bernsee 
//#include "smbPitchShift.h"

// default sample rate 
#define DEF_SAMPLE_RATE 44100

#define GUI_WIDTH 600
#define GUI_HEIGHT 450
#define VISUALIZER_WINDOW_WIDTH 510
#define VISUALIZER_WINDOW_HEIGHT 180


/* features to add 

- timestamp? marker to show how many seconds has elapsed, total time to play 
- change pitch
- turn karaoke off or on 

*/

// enum for current play state 
enum PlayState{IS_PLAYING, IS_PAUSED, IS_STOPPED};

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

// global variable to keep track of playing state?? 
PlayState currentState = IS_STOPPED;

// keep track of audiodevice id - only keep one around!
SDL_AudioDeviceID currentDeviceID;

// keep track of thread designated to play audio. 
HANDLE audioThread;

// the sdl window and renderer for visualization 
SDL_Window* sdlWnd;
SDL_Renderer* sdlRend;

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
};

// audio data struct that callback will use 
struct AudioData{
	Uint8* position;
	Uint32 length;
};

// struct to hold filename and sample rate info that can be passed to a thread when playing wav audio 
struct AudioParams{
	char* filename;
	int sampleRate;
};

int interpolateLength(float newX, float x1, float y1, float x2, float y2){
	return std::round(y1 + (newX - x1) * ((y2-y1)/(x2-x1)));
}

// get a vector of the evenly-distributed indices to sample from the dataset based on num samples desired
// https://stackoverflow.com/questions/9873626/choose-m-evenly-spaced-elements-from-a-sequence-of-length-n
std::vector<int> getSampleIndices(int dataLen, int numSamples){
	std::vector<int> result;
	int samples = (numSamples <= dataLen) ? numSamples : dataLen; 
	for(int i = 0; i < samples; i++){
		int idx = std::floor((i*dataLen)/samples) + std::floor(dataLen/(2*samples));
		result.push_back(idx);
	}
	return result;
}

// define an audio callback that SDL_AudioSpec will use
void audioCallback(void* userData, Uint8* stream, int length){
	AudioData* audio = (AudioData*)userData;
	float* streamF = (float *)stream;
	
	if(audio->length == 0){
		// stop playing stream here??
		return;
	}
	
	// length is number of bytes of userData's audio data
	Uint32 len = (Uint32)length;
	
	if(len > audio->length){
		len = audio->length;
	}
	
	int desiredNumPointsToDisplay = 1000; // number of data points to show at a time on the screen
	std::vector<int> sampleIndices = getSampleIndices((int)len-1, desiredNumPointsToDisplay); // subtract 1 to ensure last index is available (should probably check via cout to see what sampleIndices looks like)
	
	SDL_SetRenderDrawColor(sdlRend, 255, 255, 255, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(sdlRend);
	SDL_SetRenderDrawColor(sdlRend, 0, 0, 255, SDL_ALPHA_OPAQUE);
	
	float audioDataSize = 65536; //255.0; // if 8-bit data, 255. if 16-bit, 65536. etc.
	
	for(int i = 0; i < (int)sampleIndices.size(); i++){
		int sampleIdx = sampleIndices[i];

		// b/c we want 16-bit int (expecting each audio data point to be 16-bit) and not 8-bit
		int signalAmp = (audio->position[sampleIdx+1] << 8 | audio->position[sampleIdx]);
		
		int scaledVal = interpolateLength((float)signalAmp, 0.0, 0.0, audioDataSize, (float)VISUALIZER_WINDOW_HEIGHT/2); // height is divided by 2 because half of the height of the rectangle represents max amplitude since the middle of the rectangle represents 0.
		
		if(scaledVal >= 70){
			scaledVal = 0;
		}
		
		int offset = (VISUALIZER_WINDOW_HEIGHT - scaledVal) / 2;
		
		SDL_RenderDrawLine(sdlRend, i, offset, i, offset+scaledVal);
	}
	SDL_RenderPresent(sdlRend);
	
	// copy len bytes from audio stream at audio->position to stream buffer
	SDL_memcpy(streamF, audio->position, len);
	
	audio->position += len;
	audio->length -= len;
}

// try pitch shifting?? :/
// https://www.kvraudio.com/forum/viewtopic.php?t=134637
// https://www.kvraudio.com/forum/viewtopic.php?t=349092
// it works with Stephan Bernsee's solution, but note that it's slow. just don't think it's broken...
// use gdb to run it and check
// Olli Parviainen's SoundTouch works well but gets slower with larger audio files.
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
	
	int numSamplesToProcess = floatBufLen / numChannels;
    
    std::vector<float> modifiedData;
    try{
        // https://codeberg.org/soundtouch/soundtouch/src/branch/master/source/SoundStretch/main.cpp#L191
        soundTouch.putSamples(newData, numSamplesToProcess);
        
        do{
            numSamplesToProcess = soundTouch.receiveSamples(newData, floatBufLen / numChannels); // assuming 2 channels
            for(int i = 0; i < numSamplesToProcess * numChannels; i++){
                modifiedData.push_back(newData[i]);
            }
        } while (numSamplesToProcess != 0);
        
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
	
	int32_t bufferSize = (int32_t)audioData.size();
	int32_t riffChunkSize = 36 + bufferSize * 2;
	int32_t formatSize = 16;
	int16_t pcm = 1;
	int16_t numChannels = 1;
	int32_t sampleRate = DEF_SAMPLE_RATE;
	int32_t byteRate = sampleRate * 2; 
	int16_t bitsPerSample = 16; 			// 16-bit pcm wav 
	int16_t frameSize = numChannels * 2; 	// block align
	int32_t dataChunkSize = bufferSize * 2; // each entry in the buffer is split into 2 int16
	
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
	
	SDL_FreeWAV(wavStart);
}


// play wav file regularly 
void playWavAudio(std::string file = "", int sampleRate = DEF_SAMPLE_RATE){
	std::cout << "playing: " << file << std::endl; 
	SDL_SetRenderDrawColor(sdlRend, 255, 255, 255, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(sdlRend);
	
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
	currentState = IS_PLAYING;
	
	while(audio.length > 0){
		SDL_Delay(30); // set some delay so program doesn't immediately quit 
	}
	
	// done playing audio. make sure to free stuff. 
	currentState = IS_STOPPED;
	SDL_CloseAudioDevice(audioDevice);
	SDL_FreeWAV(wavStart);
}

// play wav file with vocal removal 
// assumes SDL is initialized!
void playKaraokeAudio(std::string file = "", int sampleRate = DEF_SAMPLE_RATE){
	SDL_SetRenderDrawColor(sdlRend, 255, 255, 255, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(sdlRend);
	
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
	currentState = IS_PLAYING;
	
	while(audio.length > 0){
		SDL_Delay(30); // set some delay so program doesn't immediately quit 
	}
	
	// done playing audio. make sure to free stuff 
	currentState = IS_STOPPED;
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
    soundTouch.setPitchSemiTones(2); // raise pitch by 2 semitones TODO: make this something a user can change
	
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
	currentState = IS_PLAYING;
	
	while(audio.length > 0){
		SDL_Delay(30); // set some delay so program doesn't immediately quit 
	}
	
	// done playing audio. make sure to free stuff 
	currentState = IS_STOPPED;
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

// thread function to save karaoke audio 
DWORD WINAPI saveKaraokeAudio(LPVOID lpParam){
	saveKaraokeWAV((char*)lpParam);
	return 0;
}


// procedure for main window 
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
		case WM_COMMAND:
			/* LOWORD takes the lower 16 bits of wParam => the element clicked on */
			switch(LOWORD(wParam)){
				case ID_PLAY_BUTTON:
					{
						std::cout << "the current state is: " << currentState << std::endl;
						// play regular audio 
						if(currentState == IS_STOPPED){
							// get the file first from the text area 
							HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
							int textLength = GetWindowTextLength(textbox);
							
							//https://stackoverflow.com/questions/4545525/pass-charn-param-to-thread
							TCHAR* filename = new TCHAR[textLength + 1]();
							
							GetWindowText(textbox, filename, textLength + 1);
							
							// get the sample rate specified 
							// set sample rate to default, which is 44100 if sample rate can't be extracted 
							int sampleRate = 0;
								
							HWND sampleRateTextBox = GetDlgItem(hwnd, ID_SPECIFY_SAMPLE_RATE);
							int textLengthSample = GetWindowTextLength(sampleRateTextBox);
								
							TCHAR sampleRateText[textLengthSample+1];
							GetWindowText(sampleRateTextBox, sampleRateText, textLengthSample + 1);
								
							// get the sample rate as a string
							std::string sampleRateString = std::string((char*)sampleRateText);
							//std::cout << sampleRateString << std::endl;
							
							// extract the int value from the string 
							sampleRate = extractInt(sampleRateString);
							
							if(sampleRate == 0){
								sampleRate = DEF_SAMPLE_RATE;
							}
						
							// launch a thread to play the audio 
							// pass the thread the params in the AudioParams struct  
							char* fname = (char*)(filename);
							
							AudioParams* audioParams = new AudioParams();
							audioParams->filename = fname;
							audioParams->sampleRate = sampleRate;
							
							audioThread = CreateThread(NULL, 0, playAudioProc, audioParams, 0, 0);
						}else if(currentState == IS_PAUSED){
							// start up paused audio device again
							std::cout << "starting where we left off..." << std::endl;
							SDL_PauseAudioDevice(currentDeviceID, 0);
							currentState = IS_PLAYING;
						}
					}
					break;
				case ID_PLAY_KARAOKE_BUTTON:
					{
						if(currentState == IS_STOPPED){
							// get the file first from the text area 
							HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
							int textLength = GetWindowTextLength(textbox);
							
							TCHAR* filename = new TCHAR[textLength + 1]();
							
							GetWindowText(textbox, filename, textLength + 1);
							
							// get the sample rate specified 
							int sampleRate = 0;
								
							HWND sampleRateTextBox = GetDlgItem(hwnd, ID_SPECIFY_SAMPLE_RATE);
							int textLengthSample = GetWindowTextLength(sampleRateTextBox);
								
							TCHAR sampleRateText[textLengthSample+1];
							GetWindowText(sampleRateTextBox, sampleRateText, textLengthSample + 1);
								
							// get the sample rate as a string
							std::string sampleRateString = std::string((char*)sampleRateText);
							//std::cout << sampleRateString << std::endl;
							
							// extract the int value from the string 
							sampleRate = extractInt(sampleRateString);
							
							if(sampleRate == 0){
								sampleRate = DEF_SAMPLE_RATE;
							}
						
							// launch a thread to play the audio 
							// pass the thread the params in the AudioParams struct  
							char* fname = (char*)(filename);
							
							AudioParams* audioParams = new AudioParams();
							audioParams->filename = fname;
							audioParams->sampleRate = sampleRate;
							
							audioThread = CreateThread(NULL, 0, playKaraokeAudioProc, audioParams, 0, 0);
						}
					}
					break;
				
				case ID_PITCH_SHIFT:	
				{
					if(currentState == IS_STOPPED){
							// get the file first from the text area 
							HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
							int textLength = GetWindowTextLength(textbox);
							
							TCHAR* filename = new TCHAR[textLength + 1]();
							
							GetWindowText(textbox, filename, textLength + 1);
							
							// get the sample rate specified 
							// set sample rate to default, which is 44100 if sample rate can't be extracted 
							int sampleRate = 0;
								
							HWND sampleRateTextBox = GetDlgItem(hwnd, ID_SPECIFY_SAMPLE_RATE);
							int textLengthSample = GetWindowTextLength(sampleRateTextBox);
								
							TCHAR sampleRateText[textLengthSample+1];
							GetWindowText(sampleRateTextBox, sampleRateText, textLengthSample + 1);
								
							// get the sample rate as a string
							std::string sampleRateString = std::string((char*)sampleRateText);
							//std::cout << sampleRateString << std::endl;
							
							// extract the int value from the string 
							sampleRate = extractInt(sampleRateString);
							
							if(sampleRate == 0){
								sampleRate = DEF_SAMPLE_RATE;
							}
						
							// launch a thread to play the audio 
							// pass the thread the params in the AudioParams struct  
							char* fname = (char*)(filename);
							
							AudioParams* audioParams = new AudioParams();
							audioParams->filename = fname;
							audioParams->sampleRate = sampleRate;
                            
							audioThread = CreateThread(NULL, 0, playPitchShiftedAudioProc, audioParams, 0, 0);
						}else if(currentState == IS_PAUSED){
							// start up paused audio device again
							std::cout << "starting where we left off..." << std::endl;
							SDL_PauseAudioDevice(currentDeviceID, 0);
							currentState = IS_PLAYING;
						}
				}
				break;
				
				case ID_PAUSE_BUTTON:
					// implement me 
					{
						if(currentState == IS_PLAYING){
							currentState = IS_PAUSED;
						
							// halt the audio device callback function 
							SDL_PauseAudioDevice(currentDeviceID, 1);
						}
					}
					break;
				case ID_STOP_BUTTON:
					{
						std::cout << "the current state is: " << currentState << std::endl;
						if(currentState != IS_STOPPED){
                            SDL_SetRenderDrawColor(sdlRend, 255, 255, 255, SDL_ALPHA_OPAQUE);
                            SDL_RenderClear(sdlRend);
                            SDL_RenderPresent(sdlRend);
                        
							currentState = IS_STOPPED;
							SDL_CloseAudioDevice(currentDeviceID);
						}
					}
					break;
				case ID_SAVE_KARAOKE:
					{
						HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
						int textLength = GetWindowTextLength(textbox);
						TCHAR filename[textLength + 1];
						GetWindowText(textbox, filename, textLength + 1);
						char* fname = (char*)(filename);
						
						// Y U NO OUTPUT!?? - see: https://stackoverflow.com/questions/40500616/c-11-stdthread-use-ofstream-output-stuff-but-get-nothing-why
						// https://stackoverflow.com/questions/11779504/join-equivalent-in-windows
						HANDLE saveThread = CreateThread(NULL, 0, saveKaraokeAudio, fname, 0, 0);
						
						// need to wait for this thread to finish!
						// otherwise this iteration of the message loop will be done right away and the thread dies prematurely 
						WaitForSingleObject(saveThread, INFINITE);
					}
					break;
			}
			break;
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

// the main method to launch gui 
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow){
	
	//AllocConsole();
    //freopen( "CON", "w", stdout );
	
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
		160, 30, 
		280, 20,
		hwnd,
		(HMENU)ID_ADDWAVPATH,
		hInstance,
		NULL
	);
	SendMessage(addWAVPath, WM_SETFONT, (WPARAM)hFont, true);
	
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
		180, 80, 
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
		280, 80, 
		100, 20, 
		hwnd,
		(HMENU)ID_PITCH_SHIFT,
		hInstance,
		NULL
	);
	SendMessage(playPitchShift, WM_SETFONT, (WPARAM)hFont, true);
	
	// make a button to play 
	HWND playButton = CreateWindow(
		TEXT("button"),
		TEXT("play"),
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        30, 150,
        80, 20, 
        hwnd,
        (HMENU)ID_PLAY_BUTTON,
        hInstance,
        NULL
	);
	SendMessage(playButton, WM_SETFONT, (WPARAM)hFont, true);
	
	// make a button to play karaoke version 
	HWND playKaraokeButton = CreateWindow(
		TEXT("button"),
		TEXT("play karaoke ver."),
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        120, 150,
        120, 20, 
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
        250, 150,
        80, 20, 
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
        340, 150,
        80, 20, 
        hwnd,
        (HMENU)ID_STOP_BUTTON,
        hInstance,
        NULL
	);
	SendMessage(stopButton, WM_SETFONT, (WPARAM)hFont, true);
	
	// make a button to save karaoke audio 
	HWND saveKaraokeButton = CreateWindow(
		TEXT("button"),
		TEXT("save karaoke ver."),
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        430, 150,
        120, 20, 
        hwnd,
        (HMENU)ID_SAVE_KARAOKE,
        hInstance,
        NULL
	);
	SendMessage(saveKaraokeButton, WM_SETFONT, (WPARAM)hFont, true);
	
	// child window to visualize audio (wow!)
	HWND audioVisualizerWindow = CreateWindow(
		TEXT("STATIC"),
		NULL,
		WS_CHILD | WS_VISIBLE | WS_BORDER,
        30, 200,
        VISUALIZER_WINDOW_WIDTH, VISUALIZER_WINDOW_HEIGHT, 
        hwnd,
        (HMENU)ID_AUDIO_VISUALIZER,
        hInstance,
        NULL
	);
	SendMessage(audioVisualizerWindow, WM_SETFONT, (WPARAM)hFont, true);
	
	// display the gui 
	ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
	
	// make the audio visualizer child window an SDL window!
	SDL_InitSubSystem(SDL_INIT_VIDEO);
	
	sdlWnd = SDL_CreateWindowFrom(audioVisualizerWindow);
	sdlRend = SDL_CreateRenderer(sdlWnd, -1, SDL_RENDERER_ACCELERATED);
	
	char sBuf[32];
	sprintf(sBuf, "%p", sdlWnd);
	SDL_SetHint(SDL_HINT_VIDEO_WINDOW_SHARE_PIXEL_FORMAT, sBuf);

	//SDL_SetWindowTitle(sdlWnd, "SDL Window - Set by SDL");
	//SDL_Surface* s = SDL_GetWindowSurface(sdlWnd);
	//SDL_FillRect(s, &s->clip_rect, 0xffff00ff);
	//SDL_UpdateWindowSurface(sdlWnd);
    
    // message loop
    while(GetMessage(&Msg, NULL, 0, 0) > 0){
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }
	
    return Msg.wParam;
	
}


