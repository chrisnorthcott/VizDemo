# VizDemo #

## Visualisation demo using libavcodec (FFMPEG), libao, FFTW3 and SDL2 ##

An exercise in building a music visualiser using the above. The code is well commented so it should be easy enough to use this as an example project to lift code from.

Licensed under CC0 (public domain).

[Video demo here.](http://leethax.xyz/demo.webm)

### Building the demo ###

*Debian/Ubuntu*

```
apt-get install libfftw3-dev libsdl2-dev libsdl2-ttf-dev libao-dev libavcodec-dev libavutil-dev libavformat-dev build-essential
gcc vizdemo.c -o vizdemo -Wall -lm -lSDL2 -lSDL2_ttf -lfftw3 -lao -lavcodec -lavutil -lavformat
```

*Arch*

```
pacman -S fftw sdl2 sdl2_ttf ffmpeg libao
gcc vizdemo.c -o vizdemo -Wall -lm -lSDL2 -lSDL2_ttf -lfftw3 -lao -lavcodec -lavutil -lavformat
```

*Other distros/Windows/OSX*

Sorry, you're on your own.

### Usage ###

vizdemo [file to play]

### Supported formats ###

Anything that FFMPEG (/VLC) supports that contains an audio stream. Tested with MP3s and FLACs and it seems to work okay.

### Known issues ###

* Empty FFT bins scale to +infinity
* Will occasionally segfault if ffmpeg can't load the entire file in one go.


