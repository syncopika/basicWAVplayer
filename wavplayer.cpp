#include <iostream>
#include <vector>
#include <fstream>
#include <math.h>
#include <string>
#include <fcntl.h>
#include <SDL.h>
#include <windows.h>

// give some identifiers for the GUI components 
#include "resources.h"

/* features to add 

pretty helpful: https://github.com/syncopika/syncopika.github.io/blob/master/misc/karaokeget.html

- gui
- timestamp? marker to show how many seconds has elapsed, total time to play 
- play/pause
- change pitch
- turn karaoke off or on 

*/

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
bool isPlaying = false;

// keep track of audiodevice id - only keep one around!
SDL_AudioDeviceID currentDeviceID;

// keep track of thread designated to play audio. 
HANDLE audioThread;

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
		return file.substr(0, file.size() - 4);
	}
	
}

// audio data struct that callback will use 
struct AudioData{
	Uint8* position;
	Uint32 length;
};

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
	
	// copy len bytes from audio stream at audio->position to stream buffer
	SDL_memcpy(streamF, audio->position, len);
	
	audio->position += len;
	audio->length -= len;
}

// convert data to 32-bit float karaoke audio for 1 channel 
// need to pass it the data and the length of the data 
std::vector<float> convertToKaraoke(Uint8* wavStart, Uint32 wavLength){
	
	// convert audio data to F32 
	SDL_AudioCVT cvt;
	SDL_BuildAudioCVT(&cvt, AUDIO_S16, 2, 48000, AUDIO_F32, 2, 48000);
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
	//std::cout << "file name: " << file << std::endl;
	file = getFilename(file);
	file = "OFF_VOCAL_" + file + ".wav";
	std::cout << "saving file as: " << file << std::endl;
	//std::cout << "size of audioData: " << audioData.size() << std::endl;
	
	std::ofstream stream; // create an output file stream 
	stream.open(file.c_str(), std::ios::binary); 
	
	int32_t bufferSize = (int32_t)audioData.size();
	
	int32_t riffChunkSize = 36 + bufferSize * 2;
	int32_t formatSize = 16;
	int16_t pcm = 1;
	int16_t numChannels = 1;
	int32_t sampleRate = 48000;
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
void playWavAudio(std::string file = "C:\\Users\\Nicholas Hung\\Desktop\\昼休みとヘルメットと同級生.wav"){
	
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
	
	SDL_AudioDeviceID audioDevice;
	audioDevice = SDL_OpenAudioDevice(NULL, 0, &wavSpec, NULL, 0);
	
	// update global audiodevice id variable 
	currentDeviceID = audioDevice;
	
	// play 
	SDL_PauseAudioDevice(audioDevice, 0);
	isPlaying = true;
	
	while(audio.length > 0 && isPlaying){
		SDL_Delay(10); // set some delay so program doesn't immediately quit 
	}
	
	//std::cout << "ending play" << std::endl;
	
	// done playing audio. make sure to free stuff. 
	SDL_CloseAudioDevice(audioDevice);
	SDL_FreeWAV(wavStart);
	
}

// play wav file with vocal removal 
// assumes SDL is initialized!
void playKaraokeAudio(std::string file = "C:\\Users\\Nicholas Hung\\Desktop\\route216.wav"){
	
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
	karaokeAudio.freq = wavSpec.freq;
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
	isPlaying = true;
	
	while(audio.length > 0 && isPlaying){
		SDL_Delay(10); // set some delay so program doesn't immediately quit 
	}
	
	// done playing audio. make sure to free stuff 
	SDL_CloseAudioDevice(audioDevice);
	SDL_FreeWAV(wavStart);
	
}

// thread function to play audio 
DWORD WINAPI playAudioProc(LPVOID lpParam){
	std::string filename = std::string((char*)lpParam);
	playWavAudio(filename);
	
	// done playing 
	isPlaying = false;
	return 0;
}

// thread function to play karaoke audio 
DWORD WINAPI playKaraokeAudioProc(LPVOID lpParam){
	std::string filename = std::string((char*)lpParam);
	playKaraokeAudio(filename);
	isPlaying = false;
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
						// play regular audio 
						if(!isPlaying){
							
							// get the file first from the text area 
							HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
							int textLength = GetWindowTextLength(textbox);
							TCHAR filename[textLength + 1];
							GetWindowText(textbox, filename, textLength + 1);
						
							// launch a thread to play the audio 
							// pass the thread the file to play 
							char* fname = (char*)(filename);
							audioThread = CreateThread(NULL, 0, playAudioProc, fname, 0, 0);
						}
					}
				break;
				case ID_PLAY_KARAOKE_BUTTON:
					{
						if(!isPlaying){
							// get the file first from the text area 
							HWND textbox = GetDlgItem(hwnd, ID_ADDWAVPATH);
							int textLength = GetWindowTextLength(textbox);
							TCHAR filename[textLength + 1];
							GetWindowText(textbox, filename, textLength + 1);
						
							// launch a thread to play the audio 
							// pass the thread the file to play 
							char* fname = (char*)(filename);
							audioThread = CreateThread(NULL, 0, playKaraokeAudioProc, fname, 0, 0);
						}
					}
				break;
				case ID_PAUSE_BUTTON:
					// implement me 
					break;
				case ID_STOP_BUTTON:
					{
						// implement me 
						if(isPlaying){
							isPlaying = false;
							SDL_CloseAudioDevice(currentDeviceID);
						}
					}
				break;
				case ID_SAVE_KARAOKE:
					{
						// implement me 
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
	
	AllocConsole();
    freopen( "CON", "w", stdout );
	
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
		CW_USEDEFAULT, CW_USEDEFAULT, 600, 180,
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
		50, 30, 
		100, 20,
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
	
	// make a button to play 
	HWND playButton = CreateWindow(
		TEXT("button"),
		TEXT("play"),
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        30, 80,
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
        120, 80,
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
        250, 80,
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
        340, 80,
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
        430, 80,
        120, 20, 
        hwnd,
        (HMENU)ID_SAVE_KARAOKE,
        hInstance,
        NULL
	);
	SendMessage(saveKaraokeButton, WM_SETFONT, (WPARAM)hFont, true);
	
	
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


