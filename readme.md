A WAV player that has basic functionality such as play, stop, and pause as well as rudimentary vocal removal.    
I'm hoping to eventually read up on more advanced audio processing techniques like pitch and time scaling and attempt to implement those too.    
    
Right now the application has a pitch shifting example that's hard-coded (so a parameter like how much to pitch shift by can't be changed at the moment by the user) thanks to Olli Parviainen's SoundTouch library (https://codeberg.org/soundtouch/soundtouch/src/branch/master/source). The time it takes to pitch shift audio data depends on how large the data is.
Also thanks to Stephan Bernsee's code (http://blogs.zynaptiq.com/bernsee/pitch-shifting-using-the-ft/), which I used initially for the pitch shifting feature.    
    
The visualization part kinda looks almost right but is likely not accurate and probably using incorrect data. I hope to eventually correct this as well :)    
    
![basic wav player screenshot](screenshot.png "basic wav player")    